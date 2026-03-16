@echo off
setlocal

set "ROOT=C:\Projects\RPCS3-buildable"

if not exist "%ROOT%\Start-MGS4-Benchmark.ps1" (
    echo Could not find benchmark launcher at "%ROOT%\Start-MGS4-Benchmark.ps1"
    pause
    exit /b 1
)

echo.
echo MGS4 Benchmark Launcher
echo.

set "SESSION=am5-affinity"
set /p SESSION=Benchmark session name [%SESSION%]:
if not defined SESSION set "SESSION=am5-affinity"

set "VARIANT=current"
set /p VARIANT=Variant label [%VARIANT%]:
if not defined VARIANT set "VARIANT=current"

set "SCENE=general"
set /p SCENE=Scene label [%SCENE%]:
if not defined SCENE set "SCENE=general"

echo.
echo Enabling performance reporting in the MGS4 profile...
powershell -ExecutionPolicy Bypass -File "%ROOT%\Set-MGS4-Profile.ps1" -EnablePerfReport
if errorlevel 1 goto :fail

echo.
echo Launching RPCS3 benchmark run...
powershell -ExecutionPolicy Bypass -File "%ROOT%\Start-MGS4-Benchmark.ps1" -SessionName "%SESSION%" -Variant "%VARIANT%" -Scene "%SCENE%"
if errorlevel 1 goto :fail

echo.
echo Benchmark run finished.
echo When you are ready, click "Summarize MGS4 Benchmark.cmd" to generate the report.
pause
exit /b 0

:fail
echo.
echo Benchmark launcher failed.
pause
exit /b 1
