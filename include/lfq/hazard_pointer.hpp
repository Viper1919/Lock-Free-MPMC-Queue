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
// Phase-2 discipline, same as the queue: every atomic op is seq_cst.
// Correct first, relax later. One ordering note for that later phase: the
// publish store and the re-verify load must not reorder, which needs the
// two seq_cst ops (a full barrier) — plain release/acquire is NOT enough,
// and this store->load fence is exactly the per-access cost that the
// EBR-vs-HP tradeoff table talks about.
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
  // Retired-but-not-yet-freed. Sampled concurrently this is approximate
  // (the two counters are not read atomically together), but it can only
  // overestimate briefly; reclaim_test asserts the bound with slack.
  static std::uint64_t in_flight() {
    stats s = get_stats();
    return s.retired - s.freed;
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
        for (std::atomic<void*>& s : rec->slot) s.store(nullptr);
        rec->active.store(false);
      }
    }
  };

  static tl_cache& local() {
    thread_local tl_cache tl;
    return tl;
  }

  static record* acquire_record() {
    // Recycle a released record if one exists (CAS only succeeds on
    // active == false, so live records are never stolen)...
    for (record* r = head_.load(); r != nullptr; r = r->next) {
      bool expected = false;
      if (r->active.compare_exchange_strong(expected, true)) return r;
    }
    // ...else push a fresh one. CAS loop on the list head; the list only
    // grows, so traversal never races with removal.
    record* r = new record();
    nrecords_.fetch_add(1, std::memory_order_relaxed);
    record* h = head_.load();
    do {
      r->next = h;
    } while (!head_.compare_exchange_weak(h, r));
    return r;
  }

  static void push_orphan(const retired_item& item) {
    orphan* o = new orphan{item, orphans_.load()};
    while (!orphans_.compare_exchange_weak(o->next, o)) {
    }
  }

  static void adopt_orphans(std::vector<retired_item>& bag) {
    // exchange takes the whole stack at once — pop-all has no ABA window.
    orphan* o = orphans_.exchange(nullptr);
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
    std::vector<void*> hazards;
    hazards.reserve(kSlots * nrecords_.load(std::memory_order_relaxed));
    for (record* r = head_.load(); r != nullptr; r = r->next) {
      for (const std::atomic<void*>& s : r->slot) {
        void* p = s.load();
        if (p != nullptr) hazards.push_back(p);
      }
    }
    std::sort(hazards.begin(), hazards.end());

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
      for (std::atomic<void*>& s : rec_->slot) s.store(nullptr);
    }

    // The load->publish->re-verify loop. The re-read of src is the whole
    // trick: it proves the pointer was still reachable from src AFTER the
    // hazard became visible, so no scan that could free it can have
    // missed the hazard.
    Node* protect(unsigned slot, const std::atomic<Node*>& src) {
      Node* p = src.load();
      for (;;) {
        rec_->slot[slot].store(p);
        Node* q = src.load();
        if (q == p) return p;
        p = q;
      }
    }

    // Publish only — no re-verify against src here. The CALLER must
    // re-validate against whatever source proves p is still reachable
    // (ms_queue::try_dequeue re-checks head_ after set(1, next); see the
    // comment there for why that check, and not head->next, is the one
    // that makes `next` safe).
    void set(unsigned slot, Node* p) { rec_->slot[slot].store(p); }

   private:
    hp_domain::record* rec_;
  };

  template <class Node>
  void retire(Node* p) {
    hp_domain::retire(p, [](void* q) { delete static_cast<Node*>(q); });
  }
};

}  // namespace lfq
