# Lock-Free MPMC Queue

A Michael–Scott lock-free queue in C++20 with safe memory reclamation,
verification, and a rigorous benchmark harness.

> **Thesis:** a lock-free queue is easy to write and hard to make correct —
> the difficulty is not the algorithm, it's knowing when it is safe to free
> memory.

This README will grow into the full reasoning document as the project
progresses; the sections below track status.

## Status

| Phase | Content | State |
| --- | --- | --- |
| 0 | Scaffolding: CMake + CI + sanitizers, mutex baseline, benchmark harness, machine record | ✅ |
| 1 | Michael–Scott queue, deliberately leaky, all `seq_cst`, unit + stress tests | ✅ |
| 2 | Safe memory reclamation: hazard pointers | 🔜 |
| 3a | Verification: sanitizers, invariant stress, adversarial scheduling | — |
| 3b | Benchmarking: sweeps, false-sharing experiment, honest comparison | — |
| 4 | Write-up | — |

## Non-goals

Scoping honestly is a signal, not an admission:

- **Not trying to beat production queues** (`moodycamel::ConcurrentQueue`,
  `boost::lockfree::queue`). Measuring *how far off* this is — and
  diagnosing why — is the honest, interesting result.
- **Not a general-purpose library.** No allocator customization, no
  exception-safety guarantees beyond what's documented.
- **Not a formal proof.** Testing and model checking, not verification in
  the mathematical sense.

## Build & run

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure

# Sanitizer configurations (also run in CI):
cmake -B build-tsan -DLFQ_SANITIZE=thread
cmake -B build-asan -DLFQ_SANITIZE=address,undefined

# Benchmark (CSV on stdout, median/IQR summary on stderr):
./build/bench_main --queue=mutex --threads=1,2,4,8 --ops=100000 --reps=10
```

Benchmark environment and its caveats: [`results/machine.md`](results/machine.md).
