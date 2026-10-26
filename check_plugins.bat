@echo off
rem Finds every OpenFX, VST and DirectX audio plug-in on this machine and tries each one in a process of its own.
rem Writes plugin_report.txt and plugin_report.json next to this file. Nothing is installed or changed.
rem Run it from a folder that has reno_cli.exe in bin\ or (after build_msvc.bat) in build\out\.
cd /d "%~dp0"
set "BIN=%~dp0bin"
if not exist "%BIN%\reno_cli.exe" set "BIN=%~dp0build\out"
if not exist "%BIN%\reno_cli.exe" (echo reno_cli.exe not found - build Reno first ^(build_msvc.bat^).& pause & exit /b 1)
"%BIN%\reno_cli.exe" check --out-dir "%~dp0."
echo.
echo The report is in plugin_report.txt
pause
