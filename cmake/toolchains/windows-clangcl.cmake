# Cross-compiles Reno for 64-bit Windows from Linux (or macOS) with clang-cl and lld-link, against a copy of MSVC's
# headers and libraries and the Windows SDK - the folder layout the "xwin" tool makes
# (https://github.com/Jake-Shadle/xwin: `xwin --accept-license splat --output ~/xwin`), or copied from a Visual Studio
# install:
#   $RENO_XWIN/crt/include  $RENO_XWIN/crt/lib/x86_64                 (xwin's layout), or
#   $RENO_XWIN/msvc/include $RENO_XWIN/msvc/lib/x64                   (VC\Tools\MSVC\<ver>\include, lib\x64)
#   $RENO_XWIN/sdk/include/{ucrt,um,shared,winrt} $RENO_XWIN/sdk/lib/{ucrt,um}/x86_64   (xwin), or
#   $RENO_XWIN/sdk/Include/<ver>/...      $RENO_XWIN/sdk/Lib/<ver>/{ucrt,um}/x64        (Windows Kits\10)
#
# Qt: the Windows Qt (msvc2022_64, with its lib/cmake folder) in CMAKE_PREFIX_PATH, and a Qt of the same version for
# this machine in QT_HOST_PATH (Qt's own cross-compiling rule; Reno uses no moc, so only Qt's CMake files are read).
# FFmpeg: RENO_FFMPEG_DIR, a folder with include\ and lib\*.lib.
#
#   cmake --preset windows-cross        (CMakePresets.json; set RENO_XWIN, RENO_QT_WIN, RENO_QT_HOST, RENO_FFMPEG_WIN)

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_VERSION 10.0)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

if(NOT RENO_XWIN)
  set(RENO_XWIN "$ENV{RENO_XWIN}")
endif()
if(NOT RENO_XWIN OR NOT EXISTS "${RENO_XWIN}")
  message(FATAL_ERROR "windows-clangcl.cmake: set RENO_XWIN to the folder with MSVC's and the Windows SDK's headers and libraries")
endif()
# (passed on to the compiler checks CMake runs in their own projects)
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES RENO_XWIN)

find_program(_clangcl NAMES clang-cl clang-cl-20 clang-cl-19 clang-cl-18 clang-cl-17 REQUIRED)
find_program(_lldlink NAMES lld-link lld-link-20 lld-link-19 lld-link-18 lld-link-17 REQUIRED)
find_program(_llvmlib NAMES llvm-lib llvm-lib-20 llvm-lib-19 llvm-lib-18 llvm-lib-17 REQUIRED)
find_program(_llvmrc NAMES llvm-rc llvm-rc-20 llvm-rc-19 llvm-rc-18 llvm-rc-17 REQUIRED)
find_program(_llvmmt NAMES llvm-mt llvm-mt-20 llvm-mt-19 llvm-mt-18 llvm-mt-17)
set(CMAKE_C_COMPILER "${_clangcl}")
set(CMAKE_CXX_COMPILER "${_clangcl}")
set(CMAKE_LINKER "${_lldlink}")
set(CMAKE_AR "${_llvmlib}")
set(CMAKE_RC_COMPILER "${_llvmrc}")
if(_llvmmt)
  set(CMAKE_MT "${_llvmmt}")
endif()
set(CMAKE_C_COMPILER_TARGET x86_64-pc-windows-msvc)
set(CMAKE_CXX_COMPILER_TARGET x86_64-pc-windows-msvc)
# (CMake's compiler checks link a Release program: a copied CRT often has no debug libraries)
set(CMAKE_TRY_COMPILE_CONFIGURATION Release)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

# the two layouts: xwin's, or folders copied from Visual Studio and the Windows Kits
set(_inc "")
set(_lib "")
if(EXISTS "${RENO_XWIN}/crt/include")
  list(APPEND _inc "${RENO_XWIN}/crt/include" "${RENO_XWIN}/sdk/include/ucrt" "${RENO_XWIN}/sdk/include/um"
                   "${RENO_XWIN}/sdk/include/shared" "${RENO_XWIN}/sdk/include/winrt")
  list(APPEND _lib "${RENO_XWIN}/crt/lib/x86_64" "${RENO_XWIN}/sdk/lib/ucrt/x86_64" "${RENO_XWIN}/sdk/lib/um/x86_64")
else()
  file(GLOB _sdkinc LIST_DIRECTORIES true "${RENO_XWIN}/sdk/Include/10.*")
  file(GLOB _sdklib LIST_DIRECTORIES true "${RENO_XWIN}/sdk/Lib/10.*")
  list(SORT _sdkinc)
  list(SORT _sdklib)
  list(POP_BACK _sdkinc _si)    # the newest SDK
  list(POP_BACK _sdklib _sl)
  list(APPEND _inc "${RENO_XWIN}/msvc/include" "${_si}/ucrt" "${_si}/um" "${_si}/shared" "${_si}/winrt")
  list(APPEND _lib "${RENO_XWIN}/msvc/lib/x64" "${_sl}/ucrt/x64" "${_sl}/um/x64")
endif()

set(_flags "-fms-compatibility-version=19.43 /D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH /D_WIN32_WINNT=0x0A00")
foreach(d ${_inc})
  string(APPEND _flags " /imsvc \"${d}\"")
endforeach()
set(CMAKE_C_FLAGS_INIT "${_flags}")
set(CMAKE_CXX_FLAGS_INIT "${_flags} /EHsc")
set(CMAKE_RC_FLAGS_INIT "")
foreach(d ${_inc})
  string(APPEND CMAKE_RC_FLAGS_INIT " /I \"${d}\"")
endforeach()
set(_lflags "")
foreach(d ${_lib})
  string(APPEND _lflags " /libpath:\"${d}\"")
endforeach()
set(CMAKE_EXE_LINKER_FLAGS_INIT "${_lflags}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_lflags}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${_lflags}")

# look for libraries and headers only in the target's folders (Qt, FFmpeg), programs on this machine
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)
