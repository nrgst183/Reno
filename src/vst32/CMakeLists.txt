# reno_vst32: the 32-bit host process for 32-bit VST2 plug-ins (see ../audiofx/vst32proto.h).
#
# It has to be a 32-bit program, so it is a project of its own: a Win32 build cannot share the x64 build tree of
# Reno. The main CMakeLists.txt (and build_msvc.bat, and Reno.sln through vs\reno_vst32.vcxproj) builds it with this
# file and Visual Studio's "Win32" platform. Plain Win32 and the VST2 header: no Qt, no FFmpeg, and the C runtime is
# linked in, so the program needs nothing installed beside it.
#   cmake -S src/vst32 -B build/vst32 -G "Visual Studio 17 2022" -A Win32 && cmake --build build/vst32 --config Release
# On Linux the same source builds as a 64-bit program (the tests use it: tests/vst32/run.sh).
cmake_minimum_required(VERSION 3.21)
project(reno_vst32 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
# where reno_vst32.exe goes (beside reno.exe), whatever the configuration
if(RENO_VST32_OUT)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${RENO_VST32_OUT}")
  foreach(cfg DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${cfg} "${RENO_VST32_OUT}")
  endforeach()
endif()

set(SRC ${CMAKE_CURRENT_SOURCE_DIR}/..)
add_executable(reno_vst32 ${SRC}/vst32/vst32server.cpp ${SRC}/audiofx/vst32ipc.cpp ${SRC}/audiofx/vst2host.cpp
                          ${SRC}/core/base.cpp ${SRC}/core/dynlib.cpp)
target_include_directories(reno_vst32 PRIVATE ${SRC}/../third_party/vst2)
if(MSVC)
  target_compile_options(reno_vst32 PRIVATE /W3 /utf-8 /bigobj)
  target_compile_definitions(reno_vst32 PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX UNICODE _UNICODE)
else()
  target_compile_options(reno_vst32 PRIVATE -Wall -Wno-unused-parameter)
  target_compile_definitions(reno_vst32 PRIVATE __cdecl=)
  find_package(Threads REQUIRED)
  target_link_libraries(reno_vst32 PRIVATE Threads::Threads ${CMAKE_DL_LIBS} rt)
endif()
if(WIN32)
  target_link_libraries(reno_vst32 PRIVATE user32 gdi32 ole32 oleaut32 uuid shell32 advapi32 comdlg32 winmm)
endif()
