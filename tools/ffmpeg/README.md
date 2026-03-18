# FFmpeg 8.1 Toolbox

These scripts set up a dependency-focused workflow for the FMV branch without relying on PowerShell for the fragile parts.

Current target:
- FFmpeg `8.1`
- local FMV branch at `C:\Projects\RPCS3-fmv-lab`

Why these exist:
- normal RPCS3 builds are fine through PowerShell
- FFmpeg/libplacebo packaging was where quoting, env activation, and shell behavior got messy
- this toolbox gives us a cleaner dependency lane before the actual `7.1.2 -> 8.1` upgrade work starts

Scripts:
- `ffmpeg-env.cmd`
  - resolves the repo root, dependency workspace, local libplacebo helper paths, LLVM, MSYS2 bash, and pkg-config
- `fetch-ffmpeg-8.1.cmd`
  - downloads and extracts the official FFmpeg `8.1` source tarball into `build\deps`
- `build-ffmpeg-8.1.cmd`
  - drives a local FFmpeg build using `cmd` + MSYS2 bash + clang-cl
- `stage-ffmpeg-8.1.cmd`
  - stages the installed headers and `.lib` files into `3rdparty\ffmpeg`

Typical flow:

```cmd
tools\ffmpeg\fetch-ffmpeg-8.1.cmd
tools\ffmpeg\build-ffmpeg-8.1.cmd
tools\ffmpeg\stage-ffmpeg-8.1.cmd --force
```

Dry-run helpers:

```cmd
tools\ffmpeg\fetch-ffmpeg-8.1.cmd --dry-run
tools\ffmpeg\build-ffmpeg-8.1.cmd --dry-run
tools\ffmpeg\stage-ffmpeg-8.1.cmd --dry-run
```

Notes:
- These scripts are intentionally dependency-focused. They do not rebuild RPCS3.
- The FFmpeg build script assumes the same broad toolchain shape that already worked on this branch:
  - Visual Studio Build Tools
  - LLVM/clang-cl
  - MSYS2 bash from the local vcpkg tool cache
  - local `libplacebo` helper payload under `3rdparty\libplacebo-local`
- This is the tooling foundation for the upgrade, not the full upgrade itself.
