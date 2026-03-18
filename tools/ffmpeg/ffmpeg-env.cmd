@echo off
setlocal

set "SCRIPT_DIR=%~dp0"
for %%I in ("%SCRIPT_DIR%..\..") do set "RPCS3_ROOT=%%~fI"

if not defined RPCS3_FFMPEG_VERSION (
  set "RPCS3_FFMPEG_VERSION=8.1"
)

if not defined RPCS3_VCPKG_ROOT (
  set "RPCS3_VCPKG_ROOT=C:\Projects\vcpkg"
)

set "FFMPEG_PACKAGE=ffmpeg-%RPCS3_FFMPEG_VERSION%"
set "FFMPEG_URL=https://ffmpeg.org/releases/%FFMPEG_PACKAGE%.tar.xz"
set "DEPS_ROOT=%RPCS3_ROOT%\build\deps"
set "DOWNLOAD_ROOT=%DEPS_ROOT%\downloads"
set "ARCHIVE_PATH=%DOWNLOAD_ROOT%\%FFMPEG_PACKAGE%.tar.xz"
set "SOURCE_ROOT=%DEPS_ROOT%\%FFMPEG_PACKAGE%"
set "BUILD_ROOT=%DEPS_ROOT%\%FFMPEG_PACKAGE%-build"
set "PREFIX_ROOT=%DEPS_ROOT%\%FFMPEG_PACKAGE%-prefix"
set "LIBPLACEBO_PREFIX=%RPCS3_ROOT%\3rdparty\libplacebo-local"
set "PKGCONFIG_LOCAL=%RPCS3_ROOT%\3rdparty\pkgconfig-local"
set "FFMPEG_BUNDLE_ROOT=%RPCS3_ROOT%\3rdparty\ffmpeg"
set "FFMPEG_BUNDLE_LIB=%FFMPEG_BUNDLE_ROOT%\lib\windows\x86_64"
set "FFMPEG_BUNDLE_INCLUDE=%FFMPEG_BUNDLE_ROOT%\include"

if not defined LLVM_BIN (
  set "LLVM_BIN=C:\Program Files\LLVM\bin"
)

if not defined VS_VCVARS (
  set "VS_VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
)

if not defined MSYS2_ROOT (
  for /d %%D in ("%RPCS3_VCPKG_ROOT%\downloads\tools\msys2\*") do (
    if exist "%%~fD\usr\bin\bash.exe" (
      set "MSYS2_ROOT=%%~fD"
      goto :msys_found
    )
  )
)

:msys_found
if not defined BASH_EXE (
  set "BASH_EXE=%MSYS2_ROOT%\usr\bin\bash.exe"
)

if not defined PKG_CONFIG (
  if exist "%MSYS2_ROOT%\mingw64\bin\pkg-config.exe" (
    set "PKG_CONFIG=%MSYS2_ROOT%\mingw64\bin\pkg-config.exe"
  ) else if exist "%MSYS2_ROOT%\usr\bin\pkg-config.exe" (
    set "PKG_CONFIG=%MSYS2_ROOT%\usr\bin\pkg-config.exe"
  ) else (
    for /r "%RPCS3_VCPKG_ROOT%\downloads\tools\msys2" %%F in (pkg-config.exe) do (
      set "PKG_CONFIG=%%~fF"
      goto :pkg_found
    )
  )
)

:pkg_found
if not defined PKG_CONFIG_PATH_WIN (
  set "PKG_CONFIG_PATH_WIN=%PKGCONFIG_LOCAL%;%RPCS3_VCPKG_ROOT%\installed\x64-win-llvm-static-release\lib\pkgconfig"
)

endlocal & (
  set "RPCS3_ROOT=%RPCS3_ROOT%"
  set "RPCS3_FFMPEG_VERSION=%RPCS3_FFMPEG_VERSION%"
  set "RPCS3_VCPKG_ROOT=%RPCS3_VCPKG_ROOT%"
  set "FFMPEG_PACKAGE=%FFMPEG_PACKAGE%"
  set "FFMPEG_URL=%FFMPEG_URL%"
  set "DEPS_ROOT=%DEPS_ROOT%"
  set "DOWNLOAD_ROOT=%DOWNLOAD_ROOT%"
  set "ARCHIVE_PATH=%ARCHIVE_PATH%"
  set "SOURCE_ROOT=%SOURCE_ROOT%"
  set "BUILD_ROOT=%BUILD_ROOT%"
  set "PREFIX_ROOT=%PREFIX_ROOT%"
  set "LIBPLACEBO_PREFIX=%LIBPLACEBO_PREFIX%"
  set "PKGCONFIG_LOCAL=%PKGCONFIG_LOCAL%"
  set "FFMPEG_BUNDLE_ROOT=%FFMPEG_BUNDLE_ROOT%"
  set "FFMPEG_BUNDLE_LIB=%FFMPEG_BUNDLE_LIB%"
  set "FFMPEG_BUNDLE_INCLUDE=%FFMPEG_BUNDLE_INCLUDE%"
  set "LLVM_BIN=%LLVM_BIN%"
  set "VS_VCVARS=%VS_VCVARS%"
  set "MSYS2_ROOT=%MSYS2_ROOT%"
  set "BASH_EXE=%BASH_EXE%"
  set "PKG_CONFIG=%PKG_CONFIG%"
  set "PKG_CONFIG_PATH_WIN=%PKG_CONFIG_PATH_WIN%"
)
