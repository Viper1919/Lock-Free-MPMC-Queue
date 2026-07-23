# Phase 3b — benchmark results and analysis notes

Raw data feeding the phase-4 README. Environment: Apple M2 (4P+4E,
AArch64, 128-byte cache lines), macOS, clang Release `-O2`, unpinned
(`pinned=0` in every row — macOS exposes no affinity API; see
`machine.md`). Medians of 10 reps after 2 discarded warmups; IQRs in
the CSVs. All numbers M ops/s unless stated.

## Headline: the tail under oversubscription

At 16 threads (2× hardware concurrency), pairs workload, dequeue p99:

| queue | deq p99 | vs mutex |
|---|---|---|
| mutex | 26.1 µs | — |
| ms-hp | 2.25 µs | **11.6× better** |
| moodycamel | 1.50 µs | 17.4× |
| ms (leaky) | 0.58 µs | 45× |

Throughput at the same point: ms 11.25, ms-hp 10.47, mutex 8.91.
This is the lock-free selling point in one row: a preempted lock
holder stalls every waiter for a scheduling quantum (tens of µs); a
preempted CAS loser stalls nobody. Mutex throughput also *degrades*
monotonically with oversubscription while both lock-free queues hold
flat from 8 → 16 threads.

## Nobody scales on pairs — and that's the correct result

Normalized to single-thread, at 16 threads: moodycamel holds 0.61×,
ms 0.38×, ms-hp 0.46×, mutex 0.34×. The pairs workload keeps the
queue near-empty, so every operation contends on the same one or two
cache lines — it is a contention benchmark, not a parallelism
benchmark, and *negative* scaling is the honest expectation. The MS
queue's ceiling is structural: one CAS point per end, so added
threads add retries, not throughput.

## False sharing (head_/tail_ adjacent vs padded, 128B lines)

| threads | ms | ms (adjacent) | delta | ms-hp | ms-hp (adjacent) | delta |
|---|---|---|---|---|---|---|
| 1 | 29.59 | 31.97 | **+8%** | 22.69 | 23.56 | +4% |
| 4 | 15.50 | 13.62 | −12% | 14.15 | 11.50 | −19% |
| 8 | 11.11 | 8.23 | **−26%** | 9.69 | 7.81 | −19% |
| 16 | 11.25 | 8.26 | **−27%** | 10.47 | 8.46 | −19% |

Two honest findings, not one:
- Concurrent: sharing one line between head_ and tail_ costs ~26%
  at ≥8 threads — every enqueue invalidates every dequeuer's line
  even though the fields are logically independent.
- Single-threaded, adjacent is *faster* (+8%): one hot line beats
  two, and the padding is pure overhead until there is concurrency
  to protect it from. Padding is a bet on contention.

## The price of safe reclamation (ms-hp vs ms leaky)

−23% at 1 thread (22.69 vs 29.59) — the seq_cst publish + re-verify
per protected access is a per-op toll. The gap narrows to −7% at 16
threads (10.47 vs 11.25): once CAS contention dominates, the hazard
stores hide under it. Dequeue p99 carries the rest of the cost
(2.25µs vs 0.58µs at 16t): dequeue publishes two hazards.

## Ratios at 8 threads (pc workload)

| queue | 1:3 (2P/6C) | 3:1 (6P/2C) |
|---|---|---|
| mutex | 3.35 | 7.97 |
| ms (leaky) | 9.59 | 10.44 |
| ms-hp | 6.58 | 8.67 |
| moodycamel | 13.85 | 15.40 |

Consumer-heavy is the hard case for everyone: six consumers hammer
an often-empty queue. The mutex collapses hardest (3.35) because
failed dequeues still serialize through the lock, so the starving
consumers throttle the two producers. The MS queue's empty check is
a read-mostly path and degrades far more gracefully (2.9× mutex
here). Producer-heavy (3:1) exercises the tail-lag/helping path and
everything improves.

## The honest comparison: moodycamel

Same harness, same trials. moodycamel wins everywhere, by 1.7–2.3×
on pairs at ≥8 threads, and by **11.6×** on enqueue-only at 8
threads (44.5 vs ms-hp 3.84) — that number is the diagnosis. Its
design gives each producer its own block-based sub-queue: enqueues
don't contend with each other at all and allocate a block at a time,
where the MS queue pays one CAS on a single shared tail_ plus one
`new` per element. Dequeue-only tells the same story inverted: the
gap shrinks to 1.4× (7.11 vs 5.05) because consumers must visit the
shared structure either way.

Two caveats that belong next to any use of these numbers:
- moodycamel's FIFO guarantee is per-producer, not a single total
  order — weaker semantics is part of what its throughput buys.
- Its dequeue latency at low thread counts is *worse* than the MS
  queue's p99 at 1–2 threads (its consumers search across
  sub-queues); it wins the tail only once contention rises.

## Where the mutex wins — report it

Uncontended, the mutex queue is fast: 26.4 at 1 thread beats ms-hp's
22.7, and it still edges ms-hp at 2 threads (16.1 vs 12.9). The
crossover for ms-hp is at 4 threads. A mutex is the right choice for
low-concurrency queues; this project's queue earns its complexity
only under contention and oversubscription, and the tail-latency
table is the reason to pay it.

## Single-path sweeps (8 threads)

- enq-only: mutex 6.01 > ms 4.30 > ms-hp 3.84 (and moodycamel 44.5).
  MS enqueue pays a per-op `new` plus the shared-tail CAS; the mutex
  queue amortizes storage in `std::deque` blocks. The allocator is
  part of the algorithm's cost, so it stays in the measurement.
- deq-only (prefilled): ms-hp 5.05 ≈ mutex 4.91 ≈ ms 4.80, moodycamel
  7.11. Dequeue walks a pointer chain — one cache miss per node
  dominates everyone equally; moodycamel's block locality is the
  difference.

## Charts

`phase3b_pairs_throughput.svg`, `phase3b_pairs_scalability.svg`,
`phase3b_pairs_deq_p99.svg`, `phase3b_false_sharing.svg`,
`phase3b_ratio_bars.svg` — regenerate with `bench/plot.py`.
