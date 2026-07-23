// Model-check driver. Two kinds of run, both required:
//
//  POSITIVE — the shipped orderings, expected CLEAN. The hp protocol
//  (the load-bearing formal claim) runs under FULL interleaving
//  search: every execution of the 2-thread model is explored, so a
//  clean result is exhaustive at this model size, not sampled. The
//  queue configurations are larger, so they run under the random and
//  context-bound schedulers with big iteration counts — sampled, and
//  reported as such.
//
//  NEGATIVE — one load-bearing ordering weakened per run, expected to
//  FAIL. These are the calibration: they prove the checker can see
//  precisely the class of bug each ordering exists to prevent, so the
//  positive results above mean something. A negative run that comes
//  back clean fails this driver.
//
// Run time is a few minutes; iteration counts are tuned for CI.

#include <cstdio>
#include <cstring>

#include "hp_model.hpp"
#include "queue_model.hpp"

namespace {

// Driver rule learned the hard way: Relacy replaces the GLOBAL
// operator new, and any heap allocation made outside rl::simulate
// (there is no simulation context yet) corrupts it and segfaults
// inside the next context construction. Nothing here may allocate —
// no iostreams, no strings, printf only. Negative runs therefore keep
// the default output stream; their short failure reports are the
// expected noise (collect_history stays off).

template <class Suite>
bool run(char const* name, rl::scheduler_type_e sched, unsigned iters,
         bool expect_clean) {
  rl::test_params p;
  p.search_type = sched;
  p.iteration_count = iters;
  bool const clean = rl::simulate<Suite>(p);
  bool const ok = clean == expect_clean;
  std::printf("[%s] %-46s %-9s  %-15s (expected %s)\n", ok ? "PASS" : "FAIL",
              name, format(sched), clean ? "clean" : "violation found",
              expect_clean ? "clean" : "violation");
  std::fflush(stdout);
  return ok;
}

}  // namespace

int main() {
  using namespace lfq_model;
  int failures = 0;

  std::printf("== positive: shipped orderings, expected clean ==\n");
  // The formal core, exhaustively: every interleaving of the 2-thread
  // protect/scan protocol. (Iteration counts are upper bounds; the
  // full/bound searches stop early when the space is exhausted.)
  failures += !run<hp_protocol_suite<hp_none>>(
      "hp protocol", rl::sched_full, 100000000, true);
  // Queue configurations: sampled interleaving + weak-memory search.
  failures += !run<queue_suite<bug_none, 1, 1, 2>>(
      "queue 1Px2 + 1C", rl::sched_bound, 10000000, true);
  failures += !run<queue_suite<bug_none, 1, 2, 1>>(
      "queue 1Px1 + 2C (stale-tail shape)", rl::sched_random, 300000, true);
  failures += !run<queue_suite<bug_none, 2, 1, 2>>(
      "queue 2Px2 + 1C (helping shape)", rl::sched_random, 300000, true);
  failures += !run<queue_suite<bug_none, 2, 2, 1>>(
      "queue 2Px1 + 2C", rl::sched_random, 300000, true);

  std::printf("== negative: one ordering weakened, expected violation ==\n");
  failures += !run<hp_protocol_suite<hp_missing_publish_fence>>(
      "hp publish fence removed", rl::sched_full, 100000000, false);
  failures += !run<hp_protocol_suite<hp_missing_scan_fence>>(
      "hp scan fence removed", rl::sched_full, 100000000, false);
  failures += !run<queue_suite<bug_relaxed_link_cas, 1, 1, 1>>(
      "queue link CAS release->relaxed", rl::sched_random, 100000, false);
  failures += !run<queue_suite<bug_relaxed_tail_load, 1, 2, 1>>(
      "queue dequeue tail load acquire->relaxed", rl::sched_random, 1000000,
      false);

  if (failures == 0) {
    std::printf("model_check: all runs matched expectation\n");
    return 0;
  }
  std::printf("model_check: %d run(s) diverged from expectation\n", failures);
  return 1;
}
