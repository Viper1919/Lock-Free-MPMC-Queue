// Concurrent invariants under P producers x C consumers:
//
//  1. No loss, no duplication — every enqueued value is dequeued exactly
//     once (per-value atomic counter array, checked to be all-ones).
//  2. Per-producer FIFO — each producer enqueues strictly increasing
//     sequence numbers tagged with its id; every consumer must observe any
//     given producer's values in increasing order. Global FIFO is not a
//     meaningful MPMC assertion; per-producer order is, and it is a real
//     linearizability proxy that catches genuine ordering bugs.
//  3. Drain to empty — after all threads join, consumed == produced and
//     the queue reports empty.
//
// Runs each queue at hardware concurrency AND at ~2x oversubscription:
// preemption inside the enqueue/dequeue windows is where the bugs live.
//
// LFQ_STRESS_PER_PRODUCER (env) scales iteration count down for
// sanitizer runs (~10x slowdown) without changing the schedule shape.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#include "leak_check.hpp"
#include "lfq/hazard_pointer.hpp"
#include "lfq/ms_queue.hpp"
#include "lfq/mutex_queue.hpp"

namespace {

int failures = 0;

#define CHECK_MSG(cond, ...)              \
  do {                                    \
    if (!(cond)) {                        \
      ++failures;                         \
      std::printf("FAIL %s:%d  ", __FILE__, __LINE__); \
      std::printf(__VA_ARGS__);           \
      std::printf("\n");                  \
    }                                     \
  } while (0)

constexpr std::uint64_t encode(unsigned producer, std::uint64_t seq) {
  return (static_cast<std::uint64_t>(producer) << 32) | seq;
}

struct config {
  unsigned producers;
  unsigned consumers;
  std::size_t per_producer;
};

template <class Q>
void run_stress(const char* queue_name, config cfg) {
  const std::size_t total = cfg.producers * cfg.per_producer;
  std::printf("[stress] %-6s  %uP x %uC, %zu values/producer (%zu total)\n",
              queue_name, cfg.producers, cfg.consumers, cfg.per_producer,
              total);

  Q queue;
  std::vector<std::atomic<std::uint8_t>> seen(total);  // value-initialized: 0
  std::atomic<std::size_t> consumed{0};
  std::atomic<bool> deadline_hit{false};

  // Watchdog instead of a hang: a lost value would otherwise spin the
  // consumers forever, which reports nothing. A tripped deadline IS the
  // test failure (consumed != total gets asserted below).
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(2);

  std::vector<std::thread> threads;
  threads.reserve(cfg.producers + cfg.consumers);

  for (unsigned p = 0; p < cfg.producers; ++p) {
    threads.emplace_back([&, p] {
      for (std::size_t seq = 0; seq < cfg.per_producer; ++seq) {
        queue.enqueue(encode(p, seq));
      }
    });
  }

  for (unsigned c = 0; c < cfg.consumers; ++c) {
    threads.emplace_back([&] {
      // Per-producer FIFO state, local to this consumer.
      std::vector<std::int64_t> last_seq(cfg.producers, -1);
      std::size_t since_deadline_check = 0;
      for (;;) {
        auto v = queue.try_dequeue();
        if (!v) {
          if (consumed.load() >= total) break;
          if (++since_deadline_check % 1024 == 0 &&
              std::chrono::steady_clock::now() > deadline) {
            deadline_hit.store(true);
            break;
          }
          std::this_thread::yield();
          continue;
        }
        auto producer = static_cast<unsigned>(*v >> 32);
        auto seq = static_cast<std::int64_t>(*v & 0xffffffffu);

        CHECK_MSG(producer < cfg.producers && seq >= 0 &&
                      static_cast<std::size_t>(seq) < cfg.per_producer,
                  "corrupt value: producer=%u seq=%lld", producer,
                  static_cast<long long>(seq));
        CHECK_MSG(seq > last_seq[producer],
                  "per-producer FIFO violated: producer=%u seq=%lld after %lld",
                  producer, static_cast<long long>(seq),
                  static_cast<long long>(last_seq[producer]));
        last_seq[producer] = seq;

        std::uint8_t prev =
            seen[producer * cfg.per_producer + seq].fetch_add(1);
        CHECK_MSG(prev == 0, "duplicate dequeue: producer=%u seq=%lld",
                  producer, static_cast<long long>(seq));

        consumed.fetch_add(1);
      }
    });
  }

  for (auto& t : threads) t.join();

  CHECK_MSG(!deadline_hit.load(), "watchdog deadline hit — values lost?");
  CHECK_MSG(consumed.load() == total, "consumed %zu of %zu", consumed.load(),
            total);
  std::size_t missing = 0;
  for (auto& s : seen) {
    if (s.load() != 1) ++missing;
  }
  CHECK_MSG(missing == 0, "%zu value(s) not dequeued exactly once", missing);
  CHECK_MSG(!queue.try_dequeue().has_value(), "queue not empty after drain");
  CHECK_MSG(queue.empty(), "empty() false after drain");
}

}  // namespace

int main() {
  std::size_t per_producer = 200000;
  if (const char* env = std::getenv("LFQ_STRESS_PER_PRODUCER")) {
    per_producer = std::strtoull(env, nullptr, 10);
    if (per_producer == 0) per_producer = 1000;
  }

  const unsigned hw = std::max(2u, std::thread::hardware_concurrency());
  // At hardware concurrency, and at ~2x oversubscription — forced
  // preemption inside the CAS windows is where the bugs live.
  const config configs[] = {
      {hw / 2, hw / 2, per_producer},
      {hw, hw, per_producer / 2},
#ifdef LFQ_SCHEDULE_FUZZ
      // 4x oversubscription, adversarial builds only: combined with the
      // injected yields, the scheduler now preempts mid-window as the
      // common case rather than the millions-to-one case.
      {2 * hw, 2 * hw, per_producer / 8},
#endif
  };

  for (const auto& cfg : configs) {
    run_stress<lfq::mutex_queue<std::uint64_t>>("mutex", cfg);
    {
      // The leaky variant leaks by design; see leak_check.hpp.
      [[maybe_unused]] lfq::test::scoped_lsan_disable expected_leak;
      run_stress<lfq::ms_queue<std::uint64_t>>("ms", cfg);
    }
    run_stress<lfq::ms_queue<std::uint64_t, lfq::hp_reclaimer>>("ms-hp", cfg);
  }

  if (failures == 0) {
    std::printf("stress_test: all invariants held\n");
    return 0;
  }
  std::printf("stress_test: %d failure(s)\n", failures);
  return 1;
}
