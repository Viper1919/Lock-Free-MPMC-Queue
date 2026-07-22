// Retire/free accounting for hazard-pointer reclamation (phase 2).
//
// What each test proves:
//
//  1. accounting balances — every successful dequeue retires exactly one
//     node, and a quiesced drain() frees all of them. retired == freed is
//     the books-balance milestone from the roadmap.
//  2. garbage is bounded — sampled DURING a concurrent run, in-flight
//     garbage stays under the O(threads x threshold) bound. This is the
//     property that justified choosing hazard pointers over epoch-based
//     reclamation, so it gets asserted, not just claimed. A leaky queue
//     fails this instantly (in-flight ~= total values), which is the
//     control proving the assertion has teeth.
//  3. a protected node survives its retirer's exit — a thread retires a
//     node another thread still protects, then exits. The node must stay
//     alive (we dereference it under ASan — a reclamation bug here is a
//     loud use-after-free, not a flaky counter), land on the orphan
//     stack, and be freed by a later scan on a different thread.
//
// The CI asan job runs this with leak detection ON. LSan is the other
// half of the accounting: retired==freed proves we freed what we
// retired; LSan proves nothing else was lost track of entirely.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#include "lfq/hazard_pointer.hpp"
#include "lfq/ms_queue.hpp"

namespace {

int failures = 0;

#define CHECK_MSG(cond, ...)                            \
  do {                                                  \
    if (!(cond)) {                                      \
      ++failures;                                       \
      std::printf("FAIL %s:%d  ", __FILE__, __LINE__);  \
      std::printf(__VA_ARGS__);                         \
      std::printf("\n");                                \
    }                                                   \
  } while (0)

using lfq::hp_domain;

std::size_t stress_per_producer() {
  std::size_t per_producer = 100000;
  if (const char* env = std::getenv("LFQ_STRESS_PER_PRODUCER")) {
    per_producer = std::strtoull(env, nullptr, 10);
    if (per_producer == 0) per_producer = 1000;
  }
  return per_producer;
}

void test_accounting_balances() {
  std::printf("[reclaim] accounting balances after quiesced drain\n");
  hp_domain::drain();
  const auto before = hp_domain::get_stats();

  constexpr std::uint64_t kOps = 50000;
  {
    lfq::ms_queue<std::uint64_t, lfq::hp_reclaimer> q;
    for (std::uint64_t i = 0; i < kOps; ++i) {
      q.enqueue(i);
      auto v = q.try_dequeue();
      CHECK_MSG(v.has_value() && *v == i, "lost value %llu",
                static_cast<unsigned long long>(i));
    }
  }  // ~ms_queue frees the live chain directly; only dequeues retire
  hp_domain::drain();
  const auto after = hp_domain::get_stats();

  CHECK_MSG(after.retired - before.retired == kOps,
            "expected %llu retires, saw %llu",
            static_cast<unsigned long long>(kOps),
            static_cast<unsigned long long>(after.retired - before.retired));
  CHECK_MSG(after.freed - before.freed == kOps,
            "expected %llu frees, saw %llu",
            static_cast<unsigned long long>(kOps),
            static_cast<unsigned long long>(after.freed - before.freed));
}

void test_garbage_is_bounded() {
  const unsigned hw = std::max(2u, std::thread::hardware_concurrency());
  const unsigned producers = std::max(1u, hw / 2);
  const unsigned consumers = std::max(1u, hw / 2);
  const std::size_t per_producer = stress_per_producer();
  const std::size_t total = producers * per_producer;
  std::printf("[reclaim] garbage bounded: %uP x %uC, %zu total values\n",
              producers, consumers, total);

  hp_domain::drain();
  const auto before = hp_domain::get_stats();
  CHECK_MSG(before.retired == before.freed,
            "books not balanced at test start");

  std::uint64_t max_in_flight = 0;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::minutes(2);
  bool deadline_hit = false;
  {
    lfq::ms_queue<std::uint64_t, lfq::hp_reclaimer> q;
    std::atomic<std::size_t> consumed{0};
    std::vector<std::thread> threads;
    threads.reserve(producers + consumers);

    for (unsigned p = 0; p < producers; ++p) {
      threads.emplace_back([&] {
        for (std::size_t i = 0; i < per_producer; ++i) {
          q.enqueue(i);
        }
      });
    }
    for (unsigned c = 0; c < consumers; ++c) {
      threads.emplace_back([&] {
        while (consumed.load() < total) {
          if (q.try_dequeue()) {
            consumed.fetch_add(1);
          } else {
            std::this_thread::yield();
          }
        }
      });
    }

    // Sample the garbage level while the run is hot. The counters are
    // relaxed, so a sample can transiently overestimate; the bound below
    // carries slack for that.
    while (consumed.load() < total) {
      max_in_flight = std::max(max_in_flight, hp_domain::in_flight());
      if (std::chrono::steady_clock::now() > deadline) {
        deadline_hit = true;
        consumed.store(total);  // release the consumers; fail below
        break;
      }
      std::this_thread::yield();
    }
    for (auto& t : threads) t.join();
  }
  CHECK_MSG(!deadline_hit, "watchdog deadline hit — values lost?");

  // True bound: threads x threshold in the bags, plus one node per
  // published hazard temporarily kept by scans. 2x absorbs the sampling
  // skew. The assertion is meaningful because total >> bound — a leaky
  // reclaimer would sample in-flight at ~total and fail loudly.
  const std::uint64_t bound =
      2 * (producers + consumers + 1) * hp_domain::scan_threshold();
  CHECK_MSG(total > bound,
            "test misconfigured: total %llu does not exceed bound %llu",
            static_cast<unsigned long long>(total),
            static_cast<unsigned long long>(bound));
  CHECK_MSG(max_in_flight <= bound,
            "garbage unbounded: peak in-flight %llu > bound %llu",
            static_cast<unsigned long long>(max_in_flight),
            static_cast<unsigned long long>(bound));

  // Workers exited (their leftovers scanned or orphaned); adopt and free
  // the rest. Every retire must now be matched by a free.
  hp_domain::drain();
  const auto after = hp_domain::get_stats();
  CHECK_MSG(after.retired - before.retired == total,
            "expected %llu retires, saw %llu",
            static_cast<unsigned long long>(total),
            static_cast<unsigned long long>(after.retired - before.retired));
  CHECK_MSG(after.retired == after.freed,
            "books not balanced after drain: retired=%llu freed=%llu",
            static_cast<unsigned long long>(after.retired),
            static_cast<unsigned long long>(after.freed));
}

void test_protected_node_survives_thread_exit() {
  std::printf("[reclaim] protected node survives its retirer's exit\n");
  hp_domain::drain();
  const auto before = hp_domain::get_stats();

  std::atomic<int*> src{new int(42)};
  std::atomic<int> stage{0};

  std::thread holder([&] {
    lfq::hp_reclaimer r;
    lfq::hp_reclaimer::guard<int> g(r);
    int* p = g.protect(0, src);
    CHECK_MSG(p != nullptr && *p == 42, "protect returned wrong node");
    stage.store(1);
    while (stage.load() < 2) std::this_thread::yield();
    // p has been retired, and its retirer has already exited — but our
    // hazard names it, so this dereference must be safe. Under ASan a
    // reclamation bug here is a loud use-after-free.
    CHECK_MSG(*p == 42, "protected node was reclaimed or clobbered");
  });
  while (stage.load() < 1) std::this_thread::yield();

  std::thread retirer([&] {
    lfq::hp_reclaimer r;
    r.retire(src.exchange(nullptr));
    // Exit with the node in our bag: the exit scan sees the holder's
    // hazard, keeps the node, and hands it to the orphan stack.
  });
  retirer.join();

  const auto mid = hp_domain::get_stats();
  CHECK_MSG(mid.retired - before.retired == 1, "retire not recorded");
  CHECK_MSG(mid.freed == before.freed,
            "node freed while still protected");

  stage.store(2);
  holder.join();

  hp_domain::drain();  // adopts the orphan; no hazard names it now
  const auto after = hp_domain::get_stats();
  CHECK_MSG(after.freed - before.freed == 1,
            "orphaned node was not reclaimed: freed delta %llu",
            static_cast<unsigned long long>(after.freed - before.freed));
}

}  // namespace

int main() {
  test_accounting_balances();
  test_garbage_is_bounded();
  test_protected_node_survives_thread_exit();

  if (failures == 0) {
    std::printf("reclaim_test: books balance, garbage bounded\n");
    return 0;
  }
  std::printf("reclaim_test: %d failure(s)\n", failures);
  return 1;
}
