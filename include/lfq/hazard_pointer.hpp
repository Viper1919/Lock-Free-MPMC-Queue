#pragma once

// Hazard-pointer safe memory reclamation (Michael, IEEE TPDS 2004).
//
// The problem this solves: a reader loads a node pointer and gets
// preempted; a writer unlinks and frees that node; the reader resumes and
// dereferences freed memory. Worse, the allocator recycles the address, a
// new node lands there, and the reader's CAS *succeeds* on the wrong node
// — ABA, which is silent corruption, not a crash. Tagged pointers defeat
// the ABA symptom but not the use-after-free; the only cure is knowing
// when no thread can still be reading a retired node.
//
// The hazard-pointer answer: before dereferencing a shared pointer, a
// thread PUBLISHES it to a slot that all reclaiming threads inspect, then
// RE-READS the source to prove the pointer was still live at publication
// time (publish-without-re-verify is the classic bug — the node can be
// retired between the first read and the publish). Retired nodes go to a
// per-thread list; when the list crosses a threshold, the thread snapshots
// every published hazard and frees exactly the retired nodes nobody has
// published. Two guarantees fall out:
//
//   - A protected node is never freed (no use-after-free, no ABA).
//   - Unreclaimed garbage is BOUNDED by O(records x K + per-thread
//     threshold) — unlike epoch schemes, a stalled thread pins only what
//     its own K slots name, not the whole retire backlog. That bound is
//     the property that justifies choosing HP over EBR (see README).
//
// Memory ordering (relaxed from all-seq_cst after the phase-3a
// verification net existed; the table lives in DESIGN.md). The one part
// that CANNOT weaken is the protect protocol: the publish store and the
// re-verify load must not reorder — a store->load edge that
// release/acquire cannot forbid. The barrier is written as Michael's
// canonical FENCE formulation — publish (release), seq_cst fence,
// verify (acquire) — pairing with the scanner's own seq_cst fence
// through the standard's unambiguous fence-fence rule ([atomics.fences]
// p7 wording). An earlier revision used a seq_cst store + seq_cst load
// instead, leaning on the subtler mixed op/fence rules; the Relacy
// model checker (model/) cannot verify that variant, and the fence form
// is what it verifies exhaustively — so the fence form is what ships.
// This fence is the per-access cost the EBR-vs-HP table talks about.
//
// Structure (one process-wide domain; all queues share it):
//
//   hazard record   K=2 atomic slots + active flag, one per thread, on a
//                   grow-only lock-free list. Released records (active =
//                   false) are recycled by later threads, never freed —
//                   memory high-water-marks at peak thread concurrency.
//   retire list     thread-local vector of {ptr, deleter}; no contention.
//   scan            when the list reaches max(64, 2*K*records): snapshot
//                   all hazards, sort, free every retired node not found.
//   orphans         a thread exiting with still-protected retired nodes
//                   pushes them to a global Treiber stack; any later scan
//                   adopts them. Nothing is ever silently dropped.
//
// The accounting counters (retired/freed) are relaxed atomics on the cold
// retire/scan path only — the per-op protect path is untouched. They are
// load-bearing for reclaim_test, which asserts the books balance.

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "inject.hpp"

namespace lfq {

class hp_domain {
 public:
  // K: dequeue needs two slots live at once (head and head->next);
  // enqueue needs one. Per Michael's paper this is the MS-queue minimum.
  static constexpr unsigned kSlots = 2;

  struct record {
    std::atomic<void*> slot[kSlots] = {};
    std::atomic<bool> active{true};
    record* next = nullptr;  // immutable once linked; the list only grows
  };

  // The calling thread's hazard record, acquired on first use and held
  // until thread exit (recycled, not freed). Guards cache this pointer so
  // the hot path never touches the thread_local more than once per op.
  static record* local_record() {
    tl_cache& tl = local();
    if (tl.rec == nullptr) tl.rec = acquire_record();
    return tl.rec;
  }

  // p has been unlinked from every shared location; free it once no
  // hazard names it. Deleter is a plain function pointer so retired nodes
  // from any queue instantiation coexist in one list.
  static void retire(void* p, void (*deleter)(void*)) {
    tl_cache& tl = local();
    tl.bag.push_back({p, deleter});
    retired_count_.fetch_add(1, std::memory_order_relaxed);
    if (tl.bag.size() >= scan_threshold()) scan(tl.bag);
  }

  // Scan trigger, and the garbage bound's second term: a thread frees no
  // later than every threshold retires, so per-thread backlog is bounded.
  static std::size_t scan_threshold() {
    return std::max<std::size_t>(
        64, 2 * kSlots * nrecords_.load(std::memory_order_relaxed));
  }

  // Best-effort scan on the calling thread (adopts orphans first). For
  // tests and quiesced shutdown; correctness never requires calling it.
  static void drain() { scan(local().bag); }

  struct stats {
    std::uint64_t retired;
    std::uint64_t freed;
  };
  static stats get_stats() {
    return {retired_count_.load(std::memory_order_relaxed),
            freed_count_.load(std::memory_order_relaxed)};
  }
  // Retired-but-not-yet-freed. Sampled concurrently this is approximate:
  // the counters cannot be read atomically together, and being relaxed
  // they give a racing sampler no cross-counter ordering, so a sample
  // can skew either way. Reading freed FIRST plus the clamp stops a
  // fresh-freed/stale-retired sample from underflowing to ~2^64 (seen
  // once under TSan); the residual skew is why reclaim_test's bound
  // carries slack.
  static std::uint64_t in_flight() {
    std::uint64_t freed = freed_count_.load(std::memory_order_relaxed);
    std::uint64_t retired = retired_count_.load(std::memory_order_relaxed);
    return retired > freed ? retired - freed : 0;
  }

 private:
  struct retired_item {
    void* ptr;
    void (*deleter)(void*);
  };

  struct orphan {
    retired_item item;
    orphan* next;
  };

  struct tl_cache {
    record* rec = nullptr;
    std::vector<retired_item> bag;

    ~tl_cache() {
      // Thread exit. Free what is already safe; hand anything still
      // protected by OTHER threads' hazards to the orphan stack so a
      // later scan (any thread) reclaims it. Then release the record for
      // recycling. Our own slots are clear — no guard outlives its op.
      if (!bag.empty()) {
        scan(bag);
        for (const retired_item& r : bag) push_orphan(r);
        bag.clear();
      }
      if (rec != nullptr) {
        // release x2: the slot clears must be visible to whoever
        // acquire-reads active==false and recycles this record.
        for (std::atomic<void*>& s : rec->slot) {
          s.store(nullptr, std::memory_order_release);
        }
        rec->active.store(false, std::memory_order_release);
      }
    }
  };

  static tl_cache& local() {
    thread_local tl_cache tl;
    return tl;
  }

  static record* acquire_record() {
    // Recycle a released record if one exists (CAS only succeeds on
    // active == false, so live records are never stolen). acquire on
    // success: imports the releasing thread's slot clears. relaxed on
    // failure: the value is only compared, never dereferenced through.
    for (record* r = head_.load(std::memory_order_acquire); r != nullptr;
         r = r->next) {
      bool expected = false;
      if (r->active.compare_exchange_strong(expected, true,
                                            std::memory_order_acquire,
                                            std::memory_order_relaxed)) {
        return r;
      }
    }
    // ...else push a fresh one. CAS loop on the list head; the list only
    // grows, so traversal never races with removal. release publishes
    // the record's fields; failure just reloads the head (relaxed).
    record* r = new record();
    nrecords_.fetch_add(1, std::memory_order_relaxed);
    record* h = head_.load(std::memory_order_relaxed);
    do {
      r->next = h;
    } while (!head_.compare_exchange_weak(h, r, std::memory_order_release,
                                          std::memory_order_relaxed));
    return r;
  }

  static void push_orphan(const retired_item& item) {
    // release: publishes the orphan's item (and the retirement history
    // that led here) to whichever thread adopts the stack.
    orphan* o = new orphan{item, orphans_.load(std::memory_order_relaxed)};
    while (!orphans_.compare_exchange_weak(o->next, o,
                                           std::memory_order_release,
                                           std::memory_order_relaxed)) {
    }
  }

  static void adopt_orphans(std::vector<retired_item>& bag) {
    // exchange takes the whole stack at once — pop-all has no ABA window.
    // acquire: pairs with push_orphan's release; the adopter must see the
    // orphan chain's contents and the retirer's unlink before it.
    orphan* o = orphans_.exchange(nullptr, std::memory_order_acquire);
    while (o != nullptr) {
      bag.push_back(o->item);
      orphan* next = o->next;
      delete o;
      o = next;
    }
  }

  static void scan(std::vector<retired_item>& bag) {
    adopt_orphans(bag);
    if (bag.empty()) return;

    // Snapshot every published hazard. A hazard published AFTER this
    // snapshot cannot name anything in `bag`: retire()'s contract is that
    // the node was already unlinked, so protect()'s re-verify against the
    // source would fail and discard such a pointer.
    //
    // The fence is the scanner's half of the protect protocol and cannot
    // be weaker: it orders every unlink that led to a bag entry before
    // the slot reads below, pairing with the fence in protect()/set()
    // via the fence-fence rule. Without it, "verify saw the node still
    // linked" and "scan saw the slot still empty" can BOTH happen — the
    // classic store-load race, now between two threads — and a
    // protected node gets freed (model/hp_model.hpp demonstrates it).
    // One fence amortized over the whole snapshot is why the per-slot
    // loads can then be acquire: a slot value other than p (null via
    // the guard's release, or a later release publish) synchronizes-
    // with its store, so the protector's reads of p happened-before the
    // free that follows.
    std::atomic_thread_fence(std::memory_order_seq_cst);
    std::vector<void*> hazards;
    hazards.reserve(kSlots * nrecords_.load(std::memory_order_relaxed));
    for (record* r = head_.load(std::memory_order_acquire); r != nullptr;
         r = r->next) {
      for (const std::atomic<void*>& s : r->slot) {
        void* p = s.load(std::memory_order_acquire);
        if (p != nullptr) hazards.push_back(p);
      }
    }
    std::sort(hazards.begin(), hazards.end());
    LFQ_INJECT();  // window: snapshot taken, frees not yet performed

    std::size_t kept = 0;
    std::uint64_t freed = 0;
    for (retired_item& r : bag) {
      if (std::binary_search(hazards.begin(), hazards.end(), r.ptr)) {
        bag[kept++] = r;  // still protected — keep for a later scan
      } else {
        r.deleter(r.ptr);
        ++freed;
      }
    }
    bag.resize(kept);
    freed_count_.fetch_add(freed, std::memory_order_relaxed);
  }

  // Trivially-destructible globals only: no static-destruction-order
  // hazards, and whatever is still linked at process exit stays reachable
  // from these, which leak checkers correctly treat as not-a-leak.
  inline static std::atomic<record*> head_{nullptr};
  inline static std::atomic<orphan*> orphans_{nullptr};
  inline static std::atomic<std::size_t> nrecords_{0};
  inline static std::atomic<std::uint64_t> retired_count_{0};
  inline static std::atomic<std::uint64_t> freed_count_{0};
};

// Reclaimer implementing the seam ms_queue was built around. Stateless:
// all state lives in hp_domain (per-thread records + retire lists), so a
// per-queue member costs nothing and every queue shares one domain.
struct hp_reclaimer {
  template <class Node>
  struct guard {
    explicit guard(hp_reclaimer&) : rec_(hp_domain::local_record()) {}

    guard(const guard&) = delete;
    guard& operator=(const guard&) = delete;

    // One guard per queue operation; guards on the same thread must not
    // nest (they would share the thread's K slots).
    ~guard() {
      // release: a scan that reads the null must also see every read we
      // made of the node while it was protected — that edge is what
      // makes the subsequent free safe.
      for (std::atomic<void*>& s : rec_->slot) {
        s.store(nullptr, std::memory_order_release);
      }
    }

    // The load->publish->re-verify loop. The re-read of src is the whole
    // trick: it proves the pointer was still reachable from src AFTER the
    // hazard became visible, so no scan that could free it can have
    // missed the hazard.
    //
    // The seq_cst fence between publish and verify CANNOT be removed:
    // they are a store->load pair, the one shape release/acquire cannot
    // order. Without it the publish can pass the verify — scan misses
    // the hazard while we miss the retirement, and we hold a freed
    // node. It pairs with the fence in scan(); model/hp_model.hpp
    // checks exactly this protocol exhaustively, and its negative
    // variant (fence removed) demonstrates the race. This fence is the
    // irreducible per-access cost of hazard pointers. The publish store
    // is release (not relaxed) so a scan that reads any LATER slot
    // value synchronizes with it — that path is what makes freeing a
    // previously-protected node safe.
    Node* protect(unsigned slot, const std::atomic<Node*>& src) {
      Node* p = src.load(std::memory_order_relaxed);
      for (;;) {
        LFQ_INJECT();  // window: pointer read, hazard not yet published
        rec_->slot[slot].store(p, std::memory_order_release);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        LFQ_INJECT();  // window: hazard published, not yet re-verified
        Node* q = src.load(std::memory_order_acquire);
        if (q == p) return p;
        p = q;
      }
    }

    // Publish only — no re-verify against src here. The CALLER must
    // re-validate against whatever source proves p is still reachable
    // (ms_queue::try_dequeue re-checks head_ after set(1, next); see the
    // comment there for why that check, and not head->next, is the one
    // that makes `next` safe). Same store->fence protocol as protect();
    // the caller's re-validating load is the verify half.
    void set(unsigned slot, Node* p) {
      rec_->slot[slot].store(p, std::memory_order_release);
      std::atomic_thread_fence(std::memory_order_seq_cst);
    }

   private:
    hp_domain::record* rec_;
  };

  template <class Node>
  void retire(Node* p) {
    hp_domain::retire(p, [](void* q) { delete static_cast<Node*>(q); });
  }
};

}  // namespace lfq
