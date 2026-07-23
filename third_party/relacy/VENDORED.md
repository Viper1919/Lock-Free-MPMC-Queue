# Vendored: Relacy Race Detector

- Source: https://github.com/dvyukov/relacy, commit `c063779`
  (`relacy/` headers and `LICENSE` only). BSD-style license.
- Purpose: model checking (`model/`). Relacy runs the test as
  cooperatively-scheduled fibers, explores interleavings (random /
  context-bound / full search) and simulates the C++ memory model —
  including store buffering and stale relaxed reads — so ordering bugs
  that real hardware may never exhibit become findable and, for small
  models, exhaustively excludable.

## Local patch (platform.hpp)

Upstream passes the fiber context pointer through `makecontext`'s
variadic arguments, which POSIX types as `int` — a 64-bit pointer is
truncated on LP64 AArch64 (Apple Silicon) and the fiber start crashes.
Patched to hand the pointer through a static instead (safe: the
simulation is single-threaded, and `create_fiber` swaps into the new
fiber synchronously). Marked `LOCAL PATCH` in the source.

## Sharp edges (learned here, kept for the next reader)

- Relacy replaces the **global** `operator new`. Any heap allocation
  outside `rl::simulate` (iostream construction, std::string growth)
  corrupts it and segfaults inside the next context construction —
  driver code must be allocation-free outside simulation.
- `test_params::iteration_count` must be nonzero for every scheduler,
  including full search (it is an upper bound; the search stops early
  when the space is exhausted). Zero crashes.
- Relacy does not implement the mixed seq_cst-operation/seq_cst-fence
  synchronization rules ([atomics.order] p5–p6 shapes): a seq_cst
  store paired with only a seq_cst fence on the observer side is
  reported racy. Formulate store->load protocols with fences on BOTH
  sides (the fence-fence rule) — which is also the canonical, and
  arguably the only portable-in-practice, formulation.
