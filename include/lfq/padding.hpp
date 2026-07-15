#pragma once

#include <cstddef>

namespace lfq {

// Destructive-interference (false-sharing) granularity used for alignas
// padding of hot atomics.
//
// Not std::hardware_destructive_interference_size: libc++ does not provide
// it, and GCC warns when it appears in headers because its value is
// ABI-affecting. A per-architecture constant is predictable and honest.
//
// 128 on AArch64: Apple M-series L2 prefetches in 128-byte pairs and
// hw.cachelinesize reports 128; other ARM64 parts are 64B lines but 128
// is the safe (conservative) padding either way.
#if defined(__aarch64__) || defined(_M_ARM64)
inline constexpr std::size_t cache_line_size = 128;
#else
inline constexpr std::size_t cache_line_size = 64;
#endif

}  // namespace lfq
