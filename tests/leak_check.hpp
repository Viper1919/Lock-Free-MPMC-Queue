#pragma once

// Phase 2 turns LSan back on (CI: ASAN_OPTIONS=detect_leaks=1) so that
// "hazard pointers reclaim everything" is enforced by a tool, not by an
// assertion we wrote ourselves. But the phase-1 leaky_reclaimer variants
// still run in these tests — leaky is the benchmark baseline and the
// control that proves the tests can tell the two apart — and their leak
// is BY DESIGN. This scope marks exactly those allocations as expected:
// anything allocated while a scoped_lsan_disable is alive is ignored by
// leak detection. Nothing allocated by the hp_reclaimer variants runs
// under it, so a real phase-2 leak still fails the build.

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define LFQ_LSAN_AVAILABLE 1
#endif
#elif defined(__SANITIZE_ADDRESS__)
#define LFQ_LSAN_AVAILABLE 1
#endif

#ifdef LFQ_LSAN_AVAILABLE
#include <sanitizer/lsan_interface.h>
#endif

namespace lfq::test {

struct scoped_lsan_disable {
#ifdef LFQ_LSAN_AVAILABLE
  scoped_lsan_disable() { __lsan_disable(); }
  ~scoped_lsan_disable() { __lsan_enable(); }
#else
  scoped_lsan_disable() = default;
#endif
  scoped_lsan_disable(const scoped_lsan_disable&) = delete;
  scoped_lsan_disable& operator=(const scoped_lsan_disable&) = delete;
};

}  // namespace lfq::test
