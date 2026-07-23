#pragma once

// Relacy port of ms_queue (leaky reclamation) for model checking.
//
// Honesty note, stated rather than hidden: Relacy cannot consume the
// production header — it needs its own atomic types with `($)` debug
// instrumentation — so this is a LINE-PARALLEL PORT of ms_queue.hpp,
// kept in the same operation order with the same memory orderings.
// What the checker verifies is therefore the algorithm + ordering
// choices, on a copy that must be diffed against the original by eye
// when either changes. The production sources stay covered by TSan and
// the adversarial suite; this layer's job is to explore interleavings
// and weak-memory behaviors those can only sample.
//
// The `queue_bug` knob exists for NEGATIVE checks: each value weakens
// exactly one ordering the design claims is load-bearing, and the
// model checker is expected to FIND the resulting violation. A
// verifier that has never been watched failing is itself unverified.

#include <relacy/relacy.hpp>

#include <cstring>
#include <vector>

namespace lfq_model {

enum queue_bug {
  bug_none,
  // Dequeue's tail_ load relaxed instead of acquire. Breaks the
  // "head != tail implies head->next != null" happens-before chain
  // (see the comment at that load in ms_queue.hpp): a stale tail can
  // appear BEHIND head, and the value branch dereferences null.
  bug_relaxed_tail_load,
  // Link CAS relaxed instead of release: the node's contents are
  // unpublished, and a dequeuer's value read races the producer's
  // construction.
  bug_relaxed_link_cas,
};

template <queue_bug Bug>
struct ms_queue_m {
  struct node {
    rl::atomic<node*> next;
    rl::var<unsigned> value;
  };

  rl::atomic<node*> head_;
  rl::atomic<node*> tail_;
  std::vector<node*> junk_;  // "retired" (leaky) — freed in teardown()

  void init() {
    node* dummy = new node();
    dummy->next($).store(nullptr, rl::mo_relaxed);
    head_($).store(dummy, rl::mo_relaxed);
    tail_($).store(dummy, rl::mo_relaxed);
    junk_.clear();
  }

  void teardown() {
    node* p = head_($).load(rl::mo_relaxed);
    while (p != nullptr) {
      node* next = p->next($).load(rl::mo_relaxed);
      delete p;
      p = next;
    }
    for (node* r : junk_) delete r;
    junk_.clear();
  }

  // Mirrors ms_queue::enqueue. leaky guard's protect() is an acquire
  // load of the source, inlined here.
  void enqueue(unsigned value) {
    node* n = new node();
    n->value($) = value;
    n->next($).store(nullptr, rl::mo_relaxed);
    for (;;) {
      node* tail = tail_($).load(rl::mo_acquire);
      node* next = tail->next($).load(rl::mo_acquire);
      if (tail != tail_($).load(rl::mo_acquire)) continue;

      if (next != nullptr) {
        tail_($).compare_exchange_weak(tail, next, rl::mo_release,
                                       rl::mo_relaxed);
        continue;
      }

      node* expected = nullptr;
      rl::memory_order const link_mo =
          Bug == bug_relaxed_link_cas ? rl::mo_relaxed : rl::mo_release;
      if (tail->next($).compare_exchange_weak(expected, n, link_mo,
                                              rl::mo_relaxed)) {
        tail_($).compare_exchange_strong(tail, n, rl::mo_release,
                                         rl::mo_relaxed);
        return;
      }
    }
  }

  // Mirrors ms_queue::try_dequeue (leaky: set()'s fence is absent, and
  // the head_ re-validation is the same acquire the production code
  // uses — under leaky reclamation it is a staleness heuristic).
  bool try_dequeue(unsigned& out) {
    for (;;) {
      node* head = head_($).load(rl::mo_acquire);
      rl::memory_order const tail_mo =
          Bug == bug_relaxed_tail_load ? rl::mo_relaxed : rl::mo_acquire;
      node* tail = tail_($).load(tail_mo);
      node* next = head->next($).load(rl::mo_acquire);
      if (head != head_($).load(rl::mo_acquire)) continue;

      if (head == tail) {
        if (next == nullptr) return false;
        tail_($).compare_exchange_weak(tail, next, rl::mo_release,
                                       rl::mo_relaxed);
        continue;
      }

      // THE invariant under test: head strictly behind tail implies a
      // linked successor. bug_relaxed_tail_load breaks exactly this.
      RL_ASSERT(next != nullptr);

      unsigned value = next->value($);
      if (head_($).compare_exchange_weak(head, next, rl::mo_release,
                                         rl::mo_relaxed)) {
        junk_.push_back(head);
        out = value;
        return true;
      }
    }
  }
};

// P producers x C consumers, K values per producer. Asserts, inside
// the model: corruption-free values, per-producer FIFO, exactly-once,
// drain-to-empty. Values encode producer<<8|seq.
template <queue_bug Bug, unsigned P, unsigned C, unsigned K>
struct queue_suite : rl::test_suite<queue_suite<Bug, P, C, K>, P + C> {
  ms_queue_m<Bug> q;
  unsigned seen[P][K];
  rl::atomic<unsigned> consumed;

  void before() {
    q.init();
    std::memset(&seen[0][0], 0, sizeof seen);
    consumed($).store(0, rl::mo_relaxed);
  }

  void thread(unsigned idx) {
    if (idx < P) {
      for (unsigned s = 0; s < K; ++s) {
        q.enqueue((idx << 8) | s);
      }
    } else {
      int last[P];
      for (unsigned p = 0; p < P; ++p) last[p] = -1;
      while (consumed($).load(rl::mo_relaxed) < P * K) {
        unsigned v = 0;
        if (q.try_dequeue(v)) {
          unsigned const p = v >> 8;
          unsigned const s = v & 0xffu;
          RL_ASSERT(p < P && s < K);          // no corruption
          RL_ASSERT(static_cast<int>(s) > last[p]);  // per-producer FIFO
          last[p] = static_cast<int>(s);
          RL_ASSERT(seen[p][s] == 0);         // no duplication
          seen[p][s] = 1;
          consumed($).fetch_add(1, rl::mo_relaxed);
        } else {
          rl::yield(1, $);
        }
      }
    }
  }

  void after() {
    unsigned v = 0;
    RL_ASSERT(!q.try_dequeue(v));             // drain to empty
    for (unsigned p = 0; p < P; ++p) {
      for (unsigned s = 0; s < K; ++s) {
        RL_ASSERT(seen[p][s] == 1);           // no loss
      }
    }
    q.teardown();
  }
};

}  // namespace lfq_model
