// Single TU that pulls in minimp3's implementation. Everything else in the
// project includes "minimp3.h" as a regular header.
//
// MINIMP3_NO_SIMD: portable scalar path — simplifies cross-compile to the
// Cortex-M7 device target instead of detecting SSE/NEON.
// MINIMP3_ONLY_MP3: we only decode Layer III streams; drops Layer I/II
// decode tables.
#define MINIMP3_NO_SIMD
#define MINIMP3_ONLY_MP3
#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"
