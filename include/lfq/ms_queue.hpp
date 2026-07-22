#pragma once

// Michael & Scott's lock-free MPMC queue (PODC 1996).
//
// Current state of this file, both deliberate:
//
//  1. LEAKY BY DEFAULT. retire() on leaky_reclaimer is a no-op. Phase 1
//     used this to isolate algorithm correctness from reclamation
//     correctness — two separate hard problems. Phase 2's hp_reclaimer
//     (hazard_pointer.hpp) plugs into the same seam and reclaims safely;
//     leaky stays on as the baseline that prices reclamation's cost.
//
//  2. EVERYTHING seq_cst. Every atomic op uses the default ordering.
//     Correct first; then relax one operation at a time, re-running TSan
//     and the stress suite after each, measuring what each relaxation
//     buys. The final orderings and their justifications will live in
//     DESIGN.md.
//
// The Reclaimer template parameter is the seam that makes reclamation
// swappable — the same queue can be benchmarked leaky vs. hazard-pointer
// vs. (stretch) epoch-based. Its contract, shaped for hazard pointers:
//
//   guard<Node> g(reclaimer);       // per-operation protection scope
//   Node* p = g.protect(slot, src); // load src, publish to slot, re-verify
//   g.set(slot, p);                 // publish only; CALLER must re-validate
//                                   //   against the source that proves p is
//                                   //   still reachable (see dequeue)
//   reclaimer.retire(p);            // p is unlinked; free when provably safe

#include <atomic>
#include <optional>
#include <utility>

#include "padding.hpp"

namespace lfq {

// Phase-1 reclaimer: never frees. Memory grows without bound — that
// growth is the problem statement for phase 2, not a bug.
struct leaky_reclaimer {
  template <class Node>
  struct guard {
    explicit guard(leaky_reclaimer&) {}
    Node* protect(unsigned /*slot*/, const std::atomic<Node*>& src) {
      return src.load();
    }
    void set(unsigned /*slot*/, Node* /*p*/) {}
  };

  template <class Node>
  void retire(Node* /*p*/) {}  // deliberate leak
};

// T must be copy-constructible: dequeue copies the value out of a node
// that other threads may still be reading (see the comment at that read).
// T must be default-constructible for the dummy node.
template <class T, class Reclaimer = leaky_reclaimer>
class ms_queue {
 private:
  struct node {
    std::atomic<node*> next{nullptr};
    T value{};

    node() = default;
    explicit node(T v) : value(std::move(v)) {}
  };

  using guard_t = typename Reclaimer::template guard<node>;

 public:
  ms_queue() {
    // Permanent dummy node: head_ and tail_ are never null, and
    // enqueue/dequeue never contend on an empty-queue special case.
    node* dummy = new node();
    head_.store(dummy);
    tail_.store(dummy);
  }

  ms_queue(const ms_queue&) = delete;
  ms_queue& operator=(const ms_queue&) = delete;

  // Destruction must be externally quiesced (no concurrent operations),
  // like every std container. Frees the live chain; already-retired nodes
  // are the reclaimer's responsibility (leaky: gone; hp: freed by scans).
  ~ms_queue() {
    node* p = head_.load();
    while (p != nullptr) {
      node* next = p->next.load();
      delete p;
      p = next;
    }
  }

  void enqueue(T value) {
    node* n = new node(std::move(value));
    guard_t g(reclaimer_);
    for (;;) {
      node* tail = g.protect(0, tail_);
      node* next = tail->next.load();
      if (tail != tail_.load()) continue;  // stale snapshot; retry

      if (next != nullptr) {
        // Tail is lagging: some enqueuer linked its node but hasn't swung
        // tail_ yet. Help it forward instead of waiting — this helping is
        // what makes the queue lock-free rather than obstruction-free: a
        // thread suspended between "link node" and "swing tail" cannot
        // stall anyone, because everyone else can finish its job.
        tail_.compare_exchange_weak(tail, next);
        continue;
      }

      node* expected = nullptr;
      if (tail->next.compare_exchange_weak(expected, n)) {
        // Linked: n is now visible to every thread. Try to swing tail_;
        // failure is fine — it means another thread already helped.
        tail_.compare_exchange_strong(tail, n);
        return;
      }
    }
  }

  std::optional<T> try_dequeue() {
    guard_t g(reclaimer_);
    for (;;) {
      node* head = g.protect(0, head_);
      node* tail = tail_.load();
      node* next = head->next.load();
      g.set(1, next);
      // Re-validating head_ (not head->next) is what makes `next` safe
      // under hazard pointers later: if head_ still equals head, then
      // next has not been dequeued, so it cannot have been retired.
      // Re-reading head->next would NOT prove that — a dequeued head
      // keeps its next pointer, so that check passes on a retired node.
      if (head != head_.load()) continue;

      if (head == tail) {
        if (next == nullptr) return std::nullopt;  // empty
        // Tail lagging behind a linked node; help swing it, then retry.
        // Dequeue helping enqueue is still the same progress argument.
        tail_.compare_exchange_weak(tail, next);
        continue;
      }

      // Read the value BEFORE the CAS publishes this dequeue. After the
      // CAS succeeds, `next` is the new head — another thread can dequeue
      // past it and retire it, so reading afterwards is a use-after-free
      // once reclamation is real. This is the ordering bug everyone
      // writes at least once; writing it correctly the first time.
      T value = next->value;
      if (head_.compare_exchange_weak(head, next)) {
        // Old dummy is unlinked and unreachable from head_/tail_.
        reclaimer_.retire(head);
        return value;
      }
    }
  }

  // Approximate, for tests and quiesced states only: exact answers about
  // emptiness don't exist mid-flight in an MPMC queue.
  bool empty() const {
    node* head = head_.load();
    return head->next.load() == nullptr;
  }

 private:
  // head_ and tail_ on separate cache lines: enqueuers hammer tail_,
  // dequeuers hammer head_, and sharing a line would make each side
  // invalidate the other's cache for no logical reason. Phase 3b measures
  // exactly what this padding is worth.
  alignas(cache_line_size) std::atomic<node*> head_;
  alignas(cache_line_size) std::atomic<node*> tail_;

  Reclaimer reclaimer_;
};

}  // namespace lfq
