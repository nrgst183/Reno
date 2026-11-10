cmake_minimum_required(VERSION 3.21)
# macOS: runs on macOS 11 and later (the first with Apple Silicon); the architecture is the machine's own unless
# CMAKE_OSX_ARCHITECTURES says otherwise (arm64 or x86_64; FFmpeg from Homebrew is one architecture only)
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin" AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Oldest macOS Reno runs on")
endif()
project(Reno LANGUAGES CXX)

# Reno builds on Windows (Visual Studio 2022 with clang-cl - the default in build_msvc.bat and Reno.sln - or MSVC), Linux (GCC or Clang) and macOS (Apple Clang), and
# cross-compiles for Windows from Linux with clang-cl (cmake/toolchains/windows-clangcl.cmake). It also builds for FreeBSD
# (as on Linux), for Windows 7 (RENO_WINDOWS7, then Qt DLLs patched to run there) and, with Qt for Android / iOS and FFmpeg
# built for them, for Android and iOS (qt-cmake; the editor alone, without reno_cli, the tests or other programs' plug-ins
# that need a desktop system). README.md has the steps for each.
# CMakePresets.json has a preset for each; Visual Studio opens this folder directly (File > Open > Folder).
# Reno.sln is the same build as Visual Studio projects (vs\).

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()
# all programs in one folder, so reno.exe finds reno_cli.exe, scripting\ and its DLLs
if(NOT CMAKE_RUNTIME_OUTPUT_DIRECTORY)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()

option(RENO_WITH_FFMPEG "Build the media reader/writer (needs the FFmpeg development files)" ON)
option(RENO_WITH_APP "Build the editor and reno_cli (needs Qt 6)" ON)
option(RENO_WITH_TESTS "Build the command-line test programs (mediatest, vegasfiles_test)" ON)
option(RENO_WITH_VST32_BRIDGE "Windows: also build reno_vst32.exe, the 32-bit program that runs 32-bit VST2 plug-ins (needs Visual Studio's x86 tools)" ON)
option(RENO_DEPLOY "Windows: after building, copy Qt's and FFmpeg's DLLs beside reno.exe (windeployqt)" ON)
option(RENO_WINDOWS7 "Windows: build for Windows 7 and later (no Windows 8/10 functions; then tools/use_qt_win7.ps1 puts in Qt DLLs that run there)" OFF)
set(RENO_FFMPEG_DIR "" CACHE PATH "Folder holding FFmpeg's include and lib folders (default: deps/ffmpeg from tools/get_ffmpeg.*, then vcpkg)")

# Android and iOS: the editor only (no command-line programs, tests or the 32-bit plug-in program there)
if(ANDROID OR IOS)
  set(RENO_WITH_TESTS OFF CACHE BOOL "" FORCE)
  set(RENO_WITH_VST32_BRIDGE OFF CACHE BOOL "" FORCE)
  set(RENO_DEPLOY OFF CACHE BOOL "" FORCE)
endif()
if(WIN32 AND RENO_WINDOWS7)
  # the Windows headers then declare only what Windows 7 has, so nothing newer can be called by mistake
  add_compile_definitions(_WIN32_WINNT=0x0601 WINVER=0x0601 NTDDI_VERSION=0x06010000 RENO_WINDOWS7)
endif()

if(MSVC)
  # (clang-cl takes the same options; Qt's targets add /Zc:__cplusplus /permissive- to the programs that use Qt)
  add_compile_options(/W3 /utf-8 /bigobj)
  if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    add_compile_options(/MP)
  else()
    # clang-cl (the default on Windows: build_msvc.bat, Reno.sln's ClangCL toolset, CI): no fused multiply-adds, so
    # floating-point results stay what the effects were checked with (VEGAS's own code does not fuse them either)
    add_compile_options(/clang:-ffp-contract=off)
  endif()
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX UNICODE _UNICODE)
else()
  add_compile_options(-Wall -Wextra -Wno-unused-parameter)
  # no fused multiply-adds: on ARM (Apple Silicon) compilers fuse a*b+c by default, which changes results by a rounding
  # step; the effects are made and checked to match VEGAS without them
  add_compile_options(-ffp-contract=off)
  add_compile_definitions(__cdecl=)          # the VST2 header names a Windows calling convention
endif()

# Source lists are the folders themselves, so a new .cpp is built without editing this file.
function(reno_sources out)
  set(files "")
  foreach(dir ${ARGN})
    file(GLOB found CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/${dir}/*.cpp)
    list(APPEND files ${found})
  endforeach()
  list(SORT files)
  set(${out} ${files} PARENT_SCOPE)
endfunction()

# ---- the plug-in hosts and shared code: no Qt, no FFmpeg
reno_sources(RENO_CORE_SOURCES core ofx audiofx)
add_library(reno_core STATIC ${RENO_CORE_SOURCES})
target_include_directories(reno_core PUBLIC third_party/openfx third_party/vst2 src/ofx)
find_package(Threads REQUIRED)
target_link_libraries(reno_core PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
if(APPLE)
  target_link_libraries(reno_core PUBLIC "-framework CoreFoundation")   # VST3 bundles (bundleEntry)
endif()
if(WIN32)
  target_link_libraries(reno_core PUBLIC msdmo dmoguids strmiids wmcodecdspuuid ole32 oleaut32 uuid shell32 shlwapi
                                         advapi32 user32 gdi32 comdlg32 comctl32 version propsys winmm ws2_32)
endif()

# ---- media files through FFmpeg
if(RENO_WITH_FFMPEG)
  set(_ff_hints ${RENO_FFMPEG_DIR} $ENV{FFMPEG_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/deps/ffmpeg   # (tools/get_ffmpeg.*)
                C:/vcpkg/installed/x64-windows /opt/homebrew /usr/local)
  # (Android / iOS: FFmpeg built for the device is outside the system root the cross-compiler searches)
  set(_ff_mode "")
  if((ANDROID OR IOS) AND (RENO_FFMPEG_DIR OR DEFINED ENV{FFMPEG_DIR}))
    set(_ff_mode NO_CMAKE_FIND_ROOT_PATH)
  endif()
  find_path(FFMPEG_INCLUDE_DIR libavformat/avformat.h HINTS ${_ff_hints} PATH_SUFFIXES include ${_ff_mode})
  set(FFMPEG_LIBS "")
  foreach(lib avformat avcodec avutil swscale swresample)
    find_library(FFMPEG_${lib}_LIB ${lib} HINTS ${_ff_hints} PATH_SUFFIXES lib ${_ff_mode})
    list(APPEND FFMPEG_LIBS ${FFMPEG_${lib}_LIB})
  endforeach()
  if(FFMPEG_INCLUDE_DIR AND FFMPEG_avformat_LIB AND FFMPEG_avcodec_LIB AND FFMPEG_avutil_LIB AND FFMPEG_swscale_LIB AND FFMPEG_swresample_LIB)
    reno_sources(RENO_MEDIA_SOURCES media)
    add_library(reno_media STATIC ${RENO_MEDIA_SOURCES})
    target_include_directories(reno_media PUBLIC ${FFMPEG_INCLUDE_DIR})
    target_link_libraries(reno_media PUBLIC reno_core ${FFMPEG_LIBS})
    if(WIN32)
      target_link_libraries(reno_media PUBLIC mfplat mfuuid)
    endif()
    if(IOS)
      # FFmpeg built for iOS is static (tools/build_ffmpeg_ios.sh): what its libraries use from the system
      target_link_libraries(reno_media PUBLIC "-framework VideoToolbox" "-framework AudioToolbox" "-framework CoreMedia"
                            "-framework CoreVideo" "-framework CoreFoundation" "-framework Security" z bz2 iconv)
    endif()
    if(RENO_WITH_TESTS)
      add_executable(mediatest tests/media/mediatest.cpp)
      target_link_libraries(mediatest PRIVATE reno_media)
      add_executable(vegasfiles_test tests/media/vegasfiles_test.cpp)
      target_link_libraries(vegasfiles_test PRIVATE reno_media)
    endif()
    message(STATUS "Reno: FFmpeg found in ${FFMPEG_INCLUDE_DIR}")
  else()
    message(WARNING "Reno: FFmpeg was not found (set RENO_FFMPEG_DIR); building without media files")
  endif()
endif()

# ---- the engine, the editor and reno_cli (Qt 6: Widgets, and Multimedia for sound)
if(RENO_WITH_APP AND TARGET reno_media)
  if(WIN32 AND NOT CMAKE_PREFIX_PATH AND NOT CMAKE_CROSSCOMPILING)
    set(CMAKE_PREFIX_PATH "C:/Qt/6.8.3/msvc2022_64")
  endif()
  find_package(Qt6 COMPONENTS Widgets OPTIONAL_COMPONENTS Multimedia OpenGL)
  if(Qt6_FOUND)
    # the project, the compositor and mixer, rendering; .veg reading and writing (no Qt)
    reno_sources(RENO_ENGINE_SOURCES engine vegas)
    add_library(reno_engine STATIC ${RENO_ENGINE_SOURCES})
    target_link_libraries(reno_engine PUBLIC reno_media)
    if(RENO_WITH_TESTS)
      # the Vulkan path of the built-in effects: the loader and device (skips without a driver), and CPU against GPU per effect
      add_executable(vk_test tests/gpu/vk_test.cpp)
      target_link_libraries(vk_test PRIVATE reno_engine)
      add_executable(vkfx_test tests/gpu/vkfx_test.cpp)
      target_link_libraries(vkfx_test PRIVATE reno_engine)
    endif()

    # the editor: every file of src/app (no moc: the interface code uses no Q_OBJECT classes)
    reno_sources(RENO_APP_SOURCES app)
    if(ANDROID OR IOS)
      # Qt's own way to make the Android package (a library the Java side starts) or the iOS app
      qt_add_executable(reno ${RENO_APP_SOURCES})
    else()
      add_executable(reno WIN32 ${RENO_APP_SOURCES})
    endif()
    if(WIN32)
      target_sources(reno PRIVATE resources/reno.rc)
    endif()
    if(ANDROID)
      # FFmpeg's libraries go into the package beside Reno's (when they are shared ones)
      set(_ff_so "")
      foreach(l ${FFMPEG_LIBS})
        if(l MATCHES "\\.so$")
          list(APPEND _ff_so ${l})
        endif()
      endforeach()
      set_target_properties(reno PROPERTIES QT_ANDROID_EXTRA_LIBS "${_ff_so}" QT_ANDROID_PACKAGE_NAME org.reno.editor
        QT_ANDROID_APP_NAME Reno)
    elseif(IOS)
      # (Qt's own Info.plist for iOS, with its launch screen)
      set_target_properties(reno PROPERTIES MACOSX_BUNDLE ON OUTPUT_NAME Reno
        MACOSX_BUNDLE_BUNDLE_NAME Reno MACOSX_BUNDLE_GUI_IDENTIFIER org.reno.editor
        MACOSX_BUNDLE_BUNDLE_VERSION 1.0 MACOSX_BUNDLE_SHORT_VERSION_STRING 1.0
        XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2")
    elseif(APPLE)
      # macOS: Reno.app, with reno_cli, scripting/ and the README beside the program in Contents/MacOS (where Reno looks
      # for them on every system); package it with macdeployqt (it copies Qt and FFmpeg into the bundle)
      set_target_properties(reno PROPERTIES MACOSX_BUNDLE ON OUTPUT_NAME Reno
        MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/resources/Info.plist.in
        MACOSX_BUNDLE_BUNDLE_NAME Reno MACOSX_BUNDLE_GUI_IDENTIFIER org.reno.editor
        MACOSX_BUNDLE_BUNDLE_VERSION 1.0 MACOSX_BUNDLE_SHORT_VERSION_STRING 1.0)
    endif()
    target_link_libraries(reno PRIVATE reno_engine Qt6::Widgets)
    # effects' on-screen controls are drawn with OpenGL (Qt's offscreen framebuffer; no OpenGL widgets are used)
    if(TARGET Qt6::OpenGL)
      target_link_libraries(reno PRIVATE Qt6::OpenGL)
    endif()
    if(WIN32)
      target_link_libraries(reno PRIVATE dwmapi opengl32)   # the dark title bars; OpenGL
    endif()
    if(TARGET Qt6::Multimedia)
      target_link_libraries(reno PRIVATE Qt6::Multimedia)
      target_compile_definitions(reno PRIVATE RENO_HAVE_QTMULTIMEDIA)
    else()
      message(WARNING "Reno: Qt Multimedia not found - the editor will play without sound")
    endif()

    # reno_cli: command-line renderer and plug-in checker (Qt Gui for the built-in text fonts, no widgets);
    # reno.exe runs it to try each new plug-in file in a separate process (src/cli/plugincheck.cpp)
    if(NOT (ANDROID OR IOS))
    add_executable(reno_cli src/cli/main.cpp src/cli/plugincheck.cpp
                            src/app/appstate.cpp src/app/builtintext.cpp src/app/rendertemplates.cpp src/app/logo.cpp)
    if(WIN32)
      target_sources(reno_cli PRIVATE resources/reno.rc)
    endif()
    target_link_libraries(reno_cli PRIVATE reno_engine Qt6::Gui)
    if(APPLE)
      set_target_properties(reno_cli PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/Reno.app/Contents/MacOS)
    endif()
    endif()

    # Tools > Scripting: the ScriptPortal.Vegas source (compiled on first use) and an empty Script Menu folder, next to reno
    file(GLOB RENO_SCRIPTING_FILES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/scripting/*.cs)
    if(IOS)
      set(_renodata $<TARGET_BUNDLE_CONTENT_DIR:reno>)               # (iOS: the bundle has no Contents folder)
    elseif(APPLE)
      set(_renodata $<TARGET_BUNDLE_CONTENT_DIR:reno>/Resources)    # (macOS: data files go in Contents/Resources, not beside the program)
    else()
      set(_renodata $<TARGET_FILE_DIR:reno>)
    endif()
    add_custom_command(TARGET reno POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E make_directory ${_renodata}/scripting "${_renodata}/Script Menu"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different ${RENO_SCRIPTING_FILES} ${_renodata}/scripting
      COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/README.md ${_renodata}
      VERBATIM)

    # Windows: Qt's and FFmpeg's DLLs next to reno.exe, so it runs from the build folder and the debugger
    if(WIN32 AND RENO_DEPLOY AND NOT CMAKE_CROSSCOMPILING)
      get_target_property(_qmake Qt6::qmake IMPORTED_LOCATION)
      get_filename_component(_qtbin "${_qmake}" DIRECTORY)
      find_program(WINDEPLOYQT windeployqt HINTS "${_qtbin}")
      if(WINDEPLOYQT)
        add_custom_command(TARGET reno POST_BUILD
          COMMAND "${WINDEPLOYQT}" $<IF:$<CONFIG:Debug>,--debug,--release> --no-translations --no-compiler-runtime
                  --no-opengl-sw --no-system-d3d-compiler --no-system-dxc-compiler
                  --skip-plugin-types generic,networkinformation,tls,iconengines
                  --exclude-plugins ffmpegmediaplugin,qsvg,qico,qicns,qtga,qwbmp,qpdf,qdirect2d,qminimal,qoffscreen
                  $<TARGET_FILE:reno>
          VERBATIM)
      endif()
      get_filename_component(_ffroot "${FFMPEG_INCLUDE_DIR}" DIRECTORY)
      # (only the five libraries Reno links; not avdevice, avfilter or postproc)
      file(GLOB _ffdlls "${_ffroot}/bin/avcodec*.dll" "${_ffroot}/bin/avformat*.dll" "${_ffroot}/bin/avutil*.dll"
                        "${_ffroot}/bin/swscale*.dll" "${_ffroot}/bin/swresample*.dll")
      if(_ffdlls)
        add_custom_command(TARGET reno POST_BUILD
          COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_ffdlls} $<TARGET_FILE_DIR:reno>
          VERBATIM)
      endif()
    endif()
    # reno_vst32.exe: runs 32-bit VST2 plug-ins for the 64-bit reno.exe. It cannot share this x64 build tree, so it is
    # configured and built separately (src/vst32/CMakeLists.txt) with the Win32 platform, into reno.exe's folder.
    if(WIN32 AND MSVC AND NOT CMAKE_CROSSCOMPILING AND RENO_WITH_VST32_BRIDGE)
      if(CMAKE_GENERATOR MATCHES "^Visual Studio")
        set(_vst32_gen "${CMAKE_GENERATOR}")
      else()
        set(_vst32_gen "Visual Studio 17 2022")
      endif()
      set(_vst32_wrap "")
      if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.24)
        set(_vst32_wrap ${CMAKE_COMMAND} -E env --unset=Platform --unset=VSCMD_ARG_TGT_ARCH)     # drop the x64 prompt's settings
      endif()
      set(_vst32_toolset "")
      if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        set(_vst32_toolset -T ClangCL)      # the same compiler as the rest (Visual Studio's "C++ Clang tools" component)
      endif()
      add_custom_target(reno_vst32 ALL
        COMMAND ${_vst32_wrap} ${CMAKE_COMMAND} -S ${CMAKE_CURRENT_SOURCE_DIR}/src/vst32 -B ${CMAKE_BINARY_DIR}/vst32
                -G "${_vst32_gen}" -A Win32 ${_vst32_toolset} -DRENO_VST32_OUT=${CMAKE_RUNTIME_OUTPUT_DIRECTORY}
        COMMAND ${_vst32_wrap} ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR}/vst32 --config $<IF:$<CONFIG:Debug>,Debug,Release>
        COMMENT "Building reno_vst32.exe (32-bit, for 32-bit VST2 plug-ins)"
        VERBATIM)
      add_dependencies(reno reno_vst32)
    elseif(WIN32)
      message(STATUS "Reno: reno_vst32.exe (32-bit VST2 plug-ins) is not built by this configuration: build src/vst32 with a 32-bit toolchain (see src/vst32/CMakeLists.txt)")
    endif()
    set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT reno)
  else()
    message(WARNING "Reno: Qt 6 was not found (set CMAKE_PREFIX_PATH to the Qt folder); building without the editor")
  endif()
endif()
