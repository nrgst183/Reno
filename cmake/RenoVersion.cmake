# Reno's version, for the programs (src/app/version.h), their .rc files and the macOS bundle.
#   RENO_VERSION   1.5 or 1.5.2 (a leading v is dropped). The release build sets it from the release's tag. Left empty,
#                  it is the newest v* tag git finds, or else the numbers in src/app/version.h.
# Sets RENO_VERSION_MAJOR, _MINOR, _PATCH, RENO_VERSION_STRING ("1.5", or "1.5.2" when the third number isn't 0) and
# RENO_VERSION_FULL ("1.5.0"), and writes reno_version.h. reno_use_version(<target>) lets a target's sources and .rc
# file include it. A tag made after configuring shows up the next time CMake configures.
set(RENO_VERSION "" CACHE STRING "Reno's version, e.g. 1.5 or 1.5.2 (empty: the newest v* git tag, else src/app/version.h)")
get_filename_component(_reno_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

set(_reno_ver "${RENO_VERSION}")
set(_reno_from "RENO_VERSION")
if(_reno_ver STREQUAL "")
  find_package(Git QUIET)
  if(GIT_FOUND)
    execute_process(COMMAND "${GIT_EXECUTABLE}" describe --tags --abbrev=0 --match "v[0-9]*"
                    WORKING_DIRECTORY "${_reno_root}" RESULT_VARIABLE _reno_git
                    OUTPUT_VARIABLE _reno_ver OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(_reno_git EQUAL 0)
      set(_reno_from "git tag ${_reno_ver}")
    else()
      set(_reno_ver "")
    endif()
  endif()
endif()
if(_reno_ver STREQUAL "")
  file(STRINGS "${_reno_root}/src/app/version.h" _reno_line REGEX "^#define RENO_VERSION_STRING ")
  string(REGEX REPLACE ".*\"(.*)\".*" "\\1" _reno_ver "${_reno_line}")
  set(_reno_from "src/app/version.h")
endif()
string(REGEX REPLACE "^v" "" _reno_ver "${_reno_ver}")
if(NOT _reno_ver MATCHES "^([0-9]+)\\.([0-9]+)(\\.([0-9]+))?$")
  message(FATAL_ERROR "Reno: the version \"${_reno_ver}\" (from ${_reno_from}) is not MAJOR.MINOR or MAJOR.MINOR.PATCH")
endif()
set(RENO_VERSION_MAJOR ${CMAKE_MATCH_1})
set(RENO_VERSION_MINOR ${CMAKE_MATCH_2})
set(RENO_VERSION_PATCH 0)
if(NOT "${CMAKE_MATCH_4}" STREQUAL "")
  set(RENO_VERSION_PATCH ${CMAKE_MATCH_4})
endif()
set(RENO_VERSION_STRING "${RENO_VERSION_MAJOR}.${RENO_VERSION_MINOR}")
if(NOT RENO_VERSION_PATCH EQUAL 0)
  string(APPEND RENO_VERSION_STRING ".${RENO_VERSION_PATCH}")
endif()
set(RENO_VERSION_FULL "${RENO_VERSION_MAJOR}.${RENO_VERSION_MINOR}.${RENO_VERSION_PATCH}")
message(STATUS "Reno: version ${RENO_VERSION_STRING} (from ${_reno_from})")

# (file(CONFIGURE) leaves the file alone when nothing changed, so a reconfigure doesn't rebuild what includes it)
set(RENO_VERSION_DIR "${CMAKE_BINARY_DIR}/generated")
file(CONFIGURE OUTPUT "${RENO_VERSION_DIR}/reno_version.h" @ONLY CONTENT
"// Written by cmake/RenoVersion.cmake; included by src/app/version.h.
#define RENO_VERSION_MAJOR @RENO_VERSION_MAJOR@
#define RENO_VERSION_MINOR @RENO_VERSION_MINOR@
#define RENO_VERSION_PATCH @RENO_VERSION_PATCH@
#define RENO_VERSION_STRING \"@RENO_VERSION_STRING@\"
")

function(reno_use_version target)
  target_include_directories(${target} PRIVATE "${RENO_VERSION_DIR}")
  target_compile_definitions(${target} PRIVATE RENO_VERSION_GENERATED)
endfunction()
