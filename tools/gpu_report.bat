@echo off
rem Runs Reno's graphics card checks and timings, each into its own log, plus a summary:
rem   build\gpu_report\<date_time>\  (vkcheck_<effect>.txt, gpupick.txt, fxbench_<effect>.txt, summary.txt)
rem Usage: tools\gpu_report.bat [path\to\reno_cli.exe]
setlocal enabledelayedexpansion
cd /d "%~dp0.."

set "CLI=%~1"
if not defined CLI for %%p in ("build\vs\Release\reno_cli.exe" "build\clang\reno_cli.exe" "build\clang\Release\reno_cli.exe" "build\msvc\Release\reno_cli.exe" "bin\reno_cli.exe") do (
    if not defined CLI if exist %%p set "CLI=%%~p"
)
if not defined CLI (
    echo Could not find reno_cli.exe. Build Reno first, or give its path: tools\gpu_report.bat C:\path\to\reno_cli.exe
    exit /b 1
)

for /f %%t in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd_HHmm"') do set "TS=%%t"
set "OUT=build\gpu_report\%TS%"
mkdir "%OUT%" 2>nul
set "SUM=%OUT%\summary.txt"

echo Reno graphics card report %TS% > "%SUM%"
echo reno_cli: %CLI% >> "%SUM%"
echo. >> "%SUM%"
echo Writing the logs to %OUT%
echo.

rem ---- exactness: the card's pictures against the CPU's
for %%e in (panorotation colormatch tvsimulator lensflare mirror bzmasking dissolve) do (
    echo vkcheck %%e ...
    "%CLI%" vkcheck --only %%e > "%OUT%\vkcheck_%%e.txt" 2>&1
    echo ==== vkcheck %%e ^(exit !errorlevel!^) >> "%SUM%"
    findstr /i /c:"float controls" /c:"DIFFERS" /c:"comparisons" /c:"problem" /c:"error" /c:"failed" "%OUT%\vkcheck_%%e.txt" >> "%SUM%"
    echo. >> "%SUM%"
)

rem ---- which effects go to the card, measured afresh
echo gpupick --again ... (a few minutes)
"%CLI%" gpupick --again > "%OUT%\gpupick.txt" 2>&1
echo ==== gpupick ^(exit !errorlevel!^) >> "%SUM%"
findstr /i /c:"-> CPU" /c:"effects measured" /c:"Vulkan:" "%OUT%\gpupick.txt" >> "%SUM%"
echo. >> "%SUM%"

rem ---- where the time goes: upload, kernels, download, wait
for %%e in (invert gaussianblur dissolve median levels lightrays) do (
    echo fxbench %%e ...
    "%CLI%" fxbench --gpu --only %%e > "%OUT%\fxbench_%%e.txt" 2>&1
    echo ==== fxbench %%e ^(exit !errorlevel!^) >> "%SUM%"
    type "%OUT%\fxbench_%%e.txt" >> "%SUM%"
    echo. >> "%SUM%"
)

echo.
echo Done. Summary: %SUM%
echo (all logs in %OUT%)
endlocal
