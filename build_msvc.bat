@echo off
rem Builds Reno with Visual Studio 2022 (its CMake and Ninja), Qt from C:\Qt\6.8.3\msvc2022_64 and FFmpeg from
rem deps\ffmpeg (tools\get_ffmpeg.ps1) or else C:\vcpkg\installed\x64-windows. Output goes to build\:
rem   build\msvc\   build files
rem   build\out\    reno.exe, reno_cli.exe, reno_vst32.exe (32-bit VST2 host), mediatest.exe and their DLLs
rem The log is also written to build_log.txt.
setlocal
cd /d "%~dp0"
set "QTDIR=C:\Qt\6.8.3\msvc2022_64"
set "FFDIR=C:\vcpkg\installed\x64-windows"
if exist "%~dp0deps\ffmpeg\include\libavformat\avformat.h" set "FFDIR=%~dp0deps\ffmpeg"
set "LOG=%~dp0build_log.txt"
> "%LOG%" echo Reno build %DATE% %TIME%

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (echo vswhere.exe not found - is Visual Studio installed?& goto failed)
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (echo No Visual Studio with the C++ tools was found.& goto failed)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
if not exist "%QTDIR%\bin\Qt6Core.dll" (echo Qt 6.8.3 msvc2022_64 is not in %QTDIR% - install it there.& goto failed)
if not exist "%FFDIR%\include\libavformat\avformat.h" (echo FFmpeg is not in %FFDIR%.& goto failed)

cmake -S . -B build\msvc -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="%QTDIR%" -DRENO_FFMPEG_DIR="%FFDIR%" >> "%LOG%" 2>&1
if errorlevel 1 (type "%LOG%" & goto failed)
cmake --build build\msvc >> "%LOG%" 2>&1
if errorlevel 1 (type "%LOG%" & goto failed)

if not exist build\out mkdir build\out
for %%f in (reno.exe reno_cli.exe reno_vst32.exe mediatest.exe) do copy /Y "build\msvc\bin\%%f" build\out\ >nul
rem Qt's DLLs and plug-ins (without Qt's FFmpeg backend; Reno's own FFmpeg is copied below)
"%QTDIR%\bin\windeployqt.exe" --release --no-translations --no-compiler-runtime --exclude-plugins ffmpegmediaplugin build\out\reno.exe >> "%LOG%" 2>&1
rem reno_cli.exe needs only Qt Core and Gui, which the reno.exe deployment already copied
for %%p in (av sw postproc) do if exist "%FFDIR%\bin\%%p*.dll" copy /Y "%FFDIR%\bin\%%p*.dll" build\out\ >nul
rem Tools > Scripting: the ScriptPortal.Vegas source (compiled on first use) and an empty Script Menu folder
if not exist build\out\scripting mkdir build\out\scripting
copy /Y src\scripting\*.cs build\out\scripting\ >nul
copy /Y README.md build\out\ >nul
if not exist "build\out\Script Menu" mkdir "build\out\Script Menu"
echo.
echo Built: build\out\reno.exe
echo Built: build\out\reno_cli.exe (command-line renderer and plug-in checker: reno_cli --help)
echo Built: build\out\reno_vst32.exe (runs 32-bit VST2 plug-ins for reno.exe)
echo Built: build\out\reno.exe>> "%LOG%"
pause
exit /b 0

:failed
echo.
echo Build failed. The details are in build_log.txt.
echo Build failed.>> "%LOG%"
pause
exit /b 1
