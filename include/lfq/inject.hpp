#pragma once

// Adversarial scheduling injection (phase 3a).
//
// The bugs in a lock-free structure live inside windows a few
// instructions wide: between loading a pointer and publishing its hazard,
// between linking a node and swinging the tail. A real scheduler preempts
// inside one of those windows once in millions of operations; these
// injection points force it constantly, by widening every window with a
// random yield (give the core away mid-window) or a short spin (stay
// resident but let every rival thread pass through first).
//
// LFQ_INJECT() compiles to nothing unless LFQ_SCHEDULE_FUZZ is defined,
// so normal and benchmark builds are untouched; the adversarial_* test
// targets compile the SAME sources with fuzzing on — no forked code
// paths to drift apart.
//
// The randomness is a thread-local xorshift. It must be this cheap and
// this local: a shared or locked rng at a retry point would serialize
// the very contention the injection exists to provoke, hiding every bug
// behind its own synchronization.

#ifdef LFQ_SCHEDULE_FUZZ

#include <cstdint>
#include <thread>

namespace lfq::detail {

inline void inject_point() {
  thread_local std::uint32_t state = [] {
    // The thread_local's own address: distinct per thread, varied per
    // run by ASLR. Reproducibility is deliberately not a goal here —
    // coverage of distinct interleavings across runs is.
    auto a = static_cast<std::uint64_t>(
        reinterpret_cast<std::uintptr_t>(&state));
    return static_cast<std::uint32_t>(a ^ (a >> 32)) | 1u;
  }();
  state ^= state << 13;
  state ^= state >> 17;
  state ^= state << 5;
  if ((state & 7u) == 0) {
    std::this_thread::yield();
  } else if ((state & 7u) == 1) {
    for (volatile unsigned n = state & 255u; n != 0; n = n - 1) {
    }
  }
}

}  // namespace lfq::detail

#define LFQ_INJECT() ::lfq::detail::inject_point()

#else

#define LFQ_INJECT() ((void)0)

#endif
