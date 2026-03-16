@echo off
setlocal

set "ROOT=C:\Projects\RPCS3-buildable"

if not exist "%ROOT%\Summarize-MGS4-Benchmark.ps1" (
    echo Could not find benchmark summarizer at "%ROOT%\Summarize-MGS4-Benchmark.ps1"
    pause
    exit /b 1
)

echo.
echo MGS4 Benchmark Summary
echo.

set "SESSION=am5-affinity"
set /p SESSION=Benchmark session name [%SESSION%]:
if not defined SESSION set "SESSION=am5-affinity"

echo.
echo Generating summary...
powershell -ExecutionPolicy Bypass -File "%ROOT%\Summarize-MGS4-Benchmark.ps1" -SessionName "%SESSION%"
if errorlevel 1 goto :fail

echo.
echo Summary generated under:
echo   %ROOT%\bin\benchmarks\%SESSION%
pause
exit /b 0

:fail
echo.
echo Benchmark summary failed.
pause
exit /b 1
