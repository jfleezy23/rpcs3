#include "stdafx.h"
#include "Emu/IdManager.h"
#include "Emu/perf_meter.hpp"
#include "Emu/Cell/PPUModule.h"
#include "Emu/Cell/lv2/sys_sync.h"
#include "Emu/Cell/lv2/sys_ppu_thread.h"
#include "Emu/Cell/lv2/sys_process.h"
#include "Emu/savestate_utils.hpp"
#include "sysPrxForUser.h"
#include "util/media_utils.h"
#include "util/sysinfo.hpp"

#ifdef _MSC_VER
#pragma warning(push, 0)
#else
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wold-style-cast"
#endif
extern "C"
{
#include "libavcodec/avcodec.h"
#include "libavfilter/avfilter.h"
#include "libavfilter/buffersink.h"
#include "libavfilter/buffersrc.h"
#include "libavutil/imgutils.h"
#include "libavutil/opt.h"
#include "libswscale/swscale.h"
}
#ifdef _MSC_VER
#pragma warning(pop)
#else
#pragma GCC diagnostic pop
#endif

#include "cellPamf.h"
#include "cellVdec.h"

#include <atomic>
#include <string>
#include <mutex>
#include <queue>
#include <cmath>
#include "Utilities/lockless.h"
#include <variant>
#include "util/asm.hpp"

std::mutex g_mutex_avcodec_open2;

LOG_CHANNEL(cellVdec);

namespace
{
	bool is_interlaced_frame(const AVFrame* frame)
	{
#if LIBAVUTIL_VERSION_INT < AV_VERSION_INT(58, 29, 100)
		return frame && frame->interlaced_frame != 0;
#else
		return frame && !!(frame->flags & AV_FRAME_FLAG_INTERLACED);
#endif
	}

	bool is_top_field_first_frame(const AVFrame* frame)
	{
#if LIBAVUTIL_VERSION_INT < AV_VERSION_INT(58, 29, 100)
		return frame && frame->top_field_first != 0;
#else
		return frame && !!(frame->flags & AV_FRAME_FLAG_TOP_FIELD_FIRST);
#endif
	}

	void mark_progressive_frame(AVFrame* frame)
	{
		if (!frame)
		{
			return;
		}

#if LIBAVUTIL_VERSION_INT < AV_VERSION_INT(58, 29, 100)
		frame->interlaced_frame = 0;
		frame->top_field_first = 0;
#else
		frame->flags &= ~AV_FRAME_FLAG_INTERLACED;
		frame->flags &= ~AV_FRAME_FLAG_TOP_FIELD_FIRST;
#endif
	}

	AVRational framerate_code_to_rational(u32 frc)
	{
		switch (frc)
		{
		case CELL_VDEC_FRC_24000DIV1001: return { 24000, 1001 };
		case CELL_VDEC_FRC_24: return { 24, 1 };
		case CELL_VDEC_FRC_25: return { 25, 1 };
		case CELL_VDEC_FRC_30000DIV1001: return { 30000, 1001 };
		case CELL_VDEC_FRC_30: return { 30, 1 };
		case CELL_VDEC_FRC_50: return { 50, 1 };
		case CELL_VDEC_FRC_60000DIV1001: return { 60000, 1001 };
		case CELL_VDEC_FRC_60: return { 60, 1 };
		default: return { 0, 1 };
		}
	}

	u32 doubled_framerate_code(u32 frc)
	{
		switch (frc)
		{
		case CELL_VDEC_FRC_25: return CELL_VDEC_FRC_50;
		case CELL_VDEC_FRC_30000DIV1001: return CELL_VDEC_FRC_60000DIV1001;
		case CELL_VDEC_FRC_30: return CELL_VDEC_FRC_60;
		default: return 0;
		}
	}

	bool should_use_host_mpeg2_filter(u32 codec_type, const AVFrame* frame, u32 frc)
	{
		return codec_type == CELL_VDEC_CODEC_TYPE_MPEG2 && is_interlaced_frame(frame) && doubled_framerate_code(frc) != 0;
	}

	struct host_filter_state
	{
		AVFilterGraph* graph{};
		AVFilterContext* src{};
		AVFilterContext* sink{};
		AVRational input_time_base{};
		AVRational output_time_base{};
		u32 output_frc{};
		s64 next_input_pts{};
		u64 pts_origin{};
		std::string active_filter_name;
		u64 queued_inputs{};
		u64 emitted_outputs{};
		u64 last_seq_id{};
		u64 last_cmd_id{};
		u64 last_userdata{};
		CellVdecPicAttr last_attr = CELL_VDEC_PICITEM_ATTR_NORMAL;
		bool warned_no_output{};
		bool failed{};

		void reset()
		{
			avfilter_graph_free(&graph);
			src = nullptr;
			sink = nullptr;
			input_time_base = {};
			output_time_base = {};
			output_frc = 0;
			next_input_pts = 0;
			pts_origin = 0;
			active_filter_name.clear();
			queued_inputs = 0;
			emitted_outputs = 0;
			last_seq_id = 0;
			last_cmd_id = 0;
			last_userdata = 0;
			last_attr = CELL_VDEC_PICITEM_ATTR_NORMAL;
			warned_no_output = false;
			failed = false;
		}
	};

	void deinterlace_plane_in_place(u8* data, int pitch, int width_bytes, int height, int preserved_parity)
	{
		if (!data || pitch <= 0 || width_bytes <= 0 || height <= 1)
		{
			return;
		}

		for (int y = 0; y < height; y++)
		{
			if ((y & 1) == preserved_parity)
			{
				continue;
			}

			u8* const dst = data + y * pitch;
			const int prev_y = y > 0 ? y - 1 : y + 1;
			const int next_y = y + 1 < height ? y + 1 : y - 1;
			const u8* const prev = data + prev_y * pitch;
			const u8* const next = data + next_y * pitch;

			if (prev_y == next_y)
			{
				std::memcpy(dst, prev, width_bytes);
				continue;
			}

			for (int x = 0; x < width_bytes; x++)
			{
				dst[x] = static_cast<u8>((static_cast<u16>(prev[x]) + static_cast<u16>(next[x]) + 1) / 2);
			}
		}
	}

	void deinterlace_output_frame(CellVdecPicFormatType format_type, u8* const out_data[4], const int out_line[4], int width, int height, bool top_field_first)
	{
		const int preserved_parity = top_field_first ? 0 : 1;

		switch (format_type)
		{
		case CELL_VDEC_PICFMT_ARGB32_ILV:
		case CELL_VDEC_PICFMT_RGBA32_ILV:
			deinterlace_plane_in_place(out_data[0], out_line[0], width * 4, height, preserved_parity);
			break;
		case CELL_VDEC_PICFMT_UYVY422_ILV:
			deinterlace_plane_in_place(out_data[0], out_line[0], width * 2, height, preserved_parity);
			break;
		case CELL_VDEC_PICFMT_YUV420_PLANAR:
			deinterlace_plane_in_place(out_data[0], out_line[0], width, height, preserved_parity);
			deinterlace_plane_in_place(out_data[1], out_line[1], std::max(1, width / 2), std::max(1, height / 2), preserved_parity);
			deinterlace_plane_in_place(out_data[2], out_line[2], std::max(1, width / 2), std::max(1, height / 2), preserved_parity);
			break;
		default:
			break;
		}
	}
}

template<>
void fmt_class_string<CellVdecError>::format(std::string& out, u64 arg)
{
	format_enum(out, arg, [](auto error)
	{
		switch (error)
		{
			STR_CASE(CELL_VDEC_ERROR_ARG);
			STR_CASE(CELL_VDEC_ERROR_SEQ);
			STR_CASE(CELL_VDEC_ERROR_BUSY);
			STR_CASE(CELL_VDEC_ERROR_EMPTY);
			STR_CASE(CELL_VDEC_ERROR_AU);
			STR_CASE(CELL_VDEC_ERROR_PIC);
			STR_CASE(CELL_VDEC_ERROR_FATAL);
		}

		return unknown;
	});
}

// The general sequence control flow has these possible transitions:
// closed -> dormant
// dormant -> ready
// dormant -> closed
// ready -> ending
// ready -> resetting
// ready -> closed
// ending -> dormant
// resetting -> ready
enum class sequence_state : u32
{
	closed    = 0, // Also called non-existent. Needs to be opened before anything can be done with it.
	dormant   = 1, // Waiting for the next sequence. The last picture and pic-item can be aqcuired in this state.
	ready     = 2, // Ready for decoding. Can also restart sequences in this state.
	ending    = 3, // Ending a sequence. Goes to dormant afterwards.
	resetting = 4, // Stops the current sequence and starts a new one. The pictures of the old sequence are flushed
	invalid   = 5, // Any other value is invalid
};

template<>
void fmt_class_string<sequence_state>::format(std::string& out, u64 arg)
{
	format_enum(out, arg, [](auto error)
	{
		switch (error)
		{
			STR_CASE(sequence_state::closed);
			STR_CASE(sequence_state::dormant);
			STR_CASE(sequence_state::ready);
			STR_CASE(sequence_state::ending);
			STR_CASE(sequence_state::resetting);
			STR_CASE(sequence_state::invalid);
		}

		return unknown;
	});
}

vm::gvar<s32> _cell_vdec_prx_ver; // TODO: this should probably specify the VDEC module that was loaded. E.g. CELL_SYSMODULE_VDEC_MPEG2

enum class vdec_cmd_type : u32
{
	start_sequence,
	end_sequence,
	close,
	au_decode,
	framerate,
};

struct vdec_cmd
{
	explicit vdec_cmd(vdec_cmd_type _type, u64 _seq_id, u64 _id)
		: type(_type), seq_id(_seq_id), id(_id)
	{
		ensure(_type != vdec_cmd_type::au_decode);
		ensure(_type != vdec_cmd_type::framerate);
	}

	explicit vdec_cmd(vdec_cmd_type _type, u64 _seq_id, u64 _id, s32 _mode, CellVdecAuInfo _au)
		: type(_type), seq_id(_seq_id), id(_id), mode(_mode), au(std::move(_au))
	{
		ensure(_type == vdec_cmd_type::au_decode);
	}

	explicit vdec_cmd(vdec_cmd_type _type, u64 _seq_id, u64 _id, s32 _framerate)
		: type(_type), seq_id(_seq_id), id(_id), framerate(_framerate)
	{
		ensure(_type == vdec_cmd_type::framerate);
	}

	vdec_cmd_type type{};
	u64 seq_id{};
	u64 id{};
	s32 mode{};
	s32 framerate{};
	CellVdecAuInfo au{};
};

struct vdec_frame
{
	struct frame_dtor
	{
		void operator()(AVFrame* data) const
		{
			av_frame_unref(data);
			av_frame_free(&data);
		}
	};

	u64 seq_id{};
	u64 cmd_id{};

	std::unique_ptr<AVFrame, frame_dtor> avf;
	u64 dts{};
	u64 pts{};
	u64 userdata{};
	u32 frc{};
	bool pic_item_received = false;
	CellVdecPicAttr attr = CELL_VDEC_PICITEM_ATTR_NORMAL;

	AVFrame* operator ->() const
	{
		return avf.get();
	}
};

	struct vdec_context final
	{
	static const u32 id_base = 0xf0000000;
	static const u32 id_step = 0x00000100;
	static const u32 id_count = 1024;
	SAVESTATE_INIT_POS(24);

	u32 handle = 0;

	atomic_t<u64> seq_id = 0; // The first sequence will have the ID 1
	atomic_t<u64> next_cmd_id = 0;
	atomic_t<bool> abort_decode = false; // Used for thread interaction
	atomic_t<bool> is_running = false;   // Used for thread interaction
	atomic_t<sequence_state> seq_state = sequence_state::closed;

	const AVCodec* codec{};
	const AVCodecDescriptor* codec_desc{};
	AVCodecContext* ctx{};
	SwsContext* sws{};
	host_filter_state mpeg2_filter{};

	shared_mutex mutex; // Used for 'out' queue (TODO)

	const u32 type;
	const u32 mem_addr;
	const u32 mem_size;
	const vm::ptr<CellVdecCbMsg> cb_func;
	const u32 cb_arg;
	u32 mem_bias{};

	u32 frc_set{}; // Frame Rate Override
	u64 next_pts{};
	u64 next_dts{};
	atomic_t<u32> ppu_tid{};

	std::deque<vdec_frame> out_queue;
	const u32 out_max = 60;

	u32 get_output_queue_limit() const
	{
		// Host-enhanced MPEG-2 can legitimately emit twice as many pictures as the
		// original stream, so give it more buffering room to avoid pacing the decoder
		// against the consumer too aggressively.
		if (type == CELL_VDEC_CODEC_TYPE_MPEG2 && mpeg2_filter.graph && mpeg2_filter.output_frc >= CELL_VDEC_FRC_50)
		{
			return out_max * 4;
		}

		return out_max;
	}

	atomic_t<s32> au_count{0};

	lf_queue<vdec_cmd> in_cmd;

	AVRational log_time_base{}; // Used to reduce log spam
	AVRational log_framerate{}; // Used to reduce log spam

	vdec_context(s32 type, u32 /*profile*/, u32 addr, u32 size, vm::ptr<CellVdecCbMsg> func, u32 arg)
		: type(type)
		, mem_addr(addr)
		, mem_size(size)
		, cb_func(func)
		, cb_arg(arg)
	{
		switch (type)
		{
		case CELL_VDEC_CODEC_TYPE_MPEG2:
		{
			codec = avcodec_find_decoder(AV_CODEC_ID_MPEG2VIDEO);
			break;
		}
		case CELL_VDEC_CODEC_TYPE_AVC:
		{
			codec = avcodec_find_decoder(AV_CODEC_ID_H264);
			break;
		}
		case CELL_VDEC_CODEC_TYPE_MPEG4:
		case CELL_VDEC_CODEC_TYPE_DIVX:
		{
			codec = avcodec_find_decoder(AV_CODEC_ID_MPEG4);
			break;
		}
		default:
		{
			fmt::throw_exception("Unknown video decoder type (0x%x)", type);
		}
		}

		if (!codec)
		{
			fmt::throw_exception("avcodec_find_decoder() failed (type=0x%x)", type);
		}

		codec_desc = avcodec_descriptor_get(codec->id);

		if (!codec_desc)
		{
			fmt::throw_exception("avcodec_descriptor_get() failed (type=0x%x)", type);
		}

		ctx = avcodec_alloc_context3(codec);

		if (!ctx)
		{
			fmt::throw_exception("avcodec_alloc_context3() failed (type=0x%x)", type);
		}

		AVDictionary* opts = nullptr;

		// Give prerendered cutscene decode more headroom on wide CPUs, but keep the
		// MPEG-2 path low-latency by preferring slice threading over deeper frame
		// queues. Other codecs stay on the older mixed frame/slice budget.
		ctx->thread_count = type == CELL_VDEC_CODEC_TYPE_MPEG2
			? std::clamp<u32>((utils::get_thread_count() * 2) / 3, 8, 20)
			: std::clamp<u32>(utils::get_thread_count() / 4, 2, 8);
		ctx->thread_type = type == CELL_VDEC_CODEC_TYPE_MPEG2
			? FF_THREAD_SLICE
			: FF_THREAD_FRAME | FF_THREAD_SLICE;

		std::lock_guard lock(g_mutex_avcodec_open2);

		int err = avcodec_open2(ctx, codec, &opts);
		if (err || opts)
		{
			avcodec_free_context(&ctx);
			std::string dict_content;
			if (opts)
			{
				AVDictionaryEntry* tag = nullptr;
				while ((tag = av_dict_get(opts, "", tag, AV_DICT_IGNORE_SUFFIX)))
				{
					fmt::append(dict_content, "['%s': '%s']", tag->key, tag->value);
				}
			}
			fmt::throw_exception("avcodec_open2() failed (err=0x%x='%s', opts=%s)", err, utils::av_error_to_string(err), dict_content);
		}

		av_dict_free(&opts);

		seq_state = sequence_state::dormant;
	}

	~vdec_context()
	{
		mpeg2_filter.reset();
		avcodec_free_context(&ctx);
		sws_freeContext(sws);
	}

	static u32 freq_to_framerate_code(f64 freq)
	{
		if (std::abs(freq - 23.976) < 0.002) return CELL_VDEC_FRC_24000DIV1001;
		if (std::abs(freq - 24.000) < 0.001) return CELL_VDEC_FRC_24;
		if (std::abs(freq - 25.000) < 0.001) return CELL_VDEC_FRC_25;
		if (std::abs(freq - 29.970) < 0.002) return CELL_VDEC_FRC_30000DIV1001;
		if (std::abs(freq - 30.000) < 0.001) return CELL_VDEC_FRC_30;
		if (std::abs(freq - 50.000) < 0.001) return CELL_VDEC_FRC_50;
		if (std::abs(freq - 59.940) < 0.002) return CELL_VDEC_FRC_60000DIV1001;
		if (std::abs(freq - 60.000) < 0.001) return CELL_VDEC_FRC_60;
		return 0;
	}

	bool init_mpeg2_filter(const AVFrame* frame, u32 input_frc, u64 pts_origin)
	{
		mpeg2_filter.reset();

		if (!should_use_host_mpeg2_filter(type, frame, input_frc))
		{
			return false;
		}

		const AVRational input_fps = framerate_code_to_rational(input_frc);

		if (!input_fps.num || !input_fps.den)
		{
			return false;
		}

		const AVRational sample_aspect = frame->sample_aspect_ratio.num && frame->sample_aspect_ratio.den ? frame->sample_aspect_ratio : AVRational{ 1, 1 };
		const AVFilter* const buffersrc = avfilter_get_by_name("buffer");
		const AVFilter* const buffersink = avfilter_get_by_name("buffersink");

		if (!buffersrc || !buffersink)
		{
			cellVdec.warning("Host MPEG-2 enhancement filters are unavailable. Falling back to the standard decode path.");
			mpeg2_filter.failed = true;
			return false;
		}

		mpeg2_filter.input_time_base = av_inv_q(input_fps);
		mpeg2_filter.output_time_base = av_mul_q(mpeg2_filter.input_time_base, AVRational{ 1, 2 });
		mpeg2_filter.output_frc = doubled_framerate_code(input_frc);
		mpeg2_filter.pts_origin = pts_origin;

		char args[256]{};
		std::snprintf(args, sizeof(args), "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
			frame->width, frame->height, frame->format,
			mpeg2_filter.input_time_base.num, mpeg2_filter.input_time_base.den,
			sample_aspect.num, sample_aspect.den);

		const AVPixelFormat pix_fmts[] = { static_cast<AVPixelFormat>(frame->format), AV_PIX_FMT_NONE };
		const bool has_libplacebo = avfilter_get_by_name("libplacebo") != nullptr;
		const bool has_bwdif = avfilter_get_by_name("bwdif") != nullptr;
		const bool has_yadif = avfilter_get_by_name("yadif") != nullptr;
		const AVRational output_fps = framerate_code_to_rational(mpeg2_filter.output_frc);
		char fps_arg[32]{};
		std::snprintf(fps_arg, sizeof(fps_arg), "%d/%d", output_fps.num, output_fps.den);

		const auto try_filter = [&](const char* filter_name, const std::string& filter_desc) -> bool
		{
			mpeg2_filter.graph = avfilter_graph_alloc();

			if (!mpeg2_filter.graph)
			{
				cellVdec.warning("Failed to allocate the host MPEG-2 enhancement graph. Falling back to the standard decode path.");
				return false;
			}

			if (const int ret = avfilter_graph_create_filter(&mpeg2_filter.src, buffersrc, "mgs4_mpeg2_src", args, nullptr, mpeg2_filter.graph); ret < 0)
			{
				cellVdec.warning("Failed to create the host MPEG-2 source filter for '%s' (error=0x%x): %s", filter_name, ret, utils::av_error_to_string(ret));
				mpeg2_filter.reset();
				return false;
			}

			if (const int ret = avfilter_graph_create_filter(&mpeg2_filter.sink, buffersink, "mgs4_mpeg2_sink", nullptr, nullptr, mpeg2_filter.graph); ret < 0)
			{
				cellVdec.warning("Failed to create the host MPEG-2 sink filter for '%s' (error=0x%x): %s", filter_name, ret, utils::av_error_to_string(ret));
				mpeg2_filter.reset();
				return false;
			}

			if (const int ret = av_opt_set_int_list(mpeg2_filter.sink, "pix_fmts", pix_fmts, AV_PIX_FMT_NONE, AV_OPT_SEARCH_CHILDREN); ret < 0)
			{
				cellVdec.warning("Failed to constrain the host MPEG-2 sink pixel format for '%s' (error=0x%x): %s", filter_name, ret, utils::av_error_to_string(ret));
				mpeg2_filter.reset();
				return false;
			}

			AVFilterInOut* outputs = avfilter_inout_alloc();
			AVFilterInOut* inputs = avfilter_inout_alloc();

			if (!outputs || !inputs)
			{
				avfilter_inout_free(&outputs);
				avfilter_inout_free(&inputs);
				cellVdec.warning("Failed to allocate the host MPEG-2 filter graph endpoints for '%s'. Falling back to the standard decode path.", filter_name);
				mpeg2_filter.reset();
				return false;
			}

			outputs->name = av_strdup("in");
			outputs->filter_ctx = mpeg2_filter.src;
			outputs->pad_idx = 0;
			outputs->next = nullptr;

			inputs->name = av_strdup("out");
			inputs->filter_ctx = mpeg2_filter.sink;
			inputs->pad_idx = 0;
			inputs->next = nullptr;

			const int parse_ret = avfilter_graph_parse_ptr(mpeg2_filter.graph, filter_desc.c_str(), &inputs, &outputs, nullptr);
			avfilter_inout_free(&inputs);
			avfilter_inout_free(&outputs);

			if (parse_ret < 0)
			{
				cellVdec.notice("Host MPEG-2 enhancement variant '%s' was rejected (error=0x%x): %s", filter_name, parse_ret, utils::av_error_to_string(parse_ret));
				mpeg2_filter.reset();
				return false;
			}

			if (const int ret = avfilter_graph_config(mpeg2_filter.graph, nullptr); ret < 0)
			{
				cellVdec.notice("Host MPEG-2 enhancement variant '%s' could not be configured (error=0x%x): %s", filter_name, ret, utils::av_error_to_string(ret));
				mpeg2_filter.reset();
				return false;
			}

			if (const AVRational sink_time_base = av_buffersink_get_time_base(mpeg2_filter.sink); sink_time_base.num && sink_time_base.den)
			{
				mpeg2_filter.output_time_base = sink_time_base;
			}

			if (const AVRational sink_frame_rate = av_buffersink_get_frame_rate(mpeg2_filter.sink); sink_frame_rate.num && sink_frame_rate.den)
			{
				if (const u32 sink_frc = freq_to_framerate_code(av_q2d(sink_frame_rate)); sink_frc)
				{
					mpeg2_filter.output_frc = sink_frc;
				}
			}

			mpeg2_filter.active_filter_name = filter_name;
			cellVdec.notice("Host MPEG-2 enhancement path enabled with '%s' at %d/%d fps (time_base=%d/%d).", filter_name, output_fps.num, output_fps.den, mpeg2_filter.output_time_base.num, mpeg2_filter.output_time_base.den);
			return true;
		};

		if (has_libplacebo)
		{
			std::string filter_desc = "libplacebo=deinterlace=bob";
			filter_desc += ":send_fields=1";
			filter_desc += ":fps=";
			filter_desc += fps_arg;
			filter_desc += ":frame_mixer=oversample";
			filter_desc += ":deband=1";
			filter_desc += ":deband_iterations=1";
			filter_desc += ":deband_radius=4";
			filter_desc += ":deband_threshold=3";
			filter_desc += ":deband_grain=2";

			if (try_filter("libplacebo-bob-smooth", filter_desc))
			{
				return true;
			}

			filter_desc = "libplacebo=deinterlace=yadif";
			filter_desc += ":skip_spatial_check=1";
			filter_desc += ":send_fields=1";
			filter_desc += ":fps=";
			filter_desc += fps_arg;
			filter_desc += ":frame_mixer=mitchell_clamp";
			filter_desc += ":deband=1";
			filter_desc += ":deband_iterations=2";
			filter_desc += ":deband_radius=6";
			filter_desc += ":deband_threshold=4";
			filter_desc += ":deband_grain=3";

			if (try_filter("libplacebo-yadif-smooth", filter_desc))
			{
				return true;
			}
		}

		if ((has_bwdif || has_yadif) && has_libplacebo)
		{
			std::string filter_desc = has_bwdif
				? "bwdif=mode=send_field:parity=auto:deint=interlaced,libplacebo=fps="
				: "yadif=mode=send_field:parity=auto:deint=interlaced,libplacebo=fps=";
			filter_desc += fps_arg;
			filter_desc += ":frame_mixer=mitchell_clamp";
			filter_desc += ":upscaler=ewa_lanczos";
			filter_desc += ":downscaler=ewa_lanczos";
			filter_desc += ":deband=1";
			filter_desc += ":deband_iterations=3";
			filter_desc += ":deband_radius=8";
			filter_desc += ":deband_threshold=6";
			filter_desc += ":deband_grain=4";

			if (try_filter(has_bwdif ? "bwdif+libplacebo-hq" : "yadif+libplacebo-hq", filter_desc))
			{
				return true;
			}
		}

		if (has_bwdif && try_filter("bwdif", "bwdif=mode=send_field:parity=auto:deint=interlaced"))
		{
			return true;
		}

		if (has_yadif && try_filter("yadif", "yadif=mode=send_field:parity=auto:deint=interlaced"))
		{
			return true;
		}

		cellVdec.warning("Host MPEG-2 enhancement filters are unavailable. libplacebo/bwdif/yadif variants all failed or were missing in this FFmpeg build.");
		mpeg2_filter.reset();
		mpeg2_filter.failed = true;
		return false;
	}

	bool drain_filtered_mpeg2_frames(std::deque<vdec_frame>& decoded_frames)
	{
		bool emitted_frame = false;

		while (true)
		{
			vdec_frame filtered_frame;
			filtered_frame.seq_id = mpeg2_filter.last_seq_id;
			filtered_frame.cmd_id = mpeg2_filter.last_cmd_id;
			filtered_frame.userdata = mpeg2_filter.last_userdata;
			filtered_frame.attr = mpeg2_filter.last_attr;
			filtered_frame.frc = mpeg2_filter.output_frc;
			filtered_frame.avf.reset(av_frame_alloc());

			if (!filtered_frame.avf)
			{
				cellVdec.warning("Failed to allocate an MPEG-2 output frame for the host enhancement path.");
				mpeg2_filter.reset();
				mpeg2_filter.failed = true;
				return false;
			}

			const int ret = av_buffersink_get_frame(mpeg2_filter.sink, filtered_frame.avf.get());

			if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
			{
				break;
			}

			if (ret < 0)
			{
				cellVdec.warning("Failed to drain an MPEG-2 frame from the host enhancement graph (error=0x%x): %s", ret, utils::av_error_to_string(ret));
				mpeg2_filter.reset();
				mpeg2_filter.failed = true;
				return false;
			}

			mark_progressive_frame(filtered_frame.avf.get());

			const s64 filtered_pts = filtered_frame->pts != AV_NOPTS_VALUE ? filtered_frame->pts : 0;
			const u64 pts_90k = mpeg2_filter.pts_origin + av_rescale_q(filtered_pts, mpeg2_filter.output_time_base, AVRational{ 1, 90000 });

			filtered_frame.pts = pts_90k;
			filtered_frame.dts = pts_90k;

			mpeg2_filter.emitted_outputs++;
			cellVdec.trace("Got host-filtered MPEG-2 picture (handle=0x%x, seq_id=%d, cmd_id=%d, pts=0x%llx, filter_pts=%lld)", handle, filtered_frame.seq_id, filtered_frame.cmd_id, filtered_frame.pts, filtered_pts);
			decoded_frames.push_back(std::move(filtered_frame));
			emitted_frame = true;
		}

		return emitted_frame;
	}

	bool flush_filtered_mpeg2_frames(std::deque<vdec_frame>& decoded_frames)
	{
		if (!mpeg2_filter.graph || !mpeg2_filter.src || !mpeg2_filter.sink)
		{
			return false;
		}

		const int ret = av_buffersrc_add_frame_flags(mpeg2_filter.src, nullptr, 0);

		if (ret < 0 && ret != AVERROR_EOF)
		{
			cellVdec.warning("Failed to flush the host MPEG-2 enhancement graph (error=0x%x): %s", ret, utils::av_error_to_string(ret));
			mpeg2_filter.reset();
			mpeg2_filter.failed = true;
			return false;
		}

		return drain_filtered_mpeg2_frames(decoded_frames);
	}

	bool enqueue_filtered_mpeg2_frames(std::deque<vdec_frame>& decoded_frames, const vdec_frame& source_frame)
	{
		if (mpeg2_filter.failed)
		{
			return false;
		}

		if (!mpeg2_filter.graph && !init_mpeg2_filter(source_frame.avf.get(), source_frame.frc, source_frame.pts))
		{
			return false;
		}

		if (!mpeg2_filter.graph)
		{
			return false;
		}

		mpeg2_filter.last_seq_id = source_frame.seq_id;
		mpeg2_filter.last_cmd_id = source_frame.cmd_id;
		mpeg2_filter.last_userdata = source_frame.userdata;
		mpeg2_filter.last_attr = source_frame.attr;

		AVFrame* filter_input = av_frame_clone(source_frame.avf.get());

		if (!filter_input)
		{
			cellVdec.warning("Failed to clone an MPEG-2 frame for the host enhancement path. Falling back to the standard decode path.");
			mpeg2_filter.reset();
			mpeg2_filter.failed = true;
			return false;
		}

		filter_input->pts = mpeg2_filter.next_input_pts++;
		// The newer libplacebo send_fields path derives the second field timestamp from
		// AVFrame::duration, so make sure short MPEG-2 clips don't enter the graph with
		// a zero-duration frame and collapse both fields onto the same output PTS.
		filter_input->duration = filter_input->duration > 0 ? filter_input->duration : 1;
		mpeg2_filter.queued_inputs++;
		const int add_ret = av_buffersrc_add_frame_flags(mpeg2_filter.src, filter_input, AV_BUFFERSRC_FLAG_KEEP_REF);
		av_frame_free(&filter_input);

		if (add_ret < 0)
		{
			cellVdec.warning("Failed to queue an MPEG-2 frame into the host enhancement graph (error=0x%x): %s", add_ret, utils::av_error_to_string(add_ret));
			mpeg2_filter.reset();
			mpeg2_filter.failed = true;
			return false;
		}

		const bool emitted_frame = drain_filtered_mpeg2_frames(decoded_frames);

		if (!emitted_frame && mpeg2_filter.emitted_outputs == 0 && mpeg2_filter.queued_inputs >= 4 && !mpeg2_filter.warned_no_output)
		{
			cellVdec.notice("Host MPEG-2 enhancement path '%s' has queued %llu input frame(s) without producing output yet.", mpeg2_filter.active_filter_name, mpeg2_filter.queued_inputs);
			mpeg2_filter.warned_no_output = true;
		}

		return emitted_frame;
	}

	void exec(ppu_thread& ppu, u32 vid)
	{
		perf_meter<"VDEC"_u32> perf0;

		ppu_tid.release(ppu.id);

		for (auto slice = in_cmd.pop_all(); thread_ctrl::state() != thread_state::aborting; [&]
		{
			if (slice)
			{
				slice.pop_front();
			}

			if (slice || thread_ctrl::state() == thread_state::aborting)
			{
				return;
			}

			thread_ctrl::wait_on(in_cmd);
			slice = in_cmd.pop_all(); // Pop new command list
		}())
		{
			// pcmd can be nullptr
			auto* cmd = slice.get();

			if (!cmd)
			{
				continue;
			}

			switch (cmd->type)
			{
			case vdec_cmd_type::start_sequence:
			{
				std::lock_guard lock{mutex};

				if (seq_state == sequence_state::resetting)
				{
					cellVdec.trace("Reset sequence... (handle=0x%x, seq_id=%d, cmd_id=%d)", handle, cmd->seq_id, cmd->id);
				}
				else
				{
					cellVdec.trace("Start sequence... (handle=0x%x, seq_id=%d, cmd_id=%d)", handle, cmd->seq_id, cmd->id);
				}

				avcodec_flush_buffers(ctx);

				out_queue.clear(); // Flush image queue
				log_time_base = {};
				log_framerate = {};
				mpeg2_filter.reset();

				frc_set = 0; // TODO: ???
				next_pts = 0;
				next_dts = 0;

				abort_decode = false;
				is_running = true;
				break;
			}
			case vdec_cmd_type::end_sequence:
			{
				cellVdec.trace("End sequence... (handle=0x%x, seq_id=%d, cmd_id=%d)", handle, cmd->seq_id, cmd->id);

				std::deque<vdec_frame> flushed_frames;
				flush_filtered_mpeg2_frames(flushed_frames);

				while (!flushed_frames.empty() && seq_id == cmd->seq_id)
				{
					u32 elapsed = 0;
					while (thread_ctrl::state() != thread_state::aborting && !abort_decode && seq_id == cmd->seq_id)
					{
						{
							std::lock_guard lock{mutex};

							if (out_queue.size() <= get_output_queue_limit())
							{
								break;
							}
						}

						thread_ctrl::wait_for(10000);

						if (elapsed++ >= 500)
						{
							cellVdec.error("Video end-sequence flush has been waiting for a consumer for 5 seconds. (handle=0x%x, seq_id=%d, cmd_id=%d, queue_size=%d, queue_limit=%d)", handle, cmd->seq_id, cmd->id, out_queue.size(), get_output_queue_limit());
							elapsed = 0;
						}
					}

					if (thread_ctrl::state() == thread_state::aborting || abort_decode || seq_id != cmd->seq_id)
					{
						break;
					}

					{
						std::lock_guard lock{mutex};
						out_queue.push_back(std::move(flushed_frames.front()));
						flushed_frames.pop_front();
					}

					cellVdec.trace("Sending CELL_VDEC_MSG_TYPE_PICOUT from end-sequence flush (handle=0x%x, seq_id=%d, cmd_id=%d)", handle, cmd->seq_id, cmd->id);
					cb_func(ppu, vid, CELL_VDEC_MSG_TYPE_PICOUT, CELL_OK, cb_arg);
					lv2_obj::sleep(ppu);
				}

				if (!mpeg2_filter.active_filter_name.empty())
				{
					cellVdec.notice("Host MPEG-2 enhancement summary for '%s': inputs=%llu, outputs=%llu.", mpeg2_filter.active_filter_name, mpeg2_filter.queued_inputs, mpeg2_filter.emitted_outputs);
				}

				mpeg2_filter.reset();

				{
					std::lock_guard lock{mutex};
					seq_state = sequence_state::dormant;
				}

				cellVdec.trace("Sending CELL_VDEC_MSG_TYPE_SEQDONE (handle=0x%x, seq_id=%d, cmd_id=%d)", handle, cmd->seq_id, cmd->id);
				cb_func(ppu, vid, CELL_VDEC_MSG_TYPE_SEQDONE, CELL_OK, cb_arg);
				lv2_obj::sleep(ppu);
				break;
			}
			case vdec_cmd_type::au_decode:
			{
				AVPacket packet{};
				packet.pos = -1;

				u64 au_usrd{};

				const u32 au_mode = cmd->mode;
				const u32 au_addr = cmd->au.startAddr;
				const u32 au_size = cmd->au.size;
				const u64 au_pts = u64{cmd->au.pts.upper} << 32 | cmd->au.pts.lower;
				const u64 au_dts = u64{cmd->au.dts.upper} << 32 | cmd->au.dts.lower;
				au_usrd = cmd->au.userData;

				packet.data = vm::_ptr<u8>(au_addr);
				packet.size = au_size;
				packet.pts = au_pts != umax ? au_pts : s64{smin};
				packet.dts = au_dts != umax ? au_dts : s64{smin};

				if (next_pts == 0 && au_pts != umax)
				{
					next_pts = au_pts;
				}

				if (next_dts == 0 && au_dts != umax)
				{
					next_dts = au_dts;
				}

				const CellVdecPicAttr attr = au_mode == CELL_VDEC_DEC_MODE_NORMAL ? CELL_VDEC_PICITEM_ATTR_NORMAL : CELL_VDEC_PICITEM_ATTR_SKIPPED;

				ctx->skip_frame =
					au_mode == CELL_VDEC_DEC_MODE_NORMAL ? AVDISCARD_DEFAULT :
					au_mode == CELL_VDEC_DEC_MODE_B_SKIP ? AVDISCARD_NONREF : AVDISCARD_NONINTRA;

				std::deque<vdec_frame> decoded_frames;

				if (!abort_decode && seq_id == cmd->seq_id)
				{
					cellVdec.trace("AU decoding: handle=0x%x, seq_id=%d, cmd_id=%d, size=0x%x, pts=0x%llx, dts=0x%llx, userdata=0x%llx", handle, cmd->seq_id, cmd->id, au_size, au_pts, au_dts, au_usrd);

					if (int ret = avcodec_send_packet(ctx, &packet); ret < 0)
					{
						fmt::throw_exception("AU queuing error (handle=0x%x, seq_id=%d, cmd_id=%d, error=0x%x): %s", handle, cmd->seq_id, cmd->id, ret, utils::av_error_to_string(ret));
					}

					while (!abort_decode && seq_id == cmd->seq_id)
					{
						// Keep receiving frames
						vdec_frame frame;
						frame.seq_id = cmd->seq_id;
						frame.cmd_id = cmd->id;
						frame.avf.reset(av_frame_alloc());

						if (!frame.avf)
						{
							fmt::throw_exception("av_frame_alloc() failed (handle=0x%x, seq_id=%d, cmd_id=%d)", handle, cmd->seq_id, cmd->id);
						}

						if (int ret = avcodec_receive_frame(ctx, frame.avf.get()); ret < 0)
						{
							if (ret == AVERROR(EAGAIN) || ret == AVERROR(EOF))
							{
								break;
							}

							fmt::throw_exception("AU decoding error (handle=0x%x, seq_id=%d, cmd_id=%d, error=0x%x): %s", handle, cmd->seq_id, cmd->id, ret, utils::av_error_to_string(ret));
						}

	#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(60, 31, 102)
						const int ticks_per_frame = ctx->ticks_per_frame;
	#else
						const int ticks_per_frame = (codec_desc->props & AV_CODEC_PROP_FIELDS) ? 2 : 1;
	#endif

						const bool is_interlaced = is_interlaced_frame(frame.avf.get());

						if (is_interlaced)
						{
							static std::atomic<bool> s_reported_interlaced_support = false;

							if (!s_reported_interlaced_support.exchange(true))
							{
								cellVdec.notice("Interlaced video detected. Using field-aware MPEG-2 output path.");
							}
						}

						if (frame->repeat_pict)
						{
							static std::atomic<bool> s_reported_repeat_pict = false;

							if (!s_reported_repeat_pict.exchange(true))
							{
								cellVdec.notice("Repeated MPEG-2 picture flags detected. The host enhancement path will ignore them and continue.");
							}
						}

						if (frame->pts != smin)
						{
							next_pts = frame->pts;
						}

						if (frame->pkt_dts != smin)
						{
							next_dts = frame->pkt_dts;
						}

						frame.pts = next_pts;
						frame.dts = next_dts;
						frame.userdata = au_usrd;
						frame.attr = attr;

						u64 amend = 0;

						if (frc_set)
						{
							switch (frc_set)
							{
							case CELL_VDEC_FRC_24000DIV1001: amend = 1001 * 90000 / 24000; break;
							case CELL_VDEC_FRC_24: amend = 90000 / 24; break;
							case CELL_VDEC_FRC_25: amend = 90000 / 25; break;
							case CELL_VDEC_FRC_30000DIV1001: amend = 1001 * 90000 / 30000; break;
							case CELL_VDEC_FRC_30: amend = 90000 / 30; break;
							case CELL_VDEC_FRC_50: amend = 90000 / 50; break;
							case CELL_VDEC_FRC_60000DIV1001: amend = 1001 * 90000 / 60000; break;
							case CELL_VDEC_FRC_60: amend = 90000 / 60; break;
							default:
							{
								fmt::throw_exception("Invalid frame rate code set (handle=0x%x, seq_id=%d, cmd_id=%d, frc=0x%x)", handle, cmd->seq_id, cmd->id, frc_set);
							}
							}

							frame.frc = frc_set;
						}
						else if (ctx->time_base.den && ctx->time_base.num)
						{
							const auto freq = 1. * ctx->time_base.den / ctx->time_base.num / ticks_per_frame;

							frame.frc = freq_to_framerate_code(freq);
							if (frame.frc)
							{
								amend = u64{90000} * ctx->time_base.num * ticks_per_frame / ctx->time_base.den;
							}
						}
						else if (ctx->framerate.den && ctx->framerate.num)
						{
							const auto freq = ctx->framerate.num / static_cast<f64>(ctx->framerate.den);

							frame.frc = freq_to_framerate_code(freq);
							if (frame.frc)
							{
								amend = u64{90000} * ctx->framerate.den / ctx->framerate.num;
							}
						}

						if (amend == 0 || frame.frc == 0)
						{
							if (log_time_base.den != ctx->time_base.den || log_time_base.num != ctx->time_base.num || log_framerate.den != ctx->framerate.den || log_framerate.num != ctx->framerate.num)
							{
								cellVdec.error("Invalid frequency (handle=0x%x, seq_id=%d, cmd_id=%d, timebase=%d/%d, tpf=%d framerate=%d/%d)", handle, cmd->seq_id, cmd->id, ctx->time_base.num, ctx->time_base.den, ticks_per_frame, ctx->framerate.num, ctx->framerate.den);
								log_time_base = ctx->time_base;
								log_framerate = ctx->framerate;
							}

							// Hack
							amend = u64{90000} / 30;
							frame.frc = CELL_VDEC_FRC_30;
						}

						next_pts += amend;
						next_dts += amend;

						if (!enqueue_filtered_mpeg2_frames(decoded_frames, frame))
						{
							cellVdec.trace("Got picture (handle=0x%x, seq_id=%d, cmd_id=%d, pts=0x%llx[0x%llx], dts=0x%llx[0x%llx])", handle, cmd->seq_id, cmd->id, frame.pts, frame->pts, frame.dts, frame->pkt_dts);
							decoded_frames.push_back(std::move(frame));
						}
					}
				}

				if (thread_ctrl::state() != thread_state::aborting)
				{
					// Send AUDONE even if the current sequence was reset and a new sequence was started.
					cellVdec.trace("Sending CELL_VDEC_MSG_TYPE_AUDONE (handle=0x%x, seq_id=%d, cmd_id=%d)", handle, cmd->seq_id, cmd->id);
					ensure(au_count.try_dec(0));

					cb_func(ppu, vid, CELL_VDEC_MSG_TYPE_AUDONE, CELL_OK, cb_arg);
					lv2_obj::sleep(ppu);

					while (!decoded_frames.empty() && seq_id == cmd->seq_id)
					{
						// Wait until there is free space in the image queue.
						// Do this after pushing the frame to the queue. That way the game can consume the frame and we can move on.
						u32 elapsed = 0;
						while (thread_ctrl::state() != thread_state::aborting && !abort_decode && seq_id == cmd->seq_id)
						{
							{
								std::lock_guard lock{mutex};

								if (out_queue.size() <= get_output_queue_limit())
								{
									break;
								}
							}

							thread_ctrl::wait_for(10000);

							if (elapsed++ >= 500) // 5 seconds
							{
								cellVdec.error("Video au decode has been waiting for a consumer for 5 seconds. (handle=0x%x, seq_id=%d, cmd_id=%d, queue_size=%d, queue_limit=%d)", handle, cmd->seq_id, cmd->id, out_queue.size(), get_output_queue_limit());
								elapsed = 0;
							}
						}

						if (thread_ctrl::state() == thread_state::aborting || abort_decode || seq_id != cmd->seq_id)
						{
							break;
						}

						{
							std::lock_guard lock{mutex};
							out_queue.push_back(std::move(decoded_frames.front()));
							decoded_frames.pop_front();
						}

						cellVdec.trace("Sending CELL_VDEC_MSG_TYPE_PICOUT (handle=0x%x, seq_id=%d, cmd_id=%d)", handle, cmd->seq_id, cmd->id);
						cb_func(ppu, vid, CELL_VDEC_MSG_TYPE_PICOUT, CELL_OK, cb_arg);
						lv2_obj::sleep(ppu);
					}
				}

				if (abort_decode || seq_id != cmd->seq_id)
				{
					cellVdec.warning("AU decoding: aborted (handle=0x%x, seq_id=%d, cmd_id=%d, abort_decode=%d)", handle, cmd->seq_id, cmd->id, abort_decode.load());
				}
				else
				{
					cellVdec.trace("AU decoding: done (handle=0x%x, seq_id=%d, cmd_id=%d)", handle, cmd->seq_id, cmd->id);
				}

				break;
			}
			case vdec_cmd_type::framerate:
			{
				frc_set = cmd->framerate;
				break;
			}
			case vdec_cmd_type::close:
			{
				if (!mpeg2_filter.active_filter_name.empty())
				{
					cellVdec.notice("Closing host MPEG-2 enhancement path '%s' with inputs=%llu, outputs=%llu.", mpeg2_filter.active_filter_name, mpeg2_filter.queued_inputs, mpeg2_filter.emitted_outputs);
				}

				mpeg2_filter.reset();

				std::lock_guard lock{mutex};
				out_queue.clear();
				break;
			}
			default:
				fmt::throw_exception("Unknown vdec_cmd_type (handle=0x%x, seq_id=%d, cmd_id=%d, type=%d)", handle, cmd->seq_id, cmd->id, static_cast<u32>(cmd->type));
				break;
			}

			std::lock_guard lock{mutex};
			if (seq_state == sequence_state::closed)
			{
				break;
			}
		}

		// Make sure the state is closed at the end
		std::lock_guard lock{mutex};
		seq_state = sequence_state::closed;
	}
};

extern bool check_if_vdec_contexts_exist()
{
	bool context_exists = false;

	idm::select<vdec_context>([&](u32, vdec_context&)
	{
		context_exists = true;
	});

	return context_exists;
}

extern void vdecEntry(ppu_thread& ppu, u32 vid)
{
	idm::get_unlocked<vdec_context>(vid)->exec(ppu, vid);

	ppu.state += cpu_flag::exit;
}

static error_code vdecQueryAttr(s32 type, u32 profile, u32 spec_addr /* may be 0 */, CellVdecAttr* attr)
{
	// Write 0 at start
	attr->memSize = 0;

	u32 decoderVerLower;
	u32 memSize = 0;

	const bool new_sdk = g_ps3_process_info.sdk_ver > 0x20FFFF;

	switch (type)
	{
	case CELL_VDEC_CODEC_TYPE_AVC:
	{
		cellVdec.warning("cellVdecQueryAttr: AVC (profile=%d)", profile);

		const vm::ptr<CellVdecAvcSpecificInfo> sinfo = vm::cast(spec_addr);

		if (sinfo)
		{
			if (sinfo->thisSize != sizeof(CellVdecAvcSpecificInfo))
			{
				return { CELL_VDEC_ERROR_ARG, "Invalid AVC specific info size %d", sinfo->thisSize };
			}
		}

		// TODO: sinfo

		switch (profile)
		{
		case CELL_VDEC_AVC_LEVEL_1P0: memSize = new_sdk ? 0x70167D  : 0xA014FD ; break;
		case CELL_VDEC_AVC_LEVEL_1P1: memSize = new_sdk ? 0x86CB7D  : 0xB6C9FD ; break;
		case CELL_VDEC_AVC_LEVEL_1P2: memSize = new_sdk ? 0x9E307D  : 0xCE2D7D ; break;
		case CELL_VDEC_AVC_LEVEL_1P3: memSize = new_sdk ? 0xA057FD  : 0xD054FD ; break;
		case CELL_VDEC_AVC_LEVEL_2P0: memSize = new_sdk ? 0xA057FD  : 0xD054FD ; break;
		case CELL_VDEC_AVC_LEVEL_2P1: memSize = new_sdk ? 0xE90DFD  : 0x1190AFD; break;
		case CELL_VDEC_AVC_LEVEL_2P2: memSize = new_sdk ? 0x14E49FD : 0x17E46FD; break;
		case CELL_VDEC_AVC_LEVEL_3P0: memSize = new_sdk ? 0x155B5FD : 0x185B17D; break;
		case CELL_VDEC_AVC_LEVEL_3P1: memSize = new_sdk ? 0x1CD327D : 0x1FD2AFD; break;
		case CELL_VDEC_AVC_LEVEL_3P2: memSize = new_sdk ? 0x2397B7D : 0x2696F7D; break;
		case CELL_VDEC_AVC_LEVEL_4P0: memSize = new_sdk ? 0x33A5FFD : 0x36A527D; break;
		case CELL_VDEC_AVC_LEVEL_4P1: memSize = new_sdk ? 0x33A5FFD : 0x36A527D; break;
		case CELL_VDEC_AVC_LEVEL_4P2: memSize = new_sdk ? 0x33A5FFD : 0x36A527D; break;
		default: return { CELL_VDEC_ERROR_ARG, "Invalid AVC profile level %d", profile };
		}

		decoderVerLower = 0x11300;
		break;
	}
	case CELL_VDEC_CODEC_TYPE_MPEG2:
	{
		cellVdec.warning("cellVdecQueryAttr: MPEG2 (profile=%d)", profile);

		const vm::ptr<CellVdecMpeg2SpecificInfo> sinfo = vm::cast(spec_addr);

		if (sinfo)
		{
			if (sinfo->thisSize != sizeof(CellVdecMpeg2SpecificInfo))
			{
				return { CELL_VDEC_ERROR_ARG, "Invalid MPEG2 specific info size %d", sinfo->thisSize };
			}
		}

		const u32 maxDecH = sinfo ? +sinfo->maxDecodedFrameHeight : 0;
		const u32 maxDecW = sinfo ? +sinfo->maxDecodedFrameWidth : 0;

		switch (profile)
		{
		case CELL_VDEC_MPEG2_MP_LL:
		{
			if (maxDecW > 352 || maxDecH > 288)
			{
				return { CELL_VDEC_ERROR_ARG, "Invalid max decoded frame size %dx%d for profile %d", maxDecH, maxDecW, profile };
			}

			memSize = new_sdk ? 0x11290B : 0x2A610B;
			break;
		}
		case CELL_VDEC_MPEG2_MP_ML:
		{
			if (maxDecW > 720 || maxDecH > 576)
			{
				return { CELL_VDEC_ERROR_ARG, "Invalid max decoded frame size %dx%d for profile %d", maxDecH, maxDecW, profile };
			}

			memSize = new_sdk ? 0x2DFB8B : 0x47110B;
			break;
		}
		case CELL_VDEC_MPEG2_MP_H14:
		{
			if (maxDecW > 1440 || maxDecH > 1152)
			{
				return { CELL_VDEC_ERROR_ARG, "Invalid max decoded frame size %dx%d for profile %d", maxDecH, maxDecW, profile };
			}

			memSize = new_sdk ? 0xA0270B : 0xB8F90B;
			break;
		}
		case CELL_VDEC_MPEG2_MP_HL:
		{
			if (maxDecW > 1920 || maxDecH > 1152)
			{
				return { CELL_VDEC_ERROR_ARG, "Invalid max decoded frame size %dx%d for profile %d", maxDecH, maxDecW, profile };
			}

			memSize = new_sdk ? 0xD2F40B : 0xEB990B;
			break;
		}
		default: return { CELL_VDEC_ERROR_ARG, "Invalid MPEG2 profile %d", profile };
		}

		decoderVerLower = 0x1030000;
		break;
	}
	case CELL_VDEC_CODEC_TYPE_MPEG4:
	{
		cellVdec.warning("cellVdecQueryAttr: MPEG4 (profile=%d)", profile);

		const vm::ptr<CellVdecMpeg4SpecificInfo> sinfo = vm::cast(spec_addr);

		if (sinfo)
		{
			if (sinfo->thisSize != sizeof(CellVdecMpeg4SpecificInfo))
			{
				return { CELL_VDEC_ERROR_ARG, "Invalid MPEG4 specific info size %d", sinfo->thisSize };
			}
		}

		const u32 maxDecH = sinfo ? +sinfo->maxDecodedFrameHeight : 0;
		const u32 maxDecW = sinfo ? +sinfo->maxDecodedFrameWidth : 0;

		switch (profile)
		{
		case CELL_VDEC_MPEG4_SP_L1:
		{
			if (maxDecW > 176 || maxDecH > 144)
			{
				return { CELL_VDEC_ERROR_ARG, "Invalid max decoded frame size %dx%d for profile %d", maxDecH, maxDecW, profile };
			}

			memSize = new_sdk ? 0x8B78B : 0xBB70B;
			break;
		}
		case CELL_VDEC_MPEG4_SP_L2:
		case CELL_VDEC_MPEG4_SP_L3:
		{
			if (maxDecW > 352 || maxDecH > 288)
			{
				return { CELL_VDEC_ERROR_ARG, "Invalid max decoded frame size %dx%d for profile %d", maxDecH, maxDecW, profile };
			}

			memSize = new_sdk ? 0xEFE0B : 0x11FD8B;
			break;
		}
		case CELL_VDEC_MPEG4_SP_D1_NTSC:
		{
			if (maxDecW > 720 || maxDecH > 480)
			{
				return { CELL_VDEC_ERROR_ARG, "Invalid max decoded frame size %dx%d for profile %d", maxDecH, maxDecW, profile };
			}

			memSize = new_sdk ? 0x22DB0B : 0x25DA8B;
			break;
		}
		case CELL_VDEC_MPEG4_SP_VGA:
		{
			if (maxDecW > 640 || maxDecH > 480)
			{
				return { CELL_VDEC_ERROR_ARG, "Invalid max decoded frame size %dx%d for profile %d", maxDecH, maxDecW, profile };
			}

			memSize = new_sdk ? 0x1FC00B : 0x22BF8B;
			break;
		}
		case CELL_VDEC_MPEG4_SP_D1_PAL:
		{
			if (maxDecW > 720 || maxDecH > 576)
			{
				return { CELL_VDEC_ERROR_ARG, "Invalid max decoded frame size %dx%d for profile %d", maxDecH, maxDecW, profile };
			}

			memSize = new_sdk ? 0x28570B : 0x2B568B;
			break;
		}
		default: return { CELL_VDEC_ERROR_ARG, "Invalid MPEG4 profile %d", profile };
		}

		decoderVerLower = 0x1080000;
		break;
	}
	case CELL_VDEC_CODEC_TYPE_DIVX:
	{
		cellVdec.warning("cellVdecQueryAttr: DivX (profile=%d)", profile);

		const vm::ptr<CellVdecDivxSpecificInfo2> sinfo = vm::cast(spec_addr);

		if (sinfo)
		{
			if (sinfo->thisSize != sizeof(CellVdecDivxSpecificInfo) && sinfo->thisSize != sizeof(CellVdecDivxSpecificInfo2))
			{
				return { CELL_VDEC_ERROR_ARG, "Invalid DIVX specific info size %d", sinfo->thisSize };
			}
		}

		// TODO: sinfo

		//const u32 maxDecH = sinfo ? +sinfo->maxDecodedFrameHeight : 0;
		//const u32 maxDecW = sinfo ? +sinfo->maxDecodedFrameWidth : 0;
		u32 nrOfBuf = sinfo && sinfo->thisSize == sizeof(CellVdecDivxSpecificInfo2) ? +sinfo->numberOfDecodedFrameBuffer : 0;

		if (nrOfBuf == 0)
		{
			nrOfBuf = 4;
		}
		else if (nrOfBuf == 2)
		{
			if (profile != CELL_VDEC_DIVX_QMOBILE && profile != CELL_VDEC_DIVX_MOBILE)
			{
				return { CELL_VDEC_ERROR_ARG, "Invalid number of decoded frame buffers %d for DIVX profile %d", nrOfBuf, profile };
			}
		}
		else if (nrOfBuf != 4 && nrOfBuf != 3)
		{
			return { CELL_VDEC_ERROR_ARG, "Invalid number of decoded frame buffers %d for DIVX", nrOfBuf };
		}

		// TODO: change memSize based on buffercount.

		switch (profile)
		{
		case CELL_VDEC_DIVX_QMOBILE     : memSize = new_sdk ? 0x11B720 : 0x1DEF30; break;
		case CELL_VDEC_DIVX_MOBILE      : memSize = new_sdk ? 0x19A740 : 0x26DED0; break;
		case CELL_VDEC_DIVX_HOME_THEATER: memSize = new_sdk ? 0x386A60 : 0x498060; break;
		case CELL_VDEC_DIVX_HD_720      : memSize = new_sdk ? 0x692070 : 0x805690; break;
		case CELL_VDEC_DIVX_HD_1080     : memSize = new_sdk ? 0xD78100 : 0xFC9870; break;
		default: return { CELL_VDEC_ERROR_ARG, "Invalid DIVX profile %d", profile };
		}

		decoderVerLower = 0x30806;
		break;
	}
	default: return { CELL_VDEC_ERROR_ARG, "Invalid codec type %d", type };
	}

	attr->decoderVerLower = decoderVerLower;
	attr->decoderVerUpper = 0x4840010;
	attr->memSize = !spec_addr ? ensure(memSize) : 4 * 1024 * 1024;
	attr->cmdDepth = 4;
	return CELL_OK;
}

error_code cellVdecQueryAttr(vm::cptr<CellVdecType> type, vm::ptr<CellVdecAttr> attr)
{
	cellVdec.warning("cellVdecQueryAttr(type=*0x%x, attr=*0x%x)", type, attr);

	if (!type || !attr)
	{
		return { CELL_VDEC_ERROR_ARG, "type=%d, attr=%d", !!type, !!attr };
	}

	return vdecQueryAttr(type->codecType, type->profileLevel, 0, attr.get_ptr());
}

error_code cellVdecQueryAttrEx(vm::cptr<CellVdecTypeEx> type, vm::ptr<CellVdecAttr> attr)
{
	cellVdec.warning("cellVdecQueryAttrEx(type=*0x%x, attr=*0x%x)", type, attr);

	if (!type || !attr)
	{
		return { CELL_VDEC_ERROR_ARG, "type=%d, attr=%d", !!type, !!attr };
	}

	return vdecQueryAttr(type->codecType, type->profileLevel, type->codecSpecificInfo_addr, attr.get_ptr());
}

template <typename T, typename U>
static error_code vdecOpen(ppu_thread& ppu, T type, U res, vm::cptr<CellVdecCb> cb, vm::ptr<u32> handle)
{
	if (!type || !res || !cb || !handle || !cb->cbFunc)
	{
		return { CELL_VDEC_ERROR_ARG, "type=%d, res=%d, cb=%d, handle=%d, cbFunc=%d", !!type, !!res, !!cb, !!handle, cb && cb->cbFunc };
	}

	if (!res->memAddr || res->ppuThreadPriority + 0u >= 3072 || res->spuThreadPriority + 0u >= 256 || res->ppuThreadStackSize < 4096
		|| type->codecType + 0u >= 0xe)
	{
		return { CELL_VDEC_ERROR_ARG, "memAddr=%d, ppuThreadPriority=%d, spuThreadPriority=%d, ppuThreadStackSize=%d, codecType=%d",
		                              res->memAddr, res->ppuThreadPriority, res->spuThreadPriority, res->ppuThreadStackSize, type->codecType };
	}

	u32 spec_addr = 0;

	if constexpr (std::is_same_v<std::decay_t<typename T::type>, CellVdecTypeEx>)
	{
		spec_addr = type->codecSpecificInfo_addr;
	}

	CellVdecAttr attr{};
	const error_code err = vdecQueryAttr(type->codecType, type->profileLevel, spec_addr, &attr);
	if (err != CELL_OK)
	{
		return err;
	}

	if (attr.memSize > res->memSize)
	{
		return { CELL_VDEC_ERROR_ARG, "attr.memSize=%d, res->memSize=%d", attr.memSize, res->memSize };
	}

	// Create decoder context
	shared_ptr<vdec_context> vdec;

	if (std::unique_lock lock{g_fxo->get<hle_locks_t>(), std::try_to_lock})
	{
		vdec = idm::make_ptr<vdec_context>(type->codecType, type->profileLevel, res->memAddr, res->memSize, cb->cbFunc, cb->cbArg);
	}
	else
	{
		ppu.state += cpu_flag::again;
		return {};
	}

	const u32 vid = idm::last_id();

	ensure(vdec);
	vdec->handle = vid;

	// Run thread
	vm::var<u64> _tid;
	vm::var<char[]> _name = vm::make_str("HLE Video Decoder");
	ppu_execute<&sys_ppu_thread_create>(ppu, +_tid, 0x10000, vid, +res->ppuThreadPriority, +res->ppuThreadStackSize, SYS_PPU_THREAD_CREATE_INTERRUPT, +_name);
	*handle = vid;

	const auto thrd = idm::get_unlocked<named_thread<ppu_thread>>(static_cast<u32>(*_tid));

	thrd->cmd_list
	({
		{ ppu_cmd::set_args, 1 }, u64{vid},
		{ ppu_cmd::hle_call, FIND_FUNC(vdecEntry) },
	});

	thrd->state -= cpu_flag::stop;
	thrd->state.notify_one();

	return CELL_OK;
}

error_code cellVdecOpen(ppu_thread& ppu, vm::cptr<CellVdecType> type, vm::cptr<CellVdecResource> res, vm::cptr<CellVdecCb> cb, vm::ptr<u32> handle)
{
	cellVdec.warning("cellVdecOpen(type=*0x%x, res=*0x%x, cb=*0x%x, handle=*0x%x)", type, res, cb, handle);

	return vdecOpen(ppu, type, res, cb, handle);
}

error_code cellVdecOpenEx(ppu_thread& ppu, vm::cptr<CellVdecTypeEx> type, vm::cptr<CellVdecResourceEx> res, vm::cptr<CellVdecCb> cb, vm::ptr<u32> handle)
{
	cellVdec.warning("cellVdecOpenEx(type=*0x%x, res=*0x%x, cb=*0x%x, handle=*0x%x)", type, res, cb, handle);

	return vdecOpen(ppu, type, res, cb, handle);
}

error_code cellVdecClose(ppu_thread& ppu, u32 handle)
{
	cellVdec.warning("cellVdecClose(handle=0x%x)", handle);

	std::unique_lock lock_hle{g_fxo->get<hle_locks_t>(), std::try_to_lock};

	if (!lock_hle)
	{
		ppu.state += cpu_flag::again;
		return {};
	}

	auto vdec = idm::get_unlocked<vdec_context>(handle);

	if (!vdec)
	{
		return { CELL_VDEC_ERROR_ARG, "vdec is nullptr" };
	}

	{
		std::lock_guard lock{vdec->mutex};

		if (vdec->seq_state == sequence_state::closed)
		{
			return { CELL_VDEC_ERROR_SEQ, vdec->seq_state.load() };
		}
	}

	const u64 seq_id = vdec->seq_id;
	const u64 cmd_id = vdec->next_cmd_id++;
	cellVdec.trace("Adding close cmd (handle=0x%x, seq_id=%d, cmd_id=%d)", handle, seq_id, cmd_id);

	lv2_obj::sleep(ppu);
	vdec->abort_decode = true;
	vdec->in_cmd.push(vdec_cmd(vdec_cmd_type::close, seq_id, cmd_id));

	while (!vdec->ppu_tid)
	{
		thread_ctrl::wait_for(1000);
	}

	const u32 tid = vdec->ppu_tid.exchange(-1);

	if (tid != umax)
	{
		ppu_execute<&sys_interrupt_thread_disestablish>(ppu, tid);
	}

	vdec->seq_state = sequence_state::closed;
	vdec->mutex.lock_unlock();

	if (!idm::remove_verify<vdec_context>(handle, std::move(vdec)))
	{
		// Other thread removed it beforehead
		return { CELL_VDEC_ERROR_ARG, "remove_verify failed" };
	}

	return CELL_OK;
}

error_code cellVdecStartSeq(ppu_thread& ppu, u32 handle)
{
	ppu.state += cpu_flag::wait;

	cellVdec.warning("cellVdecStartSeq(handle=0x%x)", handle);

	const auto vdec = idm::get_unlocked<vdec_context>(handle);

	if (!vdec)
	{
		return { CELL_VDEC_ERROR_ARG, "vdec is nullptr" };
	}

	sequence_state old_state{};

	{
		std::lock_guard lock{vdec->mutex};
		old_state = vdec->seq_state;

		if (old_state != sequence_state::dormant && old_state != sequence_state::ready)
		{
			return { CELL_VDEC_ERROR_SEQ, old_state };
		}

		if (old_state == sequence_state::ready)
		{
			vdec->seq_state = sequence_state::resetting;
		}
	}

	const u64 seq_id = ++vdec->seq_id;
	const u64 cmd_id = vdec->next_cmd_id++;
	cellVdec.trace("Adding start cmd (handle=0x%x, seq_id=%d, cmd_id=%d)", handle, seq_id, cmd_id);

	vdec->abort_decode = false;
	vdec->is_running = false;
	vdec->in_cmd.push(vdec_cmd(vdec_cmd_type::start_sequence, seq_id, cmd_id));

	std::lock_guard lock{vdec->mutex};

	if (false) // TODO: set to old state in case of error
	{
		vdec->seq_state = old_state;
	}
	else
	{
		vdec->seq_state = sequence_state::ready;
	}

	return CELL_OK;
}

error_code cellVdecEndSeq(ppu_thread& ppu, u32 handle)
{
	ppu.state += cpu_flag::wait;

	cellVdec.warning("cellVdecEndSeq(handle=0x%x)", handle);

	const auto vdec = idm::get_unlocked<vdec_context>(handle);

	if (!vdec)
	{
		return { CELL_VDEC_ERROR_ARG, "vdec is nullptr" };
	}

	{
		std::lock_guard lock{vdec->mutex};

		if (vdec->seq_state != sequence_state::ready)
		{
			return { CELL_VDEC_ERROR_SEQ, vdec->seq_state.load() };
		}

		vdec->seq_state = sequence_state::ending;
	}

	const u64 seq_id = vdec->seq_id;
	const u64 cmd_id = vdec->next_cmd_id++;
	cellVdec.trace("Adding end cmd (handle=0x%x, seq_id=%d, cmd_id=%d)", handle, seq_id, cmd_id);

	vdec->in_cmd.push(vdec_cmd(vdec_cmd_type::end_sequence, seq_id, cmd_id));

	return CELL_OK;
}

error_code cellVdecDecodeAu(ppu_thread& ppu, u32 handle, CellVdecDecodeMode mode, vm::cptr<CellVdecAuInfo> auInfo)
{
	ppu.state += cpu_flag::wait;

	cellVdec.trace("cellVdecDecodeAu(handle=0x%x, mode=%d, auInfo=*0x%x)", handle, +mode, auInfo);

	const auto vdec = idm::get_unlocked<vdec_context>(handle);

	if (!vdec || !auInfo || !auInfo->size || !auInfo->startAddr)
	{
		return { CELL_VDEC_ERROR_ARG, "vdec=%d, auInfo=%d, size=%d, startAddr=0x%x", !!vdec, !!auInfo, auInfo ? auInfo->size.value() : 0, auInfo ? auInfo->startAddr.value() : 0 };
	}

	{
		std::lock_guard lock{vdec->mutex};

		if (vdec->seq_state != sequence_state::ready)
		{
			return { CELL_VDEC_ERROR_SEQ, vdec->seq_state.load() };
		}
	}

	if (mode < 0 || mode > (CELL_VDEC_DEC_MODE_B_SKIP | CELL_VDEC_DEC_MODE_PB_SKIP))
	{
		return { CELL_VDEC_ERROR_ARG, "mode=%d", +mode };
	}

	if ((mode == (CELL_VDEC_DEC_MODE_B_SKIP | CELL_VDEC_DEC_MODE_PB_SKIP) && vdec->type != CELL_VDEC_CODEC_TYPE_VC1) ||
		(mode == CELL_VDEC_DEC_MODE_PB_SKIP && vdec->type != CELL_VDEC_CODEC_TYPE_AVC))
	{
		return { CELL_VDEC_ERROR_ARG, "mode=%d, type=%d", +mode, vdec->type };
	}

	if (!vdec->au_count.try_inc(4))
	{
		return CELL_VDEC_ERROR_BUSY;
	}

	const u64 seq_id = vdec->seq_id;
	const u64 cmd_id = vdec->next_cmd_id++;
	cellVdec.trace("Adding decode cmd (handle=0x%x, seq_id=%d, cmd_id=%d)", handle, seq_id, cmd_id);

	// TODO: check info
	vdec->in_cmd.push(vdec_cmd(vdec_cmd_type::au_decode, seq_id, cmd_id, mode, *auInfo));
	return CELL_OK;
}

error_code cellVdecDecodeAuEx2(ppu_thread& ppu, u32 handle, CellVdecDecodeMode mode, vm::cptr<CellVdecAuInfoEx2> auInfo)
{
	ppu.state += cpu_flag::wait;

	cellVdec.todo("cellVdecDecodeAuEx2(handle=0x%x, mode=%d, auInfo=*0x%x)", handle, +mode, auInfo);

	const auto vdec = idm::get_unlocked<vdec_context>(handle);

	if (!vdec || !auInfo || !auInfo->size || !auInfo->startAddr)
	{
		return { CELL_VDEC_ERROR_ARG, "vdec=%d, auInfo=%d, size=%d, startAddr=0x%x", !!vdec, !!auInfo, auInfo ? auInfo->size.value() : 0, auInfo ? auInfo->startAddr.value() : 0 };
	}

	{
		std::lock_guard lock{vdec->mutex};

		if (vdec->seq_state != sequence_state::ready)
		{
			return { CELL_VDEC_ERROR_SEQ, vdec->seq_state.load() };
		}
	}

	if (mode < 0 || mode > (CELL_VDEC_DEC_MODE_B_SKIP | CELL_VDEC_DEC_MODE_PB_SKIP))
	{
		return { CELL_VDEC_ERROR_ARG, "mode=%d", +mode };
	}

	if ((mode == (CELL_VDEC_DEC_MODE_B_SKIP | CELL_VDEC_DEC_MODE_PB_SKIP) && vdec->type != CELL_VDEC_CODEC_TYPE_VC1) ||
		(mode == CELL_VDEC_DEC_MODE_PB_SKIP && vdec->type != CELL_VDEC_CODEC_TYPE_AVC))
	{
		return { CELL_VDEC_ERROR_ARG, "mode=%d, type=%d", +mode, vdec->type };
	}

	if (!vdec->au_count.try_inc(4))
	{
		return CELL_VDEC_ERROR_BUSY;
	}

	CellVdecAuInfo au_info{};
	au_info.startAddr = auInfo->startAddr;
	au_info.size = auInfo->size;
	au_info.pts = auInfo->pts;
	au_info.dts = auInfo->dts;
	au_info.userData = auInfo->userData;
	au_info.codecSpecificData = auInfo->codecSpecificData;

	const u64 seq_id = vdec->seq_id;
	const u64 cmd_id = vdec->next_cmd_id++;
	cellVdec.trace("Adding decode cmd (handle=0x%x, seq_id=%d, cmd_id=%d)", handle, seq_id, cmd_id);

	// TODO: check info
	vdec->in_cmd.push(vdec_cmd(vdec_cmd_type::au_decode, seq_id, cmd_id, mode, au_info));
	return CELL_OK;
}

error_code cellVdecGetPictureExt(ppu_thread& ppu, u32 handle, vm::cptr<CellVdecPicFormat2> format, vm::ptr<u8> outBuff, u32 arg4)
{
	ppu.state += cpu_flag::wait;

	cellVdec.trace("cellVdecGetPictureExt(handle=0x%x, format=*0x%x, outBuff=*0x%x, arg4=*0x%x)", handle, format, outBuff, arg4);

	const auto vdec = idm::get_unlocked<vdec_context>(handle);

	if (!vdec || !format)
	{
		return { CELL_VDEC_ERROR_ARG, "vdec=%d, format=%d", !!vdec, !!format };
	}

	{
		std::lock_guard lock{vdec->mutex};

		if (vdec->seq_state == sequence_state::closed || vdec->seq_state > sequence_state::ending)
		{
			return { CELL_VDEC_ERROR_SEQ, vdec->seq_state.load() };
		}
	}

	if (format->formatType > 4 || (format->formatType <= CELL_VDEC_PICFMT_RGBA32_ILV && format->colorMatrixType > CELL_VDEC_COLOR_MATRIX_TYPE_BT709))
	{
		return {CELL_VDEC_ERROR_ARG, "formatType=%d, colorMatrixType=%d", +format->formatType, +format->colorMatrixType};
	}

	if (arg4 && arg4 != 8 && arg4 != 0xc)
	{
		return { CELL_VDEC_ERROR_ARG, "arg4=0x%x", arg4 };
	}

	if (arg4 || format->unk0 || format->unk1)
	{
		cellVdec.todo("cellVdecGetPictureExt: Unknown arguments (arg4=*0x%x, unk0=0x%x, unk1=0x%x)", arg4, format->unk0, format->unk1);
	}

	vdec_frame frame;
	bool notify = false;
	u64 sequence_id{};

	{
		std::lock_guard lock(vdec->mutex);

		if (vdec->out_queue.empty())
		{
			return CELL_VDEC_ERROR_EMPTY;
		}

		frame = std::move(vdec->out_queue.front());
		sequence_id = vdec->seq_id;

		vdec->out_queue.pop_front();
		if (vdec->out_queue.size() + 1 == vdec->get_output_queue_limit())
			notify = true;
	}

	if (notify)
	{
		auto vdec_ppu = idm::get_unlocked<named_thread<ppu_thread>>(vdec->ppu_tid);
		if (vdec_ppu) thread_ctrl::notify(*vdec_ppu);
	}

	if (sequence_id != frame.seq_id)
	{
		return { CELL_VDEC_ERROR_EMPTY, "sequence_id=%d, seq_id=%d", sequence_id, frame.seq_id };
	}

	if (outBuff)
	{
		const int w = frame->width;
		const int h = frame->height;

		AVPixelFormat out_f = AV_PIX_FMT_YUV420P;

		std::unique_ptr<u8[]> alpha_plane;

		switch (const u32 type = format->formatType)
		{
		case CELL_VDEC_PICFMT_ARGB32_ILV: out_f = AV_PIX_FMT_ARGB; alpha_plane.reset(new u8[w * h]); break;
		case CELL_VDEC_PICFMT_RGBA32_ILV: out_f = AV_PIX_FMT_RGBA; alpha_plane.reset(new u8[w * h]); break;
		case CELL_VDEC_PICFMT_UYVY422_ILV: out_f = AV_PIX_FMT_UYVY422; break;
		case CELL_VDEC_PICFMT_YUV420_PLANAR: out_f = AV_PIX_FMT_YUV420P; break;
		default:
		{
			fmt::throw_exception("cellVdecGetPictureExt: Unknown formatType (handle=0x%x, seq_id=%d, cmd_id=%d, type=%d)", handle, frame.seq_id, frame.cmd_id, type);
		}
		}

		// TODO: color matrix

		if (alpha_plane)
		{
			std::memset(alpha_plane.get(), format->alpha, w * h);
		}

		AVPixelFormat in_f = AV_PIX_FMT_YUV420P;

		switch (frame->format)
		{
		case AV_PIX_FMT_YUVJ420P:
			cellVdec.error("cellVdecGetPictureExt: experimental AVPixelFormat (handle=0x%x, seq_id=%d, cmd_id=%d, format=%d). This may cause suboptimal video quality.", handle, frame.seq_id, frame.cmd_id, frame->format);
			[[fallthrough]];
		case AV_PIX_FMT_YUV420P:
			in_f = alpha_plane ? AV_PIX_FMT_YUVA420P : static_cast<AVPixelFormat>(frame->format);
			break;
		default:
			fmt::throw_exception("cellVdecGetPictureExt: Unknown frame format (%d)", frame->format);
		}

		cellVdec.trace("cellVdecGetPictureExt: handle=0x%x, seq_id=%d, cmd_id=%d, w=%d, h=%d, frameFormat=%d, formatType=%d, in_f=%d, out_f=%d, alpha_plane=%d, alpha=%d, colorMatrixType=%d", handle, frame.seq_id, frame.cmd_id, w, h, frame->format, format->formatType, +in_f, +out_f, !!alpha_plane, format->alpha, format->colorMatrixType);

		// MPEG-2 FMVs are often interlaced and chroma-starved; a slightly smoother conversion filter
		// helps the final output without changing decode cadence or game timing.
		const int sws_flags = vdec->type == CELL_VDEC_CODEC_TYPE_MPEG2 ? SWS_BILINEAR : SWS_POINT;
		vdec->sws = sws_getCachedContext(vdec->sws, w, h, in_f, w, h, out_f, sws_flags, nullptr, nullptr, nullptr);

		u8* in_data[4] = { frame->data[0], frame->data[1], frame->data[2], alpha_plane.get() };
		int in_line[4] = { frame->linesize[0], frame->linesize[1], frame->linesize[2], w * 1 };
		u8* out_data[4] = { outBuff.get_ptr() };
		int out_line[4] = { w * 4 }; // RGBA32 or ARGB32

		// TODO:
		// It's possible that we need to align the pitch to 128 here.
		// PS HOME seems to rely on this somehow in certain cases.

		if (!alpha_plane)
		{
			// YUV420P or UYVY422
			out_data[1] = out_data[0] + w * h;
			out_data[2] = out_data[0] + w * h * 5 / 4;

			if (const int ret = av_image_fill_linesizes(out_line, out_f, w); ret < 0)
			{
				fmt::throw_exception("cellVdecGetPictureExt: av_image_fill_linesizes failed (handle=0x%x, seq_id=%d, cmd_id=%d, ret=0x%x): %s", handle, frame.seq_id, frame.cmd_id, ret, utils::av_error_to_string(ret));
			}
		}

		sws_scale(vdec->sws, in_data, in_line, 0, h, out_data, out_line);

		if (vdec->type == CELL_VDEC_CODEC_TYPE_MPEG2 && is_interlaced_frame(frame.avf.get()))
		{
			const auto format_type = static_cast<CellVdecPicFormatType>(+format->formatType);
			deinterlace_output_frame(format_type, out_data, out_line, w, h, is_top_field_first_frame(frame.avf.get()));
		}
	}

	return CELL_OK;
}

error_code cellVdecGetPicture(ppu_thread& ppu, u32 handle, vm::cptr<CellVdecPicFormat> format, vm::ptr<u8> outBuff)
{
	ppu.state += cpu_flag::wait;

	cellVdec.trace("cellVdecGetPicture(handle=0x%x, format=*0x%x, outBuff=*0x%x)", handle, format, outBuff);

	if (!format)
	{
		return { CELL_VDEC_ERROR_ARG, "format is nullptr" };
	}

	vm::var<CellVdecPicFormat2> format2;
	format2->formatType = format->formatType;
	format2->colorMatrixType = format->colorMatrixType;
	format2->alpha = format->alpha;
	format2->unk0 = 0;
	format2->unk1 = 0;

	return cellVdecGetPictureExt(ppu, handle, format2, outBuff, 0);
}

error_code cellVdecGetPicItem(ppu_thread& ppu, u32 handle, vm::pptr<CellVdecPicItem> picItem)
{
	ppu.state += cpu_flag::wait;

	cellVdec.trace("cellVdecGetPicItem(handle=0x%x, picItem=**0x%x)", handle, picItem);

	const auto vdec = idm::get_unlocked<vdec_context>(handle);

	if (!vdec || !picItem)
	{
		return { CELL_VDEC_ERROR_ARG, "vdec=%d, picItem=%d", !!vdec, !!picItem };
	}

	u64 sequence_id{};

	{
		std::lock_guard lock{vdec->mutex};

		if (vdec->seq_state == sequence_state::closed || vdec->seq_state > sequence_state::ending)
		{
			return { CELL_VDEC_ERROR_SEQ, vdec->seq_state.load() };
		}

		sequence_id = vdec->seq_id;
	}

	struct all_info_t
	{
		CellVdecPicItem picItem;
		union
		{
			CellVdecAvcInfo avcInfo;
			CellVdecDivxInfo divxInfo;
			CellVdecMpeg2Info mpeg2Info;
		} picInfo;
	};

	AVFrame* frame{};
	u64 seq_id{};
	u64 cmd_id{};
	u64 pts;
	u64 dts;
	u64 usrd;
	u32 frc = 0;
	CellVdecPicAttr attr = CELL_VDEC_PICITEM_ATTR_NORMAL;
	vm::ptr<CellVdecPicItem> info;
	{
		std::lock_guard lock(vdec->mutex);

		for (auto& picture : vdec->out_queue)
		{
			if (!picture.pic_item_received)
			{
				picture.pic_item_received = true;
				frame = picture.avf.get();
				seq_id = picture.seq_id;
				cmd_id = picture.cmd_id;
				pts = picture.pts;
				dts = picture.dts;
				usrd = picture.userdata;
				frc = picture.frc;
				attr = picture.attr;
				info.set(vdec->mem_addr + vdec->mem_bias);

				constexpr u64 size_needed = sizeof(all_info_t);

				if (vdec->mem_bias + size_needed >= vdec->mem_size / size_needed * size_needed)
				{
					vdec->mem_bias = 0;
					break;
				}

				vdec->mem_bias += size_needed;
				break;
			}
		}
	}

	if (!frame || seq_id != sequence_id)
	{
		// If frame is empty info was not found
		return { CELL_VDEC_ERROR_EMPTY, " frame=%d, sequence_id=%d, seq_id=%d", !!frame, sequence_id, seq_id };
	}

	info->codecType = vdec->type;
	info->startAddr = 0x00000123; // invalid value (no address for picture)
	const int buffer_size = av_image_get_buffer_size(vdec->ctx->pix_fmt, vdec->ctx->width, vdec->ctx->height, 1);
	ensure(buffer_size >= 0);
	info->size = utils::align<u32>(buffer_size, 128);
	info->auNum = 1;
	info->auPts[0].lower = static_cast<u32>(pts);
	info->auPts[0].upper = static_cast<u32>(pts >> 32);
	info->auPts[1].lower = CELL_CODEC_PTS_INVALID;
	info->auPts[1].upper = CELL_CODEC_PTS_INVALID;
	info->auDts[0].lower = static_cast<u32>(dts);
	info->auDts[0].upper = static_cast<u32>(dts >> 32);
	info->auDts[1].lower = CELL_CODEC_DTS_INVALID;
	info->auDts[1].upper = CELL_CODEC_DTS_INVALID;
	info->auUserData[0] = usrd;
	info->auUserData[1] = 0;
	info->status = CELL_OK;
	info->attr = attr;

	const vm::addr_t picinfo_addr{info.addr() + ::offset32(&all_info_t::picInfo)};
	info->picInfo_addr = picinfo_addr;

	if (vdec->type == CELL_VDEC_CODEC_TYPE_AVC)
	{
		const vm::ptr<CellVdecAvcInfo> avc = picinfo_addr;

		avc->horizontalSize = frame->width;
		avc->verticalSize = frame->height;

		switch (s32 pct = frame->pict_type)
		{
		case AV_PICTURE_TYPE_I: avc->pictureType[0] = CELL_VDEC_AVC_PCT_I; break;
		case AV_PICTURE_TYPE_P: avc->pictureType[0] = CELL_VDEC_AVC_PCT_P; break;
		case AV_PICTURE_TYPE_B: avc->pictureType[0] = CELL_VDEC_AVC_PCT_B; break;
		default:
		{
			avc->pictureType[0] = CELL_VDEC_AVC_PCT_UNKNOWN;
			cellVdec.error("cellVdecGetPicItem(AVC): unknown pict_type value (handle=0x%x, seq_id=%d, cmd_id=%d, pct=0x%x)", handle, seq_id, cmd_id, pct);
			break;
		}
		}

		avc->pictureType[1] = CELL_VDEC_AVC_PCT_UNKNOWN; // ???
		avc->idrPictureFlag = false; // ???
		avc->aspect_ratio_idc = CELL_VDEC_AVC_ARI_SAR_UNSPECIFIED; // ???
		avc->sar_height = 0;
		avc->sar_width = 0;
		avc->pic_struct = CELL_VDEC_AVC_PSTR_FRAME; // ???
		avc->picOrderCount[0] = 0; // ???
		avc->picOrderCount[1] = 0;
		avc->vui_parameters_present_flag = true; // ???
		avc->frame_mbs_only_flag = true; // ??? progressive
		avc->video_signal_type_present_flag = true; // ???
		avc->video_format = CELL_VDEC_AVC_VF_COMPONENT; // ???
		avc->video_full_range_flag = false; // ???
		avc->colour_description_present_flag = true;
		avc->colour_primaries = CELL_VDEC_AVC_CP_ITU_R_BT_709_5; // ???
		avc->transfer_characteristics = CELL_VDEC_AVC_TC_ITU_R_BT_709_5;
		avc->matrix_coefficients = CELL_VDEC_AVC_MXC_ITU_R_BT_709_5; // important
		avc->timing_info_present_flag = true;

		switch (frc)
		{
		case CELL_VDEC_FRC_24000DIV1001: avc->frameRateCode = CELL_VDEC_AVC_FRC_24000DIV1001; break;
		case CELL_VDEC_FRC_24: avc->frameRateCode = CELL_VDEC_AVC_FRC_24; break;
		case CELL_VDEC_FRC_25: avc->frameRateCode = CELL_VDEC_AVC_FRC_25; break;
		case CELL_VDEC_FRC_30000DIV1001: avc->frameRateCode = CELL_VDEC_AVC_FRC_30000DIV1001; break;
		case CELL_VDEC_FRC_30: avc->frameRateCode = CELL_VDEC_AVC_FRC_30; break;
		case CELL_VDEC_FRC_50: avc->frameRateCode = CELL_VDEC_AVC_FRC_50; break;
		case CELL_VDEC_FRC_60000DIV1001: avc->frameRateCode = CELL_VDEC_AVC_FRC_60000DIV1001; break;
		case CELL_VDEC_FRC_60: avc->frameRateCode = CELL_VDEC_AVC_FRC_60; break;
		default: cellVdec.error("cellVdecGetPicItem(AVC): unknown frc value (handle=0x%x, seq_id=%d, cmd_id=%d, frc=0x%x)", handle, seq_id, cmd_id, frc);
		}

		avc->fixed_frame_rate_flag = true;
		avc->low_delay_hrd_flag = true; // ???
		avc->entropy_coding_mode_flag = true; // ???
		avc->nalUnitPresentFlags = 0; // ???
		avc->ccDataLength[0] = 0;
		avc->ccDataLength[1] = 0;
		avc->reserved[0] = 0;
		avc->reserved[1] = 0;
	}
	// TODO: handle MPEG4 properly
	else if (vdec->type == CELL_VDEC_CODEC_TYPE_MPEG4 || vdec->type == CELL_VDEC_CODEC_TYPE_DIVX)
	{
		const vm::ptr<CellVdecDivxInfo> dvx = picinfo_addr;

		switch (s32 pct = frame->pict_type)
		{
		case AV_PICTURE_TYPE_I: dvx->pictureType = CELL_VDEC_DIVX_VCT_I; break;
		case AV_PICTURE_TYPE_P: dvx->pictureType = CELL_VDEC_DIVX_VCT_P; break;
		case AV_PICTURE_TYPE_B: dvx->pictureType = CELL_VDEC_DIVX_VCT_B; break;
		default: cellVdec.error("cellVdecGetPicItem(DivX): unknown pict_type value (handle=0x%x, seq_id=%d, cmd_id=%d, pct=0x%x)", handle, seq_id, cmd_id, pct);
		}

		dvx->horizontalSize = frame->width;
		dvx->verticalSize = frame->height;
		dvx->pixelAspectRatio = CELL_VDEC_DIVX_ARI_PAR_1_1; // ???
		dvx->parHeight = 0;
		dvx->parWidth = 0;
		dvx->colourDescription = false; // ???
		dvx->colourPrimaries = CELL_VDEC_DIVX_CP_ITU_R_BT_709; // ???
		dvx->transferCharacteristics = CELL_VDEC_DIVX_TC_ITU_R_BT_709; // ???
		dvx->matrixCoefficients = CELL_VDEC_DIVX_MXC_ITU_R_BT_709; // ???
		dvx->pictureStruct = CELL_VDEC_DIVX_PSTR_FRAME; // ???

		switch (frc)
		{
		case CELL_VDEC_FRC_24000DIV1001: dvx->frameRateCode = CELL_VDEC_DIVX_FRC_24000DIV1001; break;
		case CELL_VDEC_FRC_24: dvx->frameRateCode = CELL_VDEC_DIVX_FRC_24; break;
		case CELL_VDEC_FRC_25: dvx->frameRateCode = CELL_VDEC_DIVX_FRC_25; break;
		case CELL_VDEC_FRC_30000DIV1001: dvx->frameRateCode = CELL_VDEC_DIVX_FRC_30000DIV1001; break;
		case CELL_VDEC_FRC_30: dvx->frameRateCode = CELL_VDEC_DIVX_FRC_30; break;
		case CELL_VDEC_FRC_50: dvx->frameRateCode = CELL_VDEC_DIVX_FRC_50; break;
		case CELL_VDEC_FRC_60000DIV1001: dvx->frameRateCode = CELL_VDEC_DIVX_FRC_60000DIV1001; break;
		case CELL_VDEC_FRC_60: dvx->frameRateCode = CELL_VDEC_DIVX_FRC_60; break;
		default: cellVdec.error("cellVdecGetPicItem(DivX): unknown frc value (handle=0x%x, seq_id=%d, cmd_id=%d, frc=0x%x)", handle, seq_id, cmd_id, frc);
		}
	}
	else if (vdec->type == CELL_VDEC_CODEC_TYPE_MPEG2)
	{
		const vm::ptr<CellVdecMpeg2Info> mp2 = picinfo_addr;
		const bool is_interlaced = is_interlaced_frame(frame);
		const bool top_field_first = is_top_field_first_frame(frame);

		std::memset(mp2.get_ptr(), 0, sizeof(CellVdecMpeg2Info));
		mp2->horizontal_size = frame->width;
		mp2->vertical_size = frame->height;
		mp2->aspect_ratio_information = CELL_VDEC_MPEG2_ARI_SAR_1_1; // ???

		switch (frc)
		{
		case CELL_VDEC_FRC_24000DIV1001: mp2->frame_rate_code = CELL_VDEC_MPEG2_FRC_24000DIV1001; break;
		case CELL_VDEC_FRC_24: mp2->frame_rate_code = CELL_VDEC_MPEG2_FRC_24; break;
		case CELL_VDEC_FRC_25: mp2->frame_rate_code = CELL_VDEC_MPEG2_FRC_25; break;
		case CELL_VDEC_FRC_30000DIV1001: mp2->frame_rate_code = CELL_VDEC_MPEG2_FRC_30000DIV1001; break;
		case CELL_VDEC_FRC_30: mp2->frame_rate_code = CELL_VDEC_MPEG2_FRC_30; break;
		case CELL_VDEC_FRC_50: mp2->frame_rate_code = CELL_VDEC_MPEG2_FRC_50; break;
		case CELL_VDEC_FRC_60000DIV1001: mp2->frame_rate_code = CELL_VDEC_MPEG2_FRC_60000DIV1001; break;
		case CELL_VDEC_FRC_60: mp2->frame_rate_code = CELL_VDEC_MPEG2_FRC_60; break;
		default: cellVdec.error("cellVdecGetPicItem(MPEG2): unknown frc value (handle=0x%x, seq_id=%d, cmd_id=%d, frc=0x%x)", handle, seq_id, cmd_id, frc);
		}

		mp2->progressive_sequence = !is_interlaced;
		mp2->low_delay = true; // ???
		mp2->video_format = CELL_VDEC_MPEG2_VF_UNSPECIFIED; // ???
		mp2->colour_description = false; // ???

		switch (s32 pct = frame->pict_type)
		{
		case AV_PICTURE_TYPE_I: mp2->picture_coding_type[0] = CELL_VDEC_MPEG2_PCT_I; break;
		case AV_PICTURE_TYPE_P: mp2->picture_coding_type[0] = CELL_VDEC_MPEG2_PCT_P; break;
		case AV_PICTURE_TYPE_B: mp2->picture_coding_type[0] = CELL_VDEC_MPEG2_PCT_B; break;
		default: cellVdec.error("cellVdecGetPicItem(MPEG2): unknown pict_type value (handle=0x%x, seq_id=%d, cmd_id=%d, pct=0x%x)", handle, seq_id, cmd_id, pct);
		}

		mp2->picture_coding_type[1] = CELL_VDEC_MPEG2_PCT_FORBIDDEN; // ???
		mp2->picture_structure[0] = is_interlaced ? (top_field_first ? CELL_VDEC_MPEG2_PSTR_TOP_FIELD : CELL_VDEC_MPEG2_PSTR_BOTTOM_FIELD) : CELL_VDEC_MPEG2_PSTR_FRAME;
		mp2->picture_structure[1] = is_interlaced ? (top_field_first ? CELL_VDEC_MPEG2_PSTR_BOTTOM_FIELD : CELL_VDEC_MPEG2_PSTR_TOP_FIELD) : CELL_VDEC_MPEG2_PSTR_FRAME;
		mp2->top_field_first = top_field_first;
		mp2->repeat_first_field = false;
		mp2->progressive_frame = !is_interlaced;

		// ...
	}

	*picItem = info;
	return CELL_OK;
}

error_code cellVdecSetFrameRate(u32 handle, CellVdecFrameRate frameRateCode)
{
	cellVdec.trace("cellVdecSetFrameRate(handle=0x%x, frameRateCode=0x%x)", handle, +frameRateCode);

	const auto vdec = idm::get_unlocked<vdec_context>(handle);

	// 0x80 seems like a common prefix
	if (!vdec || (frameRateCode & 0xf8) != 0x80)
	{
		return { CELL_VDEC_ERROR_ARG, "vdec=%d, frameRateCode=0x%x", !!vdec, +frameRateCode };
	}

	std::lock_guard lock{vdec->mutex};

	if (vdec->seq_state == sequence_state::closed || vdec->seq_state >= sequence_state::invalid)
	{
		return { CELL_VDEC_ERROR_SEQ, vdec->seq_state.load() };
	}

	const u64 seq_id = vdec->seq_id;
	const u64 cmd_id = vdec->next_cmd_id++;
	cellVdec.trace("Adding framerate cmd (handle=0x%x, seq_id=%d, cmd_id=%d)", handle, seq_id, cmd_id);

	vdec->in_cmd.push(vdec_cmd(vdec_cmd_type::framerate, seq_id, cmd_id, frameRateCode & 0x87));
	return CELL_OK;
}

error_code cellVdecOpenExt(ppu_thread& ppu, vm::cptr<CellVdecType> type, vm::cptr<CellVdecResourceExt> res, vm::cptr<CellVdecCb> cb, vm::ptr<u32> handle)
{
	cellVdec.warning("cellVdecOpenExt(type=*0x%x, res=*0x%x, cb=*0x%x, handle=*0x%x)", type, res, cb, handle);

	if (!res)
	{
		return { CELL_VDEC_ERROR_ARG, "res is nullptr" };
	}

	vm::var<CellVdecResource> tmp = vm::make_var<CellVdecResource>({});
	tmp->memAddr = res->memAddr;
	tmp->memSize = res->memSize;
	tmp->ppuThreadPriority = res->ppuThreadPriority;
	tmp->ppuThreadStackSize = res->ppuThreadStackSize;
	tmp->spuThreadPriority = 0;
	tmp->numOfSpus = res->numOfSpus;

	const vm::ptr<CellVdecResource> ptr = vm::cast(tmp.addr());
	return vdecOpen(ppu, type, ptr, cb, handle);
}

error_code cellVdecStartSeqExt()
{
	UNIMPLEMENTED_FUNC(cellVdec);
	return CELL_OK;
}

error_code cellVdecGetInputStatus()
{
	UNIMPLEMENTED_FUNC(cellVdec);
	return CELL_OK;
}

error_code cellVdecGetPicItemEx()
{
	UNIMPLEMENTED_FUNC(cellVdec);
	return CELL_OK;
}

error_code cellVdecGetPicItemExt()
{
	UNIMPLEMENTED_FUNC(cellVdec);
	return CELL_OK;
}

error_code cellVdecSetFrameRateExt()
{
	UNIMPLEMENTED_FUNC(cellVdec);
	return CELL_OK;
}

error_code cellVdecSetPts(u32 handle, vm::ptr<void> unk)
{
	cellVdec.error("cellVdecSetPts(handle=0x%x, unk=*0x%x)", handle, unk);

	const auto vdec = idm::get_unlocked<vdec_context>(handle);

	if (!vdec || !unk)
	{
		return { CELL_VDEC_ERROR_ARG, "vdec=%d, unk=%d", !!vdec, !!unk };
	}

	std::lock_guard lock{vdec->mutex};

	if (vdec->seq_state == sequence_state::closed || vdec->seq_state >= sequence_state::invalid)
	{
		return { CELL_VDEC_ERROR_SEQ, vdec->seq_state.load() };
	}

	return CELL_OK;
}

DECLARE(ppu_module_manager::cellVdec)("libvdec", []()
{
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(58, 10, 100)
	avcodec_register_all();
#endif

	static ppu_static_module libavcdec("libavcdec");
	static ppu_static_module libdivx311dec("libdivx311dec");
	static ppu_static_module libdivxdec("libdivxdec");
	static ppu_static_module libmvcdec("libmvcdec");
	static ppu_static_module libsjvtd("libsjvtd");
	static ppu_static_module libsmvd2("libsmvd2");
	static ppu_static_module libsmvd4("libsmvd4");
	static ppu_static_module libsvc1d("libsvc1d");

	REG_VAR(libvdec, _cell_vdec_prx_ver); // 0x085a7ecb

	REG_FUNC(libvdec, cellVdecQueryAttr);
	REG_FUNC(libvdec, cellVdecQueryAttrEx);
	REG_FUNC(libvdec, cellVdecOpen);
	REG_FUNC(libvdec, cellVdecOpenEx);
	REG_FUNC(libvdec, cellVdecOpenExt); // 0xef4d8ad7
	REG_FUNC(libvdec, cellVdecClose);
	REG_FUNC(libvdec, cellVdecStartSeq);
	REG_FUNC(libvdec, cellVdecStartSeqExt); // 0xebb8e70a
	REG_FUNC(libvdec, cellVdecEndSeq);
	REG_FUNC(libvdec, cellVdecDecodeAu);
	REG_FUNC(libvdec, cellVdecDecodeAuEx2);
	REG_FUNC(libvdec, cellVdecGetInputStatus);
	REG_FUNC(libvdec, cellVdecGetPicture);
	REG_FUNC(libvdec, cellVdecGetPictureExt); // 0xa21aa896
	REG_FUNC(libvdec, cellVdecGetPicItem);
	REG_FUNC(libvdec, cellVdecGetPicItemEx);
	REG_FUNC(libvdec, cellVdecGetPicItemExt); // 0x2cbd9806
	REG_FUNC(libvdec, cellVdecSetFrameRate);
	REG_FUNC(libvdec, cellVdecSetFrameRateExt); // 0xcffc42a5
	REG_FUNC(libvdec, cellVdecSetPts); // 0x3ce2e4f8

	REG_HIDDEN_FUNC(vdecEntry);
});
