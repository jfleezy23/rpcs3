@echo off
setlocal EnableExtensions

set "SCRIPT_DIR=%~dp0"

call "%SCRIPT_DIR%ffmpeg-env.cmd"

set "FORCE="
set "DRY_RUN="

:parse
if "%~1"=="" goto :run
if /I "%~1"=="--force" set "FORCE=1" & shift & goto :parse
if /I "%~1"=="--dry-run" set "DRY_RUN=1" & shift & goto :parse
if /I "%~1"=="--help" goto :help
echo Unknown option: %~1
exit /b 1

:help
echo Fetches the official FFmpeg %RPCS3_FFMPEG_VERSION% source tarball and extracts it into the local dependency workspace.
echo.
echo Usage:
echo   %~nx0 [--force] [--dry-run]
exit /b 0

:run
echo [ffmpeg] version     : %RPCS3_FFMPEG_VERSION%
echo [ffmpeg] url         : %FFMPEG_URL%
echo [ffmpeg] archive     : %ARCHIVE_PATH%
echo [ffmpeg] source root : %SOURCE_ROOT%

if defined DRY_RUN exit /b 0

if not exist "%DOWNLOAD_ROOT%" mkdir "%DOWNLOAD_ROOT%" || exit /b 1
if not exist "%DEPS_ROOT%" mkdir "%DEPS_ROOT%" || exit /b 1

if defined FORCE (
  if exist "%ARCHIVE_PATH%" del /f /q "%ARCHIVE_PATH%" >nul 2>nul
  if exist "%SOURCE_ROOT%" rmdir /s /q "%SOURCE_ROOT%"
)

if not exist "%ARCHIVE_PATH%" (
  echo [ffmpeg] downloading archive...
  curl.exe -L --fail --output "%ARCHIVE_PATH%" "%FFMPEG_URL%" || exit /b 1
)

if not exist "%SOURCE_ROOT%" (
  echo [ffmpeg] extracting archive...
  tar.exe -xf "%ARCHIVE_PATH%" -C "%DEPS_ROOT%" || exit /b 1
)

echo [ffmpeg] ready.
exit /b 0
