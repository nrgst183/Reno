// Reno's version. A CMake build takes it from RENO_VERSION, or else from the newest v* git tag (cmake/RenoVersion.cmake),
// and writes reno_version.h; the numbers below are only for builds without that (Reno.sln). Read by C++ and by the .rc
// files, so nothing but preprocessor lines.
#ifndef RENO_VERSION_H
#define RENO_VERSION_H

#ifdef RENO_VERSION_GENERATED
#include "reno_version.h"
#else
#define RENO_VERSION_MAJOR 1
#define RENO_VERSION_MINOR 5
#define RENO_VERSION_PATCH 0
#define RENO_VERSION_STRING "1.5"
#endif

#endif
