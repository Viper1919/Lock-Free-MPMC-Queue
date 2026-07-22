# Design notes

This document grows with the project. Right now it holds the
verification story (phase 3a); the algorithm walkthrough, the memory
ordering table, and the reclamation analysis land with the phase-4
write-up.

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
