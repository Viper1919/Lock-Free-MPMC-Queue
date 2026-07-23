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
//  2. EXPLICIT ORDERINGS, arrived at in that order: everything was
//     seq_cst until the phase-3a verification net (TSan + adversarial
//     scheduling on a weakly-ordered AArch64 machine) existed to catch a
//     wrong relaxation, then each operation was relaxed deliberately
//     with the invariant that justifies it in a comment at the site.
//     The full ordering table and the measured payoff live in DESIGN.md.
//     The strongest ordering in the hot path is the seq_cst fence inside
//     the hazard publish (hazard_pointer.hpp) — the one barrier the
//     protocol cannot live without.
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

#include "inject.hpp"
#include "padding.hpp"

namespace lfq {

// Phase-1 reclaimer: never frees. Memory grows without bound — that
// growth is the problem statement for phase 2, not a bug.
struct leaky_reclaimer {
  template <class Node>
  struct guard {
    explicit guard(leaky_reclaimer&) {}
    Node* protect(unsigned /*slot*/, const std::atomic<Node*>& src) {
      // acquire: the caller dereferences the result, so it must see the
      // node contents the linking CAS released.
      return src.load(std::memory_order_acquire);
    }
    void set(unsigned /*slot*/, Node* /*p*/) {}
  };

  template <class Node>
  void retire(Node* /*p*/) {}  // deliberate leak
};

// T must be copy-constructible: dequeue copies the value out of a node
// that other threads may still be reading (see the comment at that read).
// T must be default-constructible for the dummy node.
//
// PadHeadTail exists so the false-sharing claim below can be MEASURED
// rather than asserted: the same queue builds with head_/tail_ on
// separate cache lines (true, the default) or adjacent (false), and the
// phase-3b sweep prices the difference.
template <class T, class Reclaimer = leaky_reclaimer, bool PadHeadTail = true>
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
      LFQ_INJECT();  // window: tail protected, snapshot not yet validated
      // acquire: pairs with the release CAS that linked whatever node we
      // may find here — if next is non-null we pass it onward as tail_.
      node* next = tail->next.load(std::memory_order_acquire);
      // Staleness heuristic only: correctness rests on the CASes below,
      // and hazard validity on protect()'s own re-verify — so acquire,
      // not seq_cst, is enough for this recheck.
      if (tail != tail_.load(std::memory_order_acquire)) continue;

      if (next != nullptr) {
        // Tail is lagging: some enqueuer linked its node but hasn't swung
        // tail_ yet. Help it forward instead of waiting — this helping is
        // what makes the queue lock-free rather than obstruction-free: a
        // thread suspended between "link node" and "swing tail" cannot
        // stall anyone, because everyone else can finish its job.
        // release: republishes next (whose contents the acquire above
        // imported) for every later acquire load of tail_. Failure value
        // is discarded, so relaxed.
        tail_.compare_exchange_weak(tail, next, std::memory_order_release,
                                    std::memory_order_relaxed);
        continue;
      }

      node* expected = nullptr;
      LFQ_INJECT();  // window: validated snapshot going stale before CAS
      // release on success: THE publication point — n's value and
      // next=nullptr must be complete before any thread can reach n.
      // relaxed on failure: the loaded value is discarded on retry.
      if (tail->next.compare_exchange_weak(expected, n,
                                           std::memory_order_release,
                                           std::memory_order_relaxed)) {
        // Linked: n is now visible to every thread. Try to swing tail_;
        // failure is fine — it means another thread already helped.
        LFQ_INJECT();  // window: node linked, tail lagging — helping fires
        // release: dequeue's head!=tail invariant is a happens-before
        // chain through tail_ swings (see the tail load in try_dequeue).
        tail_.compare_exchange_strong(tail, n, std::memory_order_release,
                                      std::memory_order_relaxed);
        return;
      }
    }
  }

  std::optional<T> try_dequeue() {
    guard_t g(reclaimer_);
    for (;;) {
      node* head = g.protect(0, head_);
      LFQ_INJECT();  // window: head protected, next not yet read
      // acquire, and not weaker: the head!=tail branch below dereferences
      // next unconditionally, which is only safe because "head is behind
      // tail" implies head->next != nullptr. That invariant is a
      // happens-before chain — whoever advanced head_ first observed
      // (acquire) or performed (release) the tail_ swing — and this
      // load's acquire is the link that imports it. Relax it and a stale
      // tail can appear BEHIND head, making next null on that branch.
      node* tail = tail_.load(std::memory_order_acquire);
      // acquire: next is dereferenced below (value read).
      node* next = head->next.load(std::memory_order_acquire);
      g.set(1, next);
      LFQ_INJECT();  // window: next's hazard published, not yet validated
      // Re-validating head_ (not head->next) is what makes `next` safe
      // under hazard pointers: if head_ still equals head, then next has
      // not been dequeued, so it cannot have been retired. Re-reading
      // head->next would NOT prove that — a dequeued head keeps its next
      // pointer, so that check passes on a retired node.
      // This load is the verify half of the hazard publish protocol;
      // the store->load ordering it needs comes from the seq_cst fence
      // inside set() (see hazard_pointer.hpp), so acquire suffices
      // here. Under leaky_reclaimer the check is a staleness heuristic.
      if (head != head_.load(std::memory_order_acquire)) continue;

      if (head == tail) {
        if (next == nullptr) return std::nullopt;  // empty
        // Tail lagging behind a linked node; help swing it, then retry.
        // Dequeue helping enqueue is still the same progress argument.
        // release/relaxed for the same reasons as enqueue's helping CAS.
        tail_.compare_exchange_weak(tail, next, std::memory_order_release,
                                    std::memory_order_relaxed);
        continue;
      }

      // Read the value BEFORE the CAS publishes this dequeue. After the
      // CAS succeeds, `next` is the new head — another thread can dequeue
      // past it and retire it, so reading afterwards is a use-after-free
      // once reclamation is real. This is the ordering bug everyone
      // writes at least once; writing it correctly the first time.
      T value = next->value;
      LFQ_INJECT();  // window: value read, dequeue not yet published
      // release on success: feeds the same chain the tail load above
      // consumes — a thread that sees the new head_ must also see the
      // tail_ swing that had to precede it. relaxed on failure (retry).
      if (head_.compare_exchange_weak(head, next, std::memory_order_release,
                                      std::memory_order_relaxed)) {
        // Old dummy is unlinked and unreachable from head_/tail_.
        LFQ_INJECT();  // window: head swung, node not yet retired
        reclaimer_.retire(head);
        return value;
      }
    }
  }

  // Approximate, for tests and quiesced states only: exact answers about
  // emptiness don't exist mid-flight in an MPMC queue.
  bool empty() const {
    node* head = head_.load(std::memory_order_acquire);
    return head->next.load(std::memory_order_acquire) == nullptr;
  }

 private:
  // head_ and tail_ on separate cache lines: enqueuers hammer tail_,
  // dequeuers hammer head_, and sharing a line would make each side
  // invalidate the other's cache for no logical reason. The unpadded
  // variant exists purely to measure what this costs (see PadHeadTail).
  static constexpr std::size_t head_tail_align =
      PadHeadTail ? cache_line_size : alignof(std::atomic<node*>);
  alignas(head_tail_align) std::atomic<node*> head_;
  alignas(head_tail_align) std::atomic<node*> tail_;

  Reclaimer reclaimer_;
};

}  // namespace lfq
