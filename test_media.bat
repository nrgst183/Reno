@echo off
rem Writes short test clips with each container, reads them back and checks them.
rem Output goes to media_test_out next to this file. Needs mediatest.exe in bin\ or (after build_msvc.bat) in build\out\.
cd /d "%~dp0"
set "BIN=%~dp0bin"
if not exist "%BIN%\mediatest.exe" set "BIN=%~dp0build\out"
if not exist "%BIN%\mediatest.exe" (echo mediatest.exe not found - build Reno first ^(build_msvc.bat^).& pause & exit /b 1)
if not exist media_test_out mkdir media_test_out
"%BIN%\mediatest.exe" "%~dp0media_test_out"
echo.
echo The log is in media_test_out\mediatest_log.txt
pause
