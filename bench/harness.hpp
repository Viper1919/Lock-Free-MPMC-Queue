#pragma once

// Benchmark harness primitives. Methodology decisions live here so
// bench_main.cpp is only the sweep driver:
//
//  - steady_clock, never system_clock (monotonic; immune to NTP slew).
//  - Thread pinning where the OS allows it (Linux). macOS exposes no
//    affinity API on Apple Silicon, so runs there record pinned=0 and the
//    caveat travels with the data instead of silently disappearing.
//  - Latency samples go into preallocated per-thread vectors; percentiles
//    are computed after the run. Allocating mid-measurement would measure
//    the allocator.
//  - do_not_optimize() keeps the compiler from deleting the benchmark.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

namespace lfq::bench {

inline std::uint64_t now_ns() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

// Compiler barrier: the value is "used" as far as the optimizer knows.
inline void do_not_optimize(std::uint64_t& value) {
  asm volatile("" : "+r"(value) : : "memory");
}

// Pin the calling thread to a core. Returns false where unsupported
// (macOS) so callers can record honesty instead of assuming success.
inline bool pin_to_core(unsigned core) {
#if defined(__linux__)
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(core % std::thread::hardware_concurrency(), &set);
  return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
#else
  (void)core;
  return false;
#endif
}

// Start line for worker threads. Workers spin (never park) so every
// thread leaves the line simultaneously; the driver timestamps around
// release() so thread creation cost never pollutes the measurement.
class spin_barrier {
 public:
  void worker_arrive_and_wait() {
    arrived_.fetch_add(1, std::memory_order_relaxed);
    while (!go_.load(std::memory_order_acquire)) {
      // spin
    }
  }

  void driver_wait_for(unsigned n) {
    while (arrived_.load(std::memory_order_relaxed) < static_cast<int>(n)) {
      std::this_thread::yield();
    }
  }

  void driver_release() { go_.store(true, std::memory_order_release); }

 private:
  std::atomic<int> arrived_{0};
  std::atomic<bool> go_{false};
};

// Percentile by index on a sorted sample vector (nearest-rank flavor).
inline std::uint64_t percentile_sorted(const std::vector<std::uint64_t>& sorted,
                                       double q) {
  if (sorted.empty()) return 0;
  auto idx = static_cast<std::size_t>(q * static_cast<double>(sorted.size() - 1) + 0.5);
  if (idx >= sorted.size()) idx = sorted.size() - 1;
  return sorted[idx];
}

// Median / quartiles across repetitions. Median and IQR, never the mean:
// one scheduler hiccup destroys a mean but barely moves a median.
struct rep_summary {
  double median;
  double q1;
  double q3;
};

inline rep_summary summarize(std::vector<double> reps) {
  std::sort(reps.begin(), reps.end());
  auto at = [&](double q) {
    auto idx = static_cast<std::size_t>(q * static_cast<double>(reps.size() - 1) + 0.5);
    if (idx >= reps.size()) idx = reps.size() - 1;
    return reps[idx];
  };
  return {at(0.50), at(0.25), at(0.75)};
}

}  // namespace lfq::bench
