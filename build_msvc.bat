@echo off
rem Builds Reno with Visual Studio 2022's clang-cl (its "C++ Clang tools for Windows" component), CMake and Ninja, Qt
rem from C:\Qt\6.8.3\msvc2022_64 and FFmpeg from deps\ffmpeg (tools\get_ffmpeg.ps1) or else C:\vcpkg\installed\x64-windows.
rem "build_msvc.bat msvc" builds with MSVC's own compiler instead (as does a Visual Studio without the Clang tools).
rem Output goes to build\:
rem   build\clang\  build files (build\msvc\ with MSVC)
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
rem the compiler: clang-cl from Visual Studio (or another LLVM on the PATH), unless "msvc" was asked for
set "COMPILER="
set "BDIR=build\msvc"
if /i "%~1"=="msvc" goto compilerchosen
if exist "%VSDIR%\VC\Tools\Llvm\x64\bin\clang-cl.exe" set "COMPILER=%VSDIR%\VC\Tools\Llvm\x64\bin\clang-cl.exe"
if not defined COMPILER for /f "delims=" %%c in ('where clang-cl 2^>nul') do if not defined COMPILER set "COMPILER=%%c"
if defined COMPILER goto haveclang
echo clang-cl was not found: add "C++ Clang tools for Windows" in the Visual Studio Installer to build with it.
echo Building with MSVC instead.
goto compilerchosen
:haveclang
set "BDIR=build\clang"
:compilerchosen
if defined COMPILER echo Compiler: clang-cl, %COMPILER%
if not defined COMPILER echo Compiler: MSVC
>> "%LOG%" echo Compiler: %COMPILER%
if not exist "%QTDIR%\bin\Qt6Core.dll" (echo Qt 6.8.3 msvc2022_64 is not in %QTDIR% - install it there.& goto failed)
if not exist "%FFDIR%\include\libavformat\avformat.h" (echo FFmpeg is not in %FFDIR%.& goto failed)

set "CC_ARGS="
if defined COMPILER set CC_ARGS=-DCMAKE_C_COMPILER="%COMPILER%" -DCMAKE_CXX_COMPILER="%COMPILER%"
cmake -S . -B %BDIR% -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="%QTDIR%" -DRENO_FFMPEG_DIR="%FFDIR%" %CC_ARGS% >> "%LOG%" 2>&1
if errorlevel 1 (type "%LOG%" & goto failed)
cmake --build %BDIR% >> "%LOG%" 2>&1
if errorlevel 1 (type "%LOG%" & goto failed)

if not exist build\out mkdir build\out
for %%f in (reno.exe reno_cli.exe reno_vst32.exe mediatest.exe) do copy /Y "%BDIR%\bin\%%f" build\out\ >nul
rem Qt's DLLs and plug-ins (without Qt's FFmpeg backend; Reno's own FFmpeg is copied below)
rem and without what Reno never uses: the software OpenGL fallback, Direct3D shader compilers, SVG and PDF, network
rem and touch plug-ins, and the image formats it does not read (the color-match picker reads png jpg bmp tif gif webp)
"%QTDIR%\bin\windeployqt.exe" --release --no-translations --no-compiler-runtime --no-opengl-sw --no-system-d3d-compiler --no-system-dxc-compiler --skip-plugin-types generic,networkinformation,tls,iconengines --exclude-plugins ffmpegmediaplugin,qsvg,qico,qicns,qtga,qwbmp,qpdf,qdirect2d,qminimal,qoffscreen build\out\reno.exe >> "%LOG%" 2>&1
rem (leftovers of an earlier, fuller deployment into the same folder)
for %%d in (iconengines generic networkinformation tls) do if exist "build\out\%%d" rmdir /s /q "build\out\%%d"
for %%f in (Qt6Svg Qt6Pdf Qt6OpenGLWidgets opengl32sw D3Dcompiler_47 dxcompiler dxil avdevice-* avfilter-* postproc-*) do del /q "build\out\%%f*.dll" 2>nul
for %%f in (qsvg qico qicns qtga qwbmp qpdf) do del /q "build\out\imageformats\%%f.dll" 2>nul
rem reno_cli.exe needs only Qt Core and Gui, which the reno.exe deployment already copied
for %%p in (avcodec avformat avutil swscale swresample) do if exist "%FFDIR%\bin\%%p*.dll" copy /Y "%FFDIR%\bin\%%p*.dll" build\out\ >nul
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
