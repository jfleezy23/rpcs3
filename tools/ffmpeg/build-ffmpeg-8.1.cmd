@echo off
setlocal EnableExtensions

set "SCRIPT_DIR=%~dp0"

call "%SCRIPT_DIR%ffmpeg-env.cmd"

set "CLEAN="
set "DRY_RUN="
set "NO_FETCH="

:parse
if "%~1"=="" goto :run
if /I "%~1"=="--clean" set "CLEAN=1" & shift & goto :parse
if /I "%~1"=="--dry-run" set "DRY_RUN=1" & shift & goto :parse
if /I "%~1"=="--no-fetch" set "NO_FETCH=1" & shift & goto :parse
if /I "%~1"=="--help" goto :help
echo Unknown option: %~1
exit /b 1

:help
echo Builds FFmpeg %RPCS3_FFMPEG_VERSION% with the FMV-branch assumptions ^(clang-cl, avfilter, libplacebo, Vulkan^) using cmd + MSYS2/bash.
echo.
echo Usage:
echo   %~nx0 [--clean] [--no-fetch] [--dry-run]
exit /b 0

:run
if not defined NO_FETCH (
  if defined DRY_RUN (
    call "%SCRIPT_DIR%fetch-ffmpeg-8.1.cmd" --dry-run || exit /b 1
  ) else (
    call "%SCRIPT_DIR%fetch-ffmpeg-8.1.cmd" || exit /b 1
  )
)

if exist "%VS_VCVARS%" goto :vs_ok
echo Missing vcvars64.bat: %VS_VCVARS%
exit /b 1

:vs_ok
if exist "%BASH_EXE%" goto :bash_ok
echo Missing MSYS2 bash.exe: %BASH_EXE%
exit /b 1

:bash_ok
if exist "%PKG_CONFIG%" goto :pkg_ok
echo Missing pkg-config.exe: %PKG_CONFIG%
exit /b 1

:pkg_ok

echo [ffmpeg] source root : %SOURCE_ROOT%
echo [ffmpeg] build root  : %BUILD_ROOT%
echo [ffmpeg] prefix root : %PREFIX_ROOT%
echo [ffmpeg] llvm bin    : %LLVM_BIN%
echo [ffmpeg] bash        : %BASH_EXE%
echo [ffmpeg] pkg-config  : %PKG_CONFIG%

if defined DRY_RUN exit /b 0

if defined CLEAN (
  if exist "%BUILD_ROOT%" rmdir /s /q "%BUILD_ROOT%"
  if exist "%PREFIX_ROOT%" rmdir /s /q "%PREFIX_ROOT%"
)

if not exist "%BUILD_ROOT%" mkdir "%BUILD_ROOT%" || exit /b 1
if not exist "%PREFIX_ROOT%" mkdir "%PREFIX_ROOT%" || exit /b 1

call "%VS_VCVARS%" >nul || exit /b 1
set "PATH=%LLVM_BIN%;%PATH%"
set "CC=clang-cl"
set "CXX=clang-cl"
set "AR=llvm-lib"
set "LD=lld-link"
set "PKG_CONFIG_PATH=%PKG_CONFIG_PATH_WIN%"

set "BASH_COMMAND=set -euo pipefail; "
set "BASH_COMMAND=%BASH_COMMAND%export PATH=\"%LLVM_BIN%:$PATH\"; "
set "BASH_COMMAND=%BASH_COMMAND%export CC=clang-cl; export CXX=clang-cl; export AR=llvm-lib; export LD=lld-link; "
set "BASH_COMMAND=%BASH_COMMAND%export PKG_CONFIG=\"%PKG_CONFIG%\"; export PKG_CONFIG_PATH=\"%PKG_CONFIG_PATH_WIN%\"; "
set "BASH_COMMAND=%BASH_COMMAND%cd \"%BUILD_ROOT%\"; "
set "BASH_COMMAND=%BASH_COMMAND%\"%SOURCE_ROOT%/configure\" "
set "BASH_COMMAND=%BASH_COMMAND%--toolchain=msvc --arch=x86_64 --target-os=win64 --disable-programs --disable-doc --disable-debug --enable-static --disable-shared --enable-avfilter --enable-swresample --enable-swscale --enable-libplacebo --enable-vulkan --prefix=\"%PREFIX_ROOT%\"; "
set "BASH_COMMAND=%BASH_COMMAND%make -j%NUMBER_OF_PROCESSORS%; make install"

"%BASH_EXE%" -lc "%BASH_COMMAND%" || exit /b 1

echo [ffmpeg] build complete.
exit /b 0
