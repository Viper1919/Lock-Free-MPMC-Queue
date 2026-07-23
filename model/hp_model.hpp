#pragma once

// Relacy model of the hazard-pointer protect/scan protocol — the part
// of this project where formal exploration matters most, because the
// correctness argument is a cross-thread store->load ordering claim
// that no amount of stress testing can prove.
//
// The model is the protocol skeleton, not the whole domain (records,
// bags, orphans are engineering around this core): one protector
// running the publish/verify loop from hp_reclaimer::guard::protect,
// one retirer running unlink -> fence -> scan -> free from
// hp_domain::scan. "Free" is modeled as a plain write into the node —
// so a protocol violation shows up as a Relacy DATA RACE between the
// protector's read and the poison write, which is exactly what a
// use-after-free is.
//
// The `hp_bug` knob weakens one claimed-load-bearing ordering per
// value; the checker is expected to FIND the race for each. hp_none
// must survive FULL interleaving search clean — that is the theorem
// (protect's seq_cst pair + scan's seq_cst fence => no missed hazard)
// checked exhaustively at this model size.

#include <relacy/relacy.hpp>

namespace lfq_model {

enum hp_bug {
  hp_none,
  // Protector's seq_cst fence (between publish store and verify load)
  // removed: the verify may be satisfied before the publish is
  // visible (store->load reordering), so verify passes against
  // pre-unlink state while scan reads the pre-publish slot. Both
  // sides miss each other; the node frees while protected.
  hp_missing_publish_fence,
  // Scanner's seq_cst fence removed (slot load stays acquire): same
  // race from the other side — the scanner's slot read may be
  // satisfied before its unlink is visible to the verify load.
  hp_missing_scan_fence,
};

template <hp_bug Bug>
struct hp_protocol_suite : rl::test_suite<hp_protocol_suite<Bug>, 2> {
  struct node {
    rl::var<int> data;
  };

  node n1, n2;
  rl::atomic<node*> src;   // the shared source (head_ in the queue)
  rl::atomic<node*> slot;  // one hazard slot

  void before() {
    n1.data($) = 1;
    n2.data($) = 2;
    src($).store(&n1, rl::mo_release);
    slot($).store(nullptr, rl::mo_relaxed);
  }

  void thread(unsigned idx) {
    if (idx == 0) {
      // Protector: guard::protect() verbatim — publish, fence,
      // re-verify (Michael's canonical fence formulation).
      node* p = src($).load(rl::mo_relaxed);
      for (;;) {
        slot($).store(p, rl::mo_release);
        if (Bug != hp_missing_publish_fence) {
          rl::atomic_thread_fence(rl::mo_seq_cst, $);
        }
        node* q = src($).load(rl::mo_acquire);
        if (q == p) break;
        p = q;
      }
      // Verified reachable after the hazard was visible: this read
      // must be safe. If the protocol is broken, it races the poison
      // write below — the model's use-after-free.
      int v = p->data($);
      RL_ASSERT(v == 1 || v == 2);
      slot($).store(nullptr, rl::mo_release);
    } else {
      // Retirer: unlink (release, like the queue's head_ CAS), then
      // the scan protocol from hp_domain::scan.
      node* old = src($).exchange(&n2, rl::mo_release);
      RL_ASSERT(old == &n1);
      if (Bug != hp_missing_scan_fence) {
        rl::atomic_thread_fence(rl::mo_seq_cst, $);
      }
      node* h = slot($).load(rl::mo_acquire);
      if (h != old) {
        old->data($) = 666;  // "free": poison the node
      }
    }
  }
};

}  // namespace lfq_model
