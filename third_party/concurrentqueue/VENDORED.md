# Vendored: moodycamel::ConcurrentQueue

- Source: https://github.com/cameron314/concurrentqueue, tag `v1.0.4`,
  files `concurrentqueue.h` and `LICENSE.md`, unmodified.
- Purpose: the honest-comparison baseline for the phase-3b benchmarks.
  It is a heavily optimized block-based MPMC queue — being outperformed
  by it and explaining why is part of the project's write-up, so it is
  benchmarked under the same harness as everything else.
- Scope: bench-only. Nothing in `include/lfq/` depends on it, and it is
  added to the bench target as a SYSTEM include so its warnings are its
  own business.
