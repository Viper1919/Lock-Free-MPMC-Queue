# Design notes

The deep-dive companion to the README: the algorithm, the memory
ordering table, the reclamation analysis, the verification story, and
the benchmark methodology.

## The algorithm

Michael & Scott (PODC 1996): a singly-linked list with a permanent
dummy node, `head_` pointing at the dummy, `tail_` at (or one behind)
the last node. The dummy means head_ and tail_ are never null and the
empty queue needs no special case that would make enqueuers and
dequeuers contend on a shared "is it empty" decision.

**Enqueue** snapshots `tail` and `tail->next`, validates the snapshot,
then either (a) finds `next != nullptr` — tail is lagging because some
enqueuer linked its node but hasn't swung `tail_` — and *helps* by
CASing `tail_` forward before retrying, or (b) CASes `tail->next` from
null to the new node (the linearization point) and then swings `tail_`,
where failure is fine because someone else helped.

**Dequeue** snapshots `head`, `tail`, `head->next`, re-validates
`head_`, and: if `head == tail` with null next the queue is empty; with
non-null next the tail lags and dequeue helps swing it (same progress
argument); otherwise it reads the value *out of next* — the new dummy —
and CASes `head_` forward, retiring the old dummy on success.

Two load-bearing details:

- **Helping is the progress guarantee.** A thread suspended between
  "link node" and "swing tail" cannot stall anyone — every other thread
  can complete that swing for it. That makes the queue lock-free (some
  thread always makes progress), not wait-free (a specific thread can
  retry forever under contention), and not merely obstruction-free (which
  would only guarantee progress in isolation).
- **The value is read before the winning CAS.** After `head_` swings,
  `next` is the new dummy and another thread can dequeue past it and
  retire it; a read after the CAS is a use-after-free the moment
  reclamation is real.

## Memory ordering

The discipline was: everything `seq_cst` until the phase-3a
verification net (TSan + adversarial scheduling, running on a
weakly-ordered AArch64 machine) existed to catch a wrong relaxation —
then relax one class of operation at a time, re-running every suite
after each step, measuring before and after. Every non-default
ordering carries its invariant in a comment at the site; this table is
the index.

| Site | Ordering | Invariant it rests on |
|---|---|---|
| `protect()` verify load; dequeue's `head_` re-validation | `seq_cst` | The hazard publish/verify store→load pair — the one shape release/acquire cannot order (below) |
| hazard slot publish (`protect`/`set` store) | `seq_cst` | Same pair, other half |
| `scan()` fence before hazard snapshot | `seq_cst` fence | The scanner's half of the protocol, amortized once per scan |
| loads of `head_`, `tail_`, `node::next` | `acquire` | Must see the node contents the linking CAS released; dequeue's `tail_` load also anchors the `head != tail ⇒ next != null` happens-before chain |
| link CAS (`tail->next`: null→n) | `release` / `relaxed` | The publication point of n's contents; failure value is discarded |
| `tail_` swings, `head_` swing | `release` / `relaxed` | Republish reachable contents; feed the dequeue invariant chain |
| guard destructor slot clears | `release` | A scan that acquires the null imports every read we made of the node — that edge makes the free safe |
| `scan()` per-slot loads | `acquire` | With the fence above: presence keeps the node; absence proves the protector's reads happened-before the free |
| record recycle CAS (`active`) | `acquire` / `relaxed` | Imports the releasing thread's slot clears |
| record push, orphan push / adopt | `release` / `acquire` | Publish/import the record fields and orphan chain |
| retire/free counters, `nrecords_` | `relaxed` | Monitoring and sizing only; never proof of anything |

**Why the protect pair cannot relax.** Protection is a store (publish
the hazard) followed by a load (verify the pointer is still reachable).
Release/acquire orders load→load, load→store, store→store — never
store→load. Weaken either op and the CPU may hoist the verify load
above the publish store: the verify passes against a pre-retirement
snapshot while the scan reads a pre-publish snapshot of the slot — both
threads miss each other and a protected node is freed. The scanner
needs the mirror-image barrier, provided as one `seq_cst` fence before
the hazard snapshot rather than per-slot `seq_cst` loads.

**What relaxing bought: nothing measurable on this machine — and that
is a finding, not a failure.** Same-session before/after
(`results/relax_step*_*.csv`): every median within or touching IQR
noise at 1/8/16 threads, for both the queue relaxation and the domain
relaxation. On AArch64, `seq_cst` loads and stores compile to
`ldar`/`stlr` — the same instructions acquire/release produce — and
the hot cost centers are the CAS traffic and cache-line movement, not
fence strength. The asymmetry to know: on x86-TSO plain loads/stores
are already acquire/release, but a `seq_cst` *store* costs a full
barrier (`xchg`/`mfence`) — so the ordering work matters there
precisely for the one store this design cannot relax, the hazard
publish. The memory-model work here documents *why* each ordering is
sufficient; it is not a throughput optimization on either
architecture's hot path.

## Reclamation

**The problem.** Thread A loads `head`, gets preempted. Thread B
dequeues and frees that node. A resumes and dereferences freed memory.
Worse: the allocator recycles the address, a new node lands there, and
A's CAS *succeeds* on the wrong node — ABA, which is silent corruption,
not a crash. Tagged/counted pointers (a version counter in spare bits,
or double-width CAS) defeat the ABA symptom but not the use-after-free:
the algorithm still reads through a freed pointer. The only cure is
knowing when no thread can still be reading a retired node — safe
memory reclamation.

**Hazard pointers over epoch-based reclamation, as a decision:**

| | Hazard pointers | Epoch-based (EBR) |
|---|---|---|
| Garbage bound | **O(records × K + threshold)** | Unbounded — one stalled thread pins all garbage |
| Read cost | Per-access publish + store→load ordering | Nearly free in the common case |
| Complexity | Per-access bookkeeping | Simpler to write, subtler to reason about |
| Failure mode | Graceful — a stalled thread pins only what its K slots name | A blocked thread leaks the world |

The bounded-garbage guarantee is the property a systems person cares
about, and it is asserted by `reclaim_test` *during* a hot concurrent
run, not just claimed. The cost side is equally concrete: −23%
single-threaded throughput vs the leaky baseline, converging to −7% at
16 threads (see the results).

**Domain design** (`hazard_pointer.hpp`): one process-wide domain; a
grow-only lock-free list of hazard records (K=2 slots each — dequeue
needs `head` and `head->next` live at once), recycled on thread exit,
never freed. Retirement goes to thread-local bags (no contention);
crossing `max(64, 2·K·records)` triggers a scan that snapshots all
hazards and frees exactly the retired nodes nobody names. A thread
exiting with still-protected retired nodes pushes them to a global
orphan stack that any later scan adopts — nothing is ever silently
dropped, which is what lets LSan enforce the books.

## Verification

"It worked on my machine" is worth nothing for a concurrent data
structure: the bugs are timing-dependent by construction, and any one
machine has one memory model. No single tool below is sufficient —
each catches a class of bug the others structurally cannot, so the
evidence is layered.

### Layer 1 — sanitizers, in CI since phase 0

- **TSan** understands `std::atomic`, so a report is a genuine data
  race, not a false positive on the lock-free core. Clean TSan is
  table stakes, not proof: it observes the interleavings that actually
  ran, and it cannot prove the absence of races on paths the schedule
  never took.
- **ASan** turns use-after-free — the failure mode safe memory
  reclamation exists to prevent — into a loud, attributed crash
  instead of silent corruption. `reclaim_test` leans on this: it
  dereferences a retired-but-protected node on purpose, so a
  reclamation bug becomes an ASan report rather than a flaky counter.
- **LSan** (Linux CI; macOS arm64 ASan has no leak detection) enforces
  the other half of the reclamation contract: nothing retired is ever
  lost track of. The deliberately-leaky phase-1 baseline is bracketed
  with `scoped_lsan_disable` so only a genuine `hp_reclaimer` leak
  fails the build.
- **UBSan** because a data structure built on atomics and casts has
  no business tolerating undefined behavior anywhere.

### Layer 2 — structural invariants under stress

`stress_test` runs P producers × C consumers for millions of
operations and asserts the three properties that define a MPMC queue:

1. **No loss, no duplication** — every enqueued value is dequeued
   exactly once (per-value counter array, checked all-ones).
2. **Per-producer FIFO** — each producer enqueues strictly increasing
   sequence numbers; every consumer must observe any given producer's
   values in increasing order. Global FIFO is not a meaningful MPMC
   assertion; per-producer order is, and it is a real linearizability
   proxy that catches genuine ordering bugs.
3. **Drain to empty** — after the run, consumed == produced, the queue
   reports empty, and (for hazard pointers) retired == freed once the
   domain drains.

`reclaim_test` adds the reclamation-specific invariants: the books
balance after a quiesced drain, in-flight garbage stays under the
O(threads × threshold) bound *while the run is hot* — the property
that justified hazard pointers over epoch-based reclamation — and a
node retired by an exiting thread survives for as long as another
thread's hazard names it.

### Layer 3 — adversarial scheduling

The bugs in a lock-free structure live inside windows a few
instructions wide: pointer read but hazard not yet published; hazard
published but not yet re-verified; node linked but tail not yet swung;
value read but dequeue not yet won; head swung but node not yet
retired. A real scheduler preempts inside one of those windows once in
millions of operations, which is exactly why these bugs survive normal
testing.

`inject.hpp` plants an `LFQ_INJECT()` point inside every such window.
In normal and benchmark builds the macro compiles to nothing. The
`adversarial_stress_test` and `adversarial_reclaim_test` targets
rebuild the *same* test sources with `LFQ_SCHEDULE_FUZZ` on: each
injection point then rolls a thread-local xorshift and either yields
the core (1/8) or spins briefly (1/8), while the thread count rises to
4× hardware concurrency. Preemption-mid-window becomes the common case
instead of the millions-to-one case. The rng is deliberately
thread-local and lock-free itself — a shared or locked rng at a retry
point would serialize the very contention it exists to provoke.

These targets run under every CI job, so the adversarial schedules are
also explored under TSan and ASan, not just in release mode.

### Memory-model coverage

x86-64 is TSO: it forgives acquire/release mistakes that weaker
architectures do not. This project's primary development machine is an
Apple M2 (AArch64, weakly ordered), and CI covers x86-64 Linux with
GCC and Clang — so every test layer above runs on both a weak and a
strong memory model on every push. Note the honest limitation hiding
in that sentence: everything is still `seq_cst` today, so the weak
machine is currently validating the algorithm, not the relaxed
orderings. When the deliberate relaxation pass happens (see roadmap),
the AArch64 runs become the load-bearing evidence.

### What this does not prove

- **No exhaustive interleaving exploration.** Model checking (Relacy,
  CDSChecker) explores all schedules and memory-model behaviors for
  small inputs and would catch bugs that cannot fire on either test
  machine. It was scoped out of this phase deliberately — it is the
  first item on the project's de-scope list — and remains the highest-
  value next step for the verification story.
- **No formal proof.** Testing and adversarial scheduling raise
  confidence; they do not verify in the mathematical sense.
- **Quiesced-only operations are asserted by contract, not by tool.**
  `empty()`, destruction, and `drain()` require external quiescence,
  like any standard container; nothing checks a caller who violates
  that.

## Benchmark methodology

A bad benchmark is an anti-signal, so every choice below is a
decision with a reason, not a default. The harness is custom
(`bench/harness.hpp` + `bench/bench_main.cpp`); results and analysis
live in `results/` (`phase3b_notes.md` is the summary).

**Workloads.** Four, because no single shape is representative:
`pairs` (every thread alternates enqueue/dequeue — the queue stays
near-empty, which is *maximum* head/tail contention, a stress test
for the CAS points, not a parallelism showcase); `pc` with a
`--ratio` P:C split (1:N lives on the empty-queue path, N:1
exercises tail-lagging and helping); `enq` and `deq` isolate one end
each — `deq` prefills before timing so filling is setup, not
measurement.

**Thread counts sweep past hardware concurrency** (…8, 16 on an
8-core machine), deliberately: a preempted lock holder stalls all
waiters, a preempted CAS loser stalls no one, and oversubscription
is the regime where that distinction is measurable. It produced the
headline result (mutex p99 26µs vs ms-hp 2.3µs at 16 threads).

**Rigor.**
- Median and IQR over ≥10 repetitions, never the mean; 2 warmup reps
  discarded. One scheduler hiccup destroys a mean.
- Latency samples go to preallocated per-thread vectors, merged and
  percentiled after the run — allocating mid-measurement measures
  the allocator.
- Dequeue latency includes empty-retry spinning: "time until I hold
  an item" is what a consumer experiences.
- `steady_clock` only; `do_not_optimize` keeps the compiler from
  deleting the loop; a spin barrier releases all threads at once and
  the driver timestamps around the release, so thread startup never
  pollutes the window.
- Threads are pinned where the OS allows it. macOS on Apple Silicon
  has no affinity API, so every local CSV row records `pinned=0` —
  the caveat travels with the data. Turbo/QoS is likewise not
  controlled; `results/machine.md` records the environment.

**The false-sharing experiment** is a template parameter, not a
fork: `ms_queue<T, R, /*PadHeadTail=*/false>` places head_ and tail_
adjacent; the default pads them to separate (128-byte, on this
machine) cache lines. Same binary, same trials, one variable.
Measured: ~26% throughput at ≥8 threads — and an inversion at 1
thread where adjacent is ~8% *faster*, because padding is pure
overhead until there is concurrency to protect against.

**The honest comparison** is `moodycamel::ConcurrentQueue`
(vendored, same harness, adapter-narrowed to the identical API). It
wins — up to 11.6× on enqueue-only — and the numbers stay in the
report with the diagnosis (per-producer block-based sub-queues
remove the shared-tail CAS and the per-element allocation) and the
semantic caveat (its FIFO is per-producer only; weaker guarantees
are part of the price of its speed).
