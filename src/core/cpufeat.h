// What the processor can do, asked once: for code that has an AVX2 version next to its SSE2 one and picks at run time (the same results: the AVX2 code
// does the same operations in the same order on wider vectors, no fused multiply-add).
#pragma once
// (64-bit only: the 32-bit plug-in bridge has no AVX2 code)
#if defined(__x86_64__) || defined(_M_X64)
#  define RENO_X86 1
#  if defined(_MSC_VER) && !defined(__clang__)
#    include <immintrin.h>
#    include <intrin.h>
#    define RENO_AVX2_FN
#    define RENO_HAVE_AVX2_CODE 1
#  elif defined(__GNUC__) || defined(__clang__)
#    include <immintrin.h>
     // clang-cl (clang with _MSC_VER) leaves the AVX/AVX2 intrinsics out of <immintrin.h> unless the whole file is built
     // for AVX2; their own headers declare them with a per-function target, so they can be included after it.
#    if defined(_MSC_VER) && defined(__clang__) && !defined(__AVX2__)
#      include <avxintrin.h>
#      include <avx2intrin.h>
#    endif
#    if defined(_MSC_VER)
#      include <intrin.h>
#    endif
#    define RENO_AVX2_FN __attribute__((target("avx2")))
#    define RENO_HAVE_AVX2_CODE 1
#  endif
#endif

namespace reno {

// True when the processor (and the operating system, which must save the wide registers) can run AVX2 code. RENO_NO_AVX2 in the environment turns it off.
bool cpuHasAvx2();

}  // namespace reno
