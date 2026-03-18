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
echo Stages the locally built FFmpeg %RPCS3_FFMPEG_VERSION% headers and libs into 3rdparty/ffmpeg.
echo.
echo Usage:
echo   %~nx0 [--force] [--dry-run]
exit /b 0

:run
echo [ffmpeg] prefix root   : %PREFIX_ROOT%
echo [ffmpeg] bundle root   : %FFMPEG_BUNDLE_ROOT%
echo [ffmpeg] include stage : %FFMPEG_BUNDLE_INCLUDE%
echo [ffmpeg] lib stage     : %FFMPEG_BUNDLE_LIB%

if defined DRY_RUN exit /b 0

if not exist "%PREFIX_ROOT%\include" (
  echo Missing install include tree: %PREFIX_ROOT%\include
  exit /b 1
)

if not exist "%PREFIX_ROOT%\lib" (
  echo Missing install lib tree: %PREFIX_ROOT%\lib
  exit /b 1
)

if defined FORCE (
  if exist "%FFMPEG_BUNDLE_INCLUDE%" rmdir /s /q "%FFMPEG_BUNDLE_INCLUDE%"
  if exist "%FFMPEG_BUNDLE_LIB%" rmdir /s /q "%FFMPEG_BUNDLE_LIB%"
)

if not exist "%FFMPEG_BUNDLE_INCLUDE%" mkdir "%FFMPEG_BUNDLE_INCLUDE%" || exit /b 1
if not exist "%FFMPEG_BUNDLE_LIB%" mkdir "%FFMPEG_BUNDLE_LIB%" || exit /b 1

xcopy /E /I /Y "%PREFIX_ROOT%\include\*" "%FFMPEG_BUNDLE_INCLUDE%\" >nul || exit /b 1
xcopy /I /Y "%PREFIX_ROOT%\lib\*.lib" "%FFMPEG_BUNDLE_LIB%\" >nul || exit /b 1

echo [ffmpeg] staged.
exit /b 0
