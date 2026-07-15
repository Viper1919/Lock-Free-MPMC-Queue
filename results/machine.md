# Benchmark machine

Recorded **before** any numbers, per methodology: results without an
environment record are noise.

## Hardware

| | |
| --- | --- |
| CPU | Apple M2 (AArch64) |
| Cores | 8 physical, no SMT — **heterogeneous**: 4 performance (P) + 4 efficiency (E) |
| Cache line | 128 bytes (`hw.cachelinesize`) |
| RAM | 16 GiB unified |

## Software

| | |
| --- | --- |
| OS | macOS 26.5.1 (Darwin 25.5.0) |
| Compiler | Apple clang 21.0.0 (clang-2100.1.1.101), target arm64-apple-darwin |
| Build | CMake 4.3.2, `-O2` via `CMAKE_BUILD_TYPE=Release`, C++20 |

## Caveats (honest confounds, in decreasing order of impact)

1. **No thread pinning.** macOS exposes no public affinity API on Apple
   Silicon; the harness records `pinned=0` in every CSV row from this
   machine. The scheduler decides P-core vs E-core placement, which adds
   variance — mitigated by ≥10 repetitions and reporting median/IQR.
2. **Heterogeneous cores.** Scaling curves past 4 threads mix P- and
   E-cores; a throughput knee at 5+ threads may be core asymmetry, not the
   data structure. Interpret thread-sweep shape with this in mind.
3. **DVFS / turbo not controllable.** macOS manages frequency scaling;
   there is no governor to fix. Warmup runs are discarded to let clocks and
   caches settle, but frequency drift within a run cannot be ruled out.
4. **Memory model: this is a strength.** AArch64 is weakly ordered, unlike
   TSO x86 — acquire/release mistakes that x86 silently forgives can
   actually fire here. Testing on this machine is the *stronger* test; the
   portability gap is the reverse of the usual one (x86 validation should
   be noted as pending, e.g. via CI on ubuntu runners).
5. **Clock overhead.** `steady_clock::now()` costs ~20–40 ns per call and
   the pairs workload makes 3 calls per iteration; per-op latency numbers
   include this floor. Comparisons between queues remain fair (identical
   instrumentation), absolute numbers carry the offset.
