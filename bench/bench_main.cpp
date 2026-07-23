// Sweep driver. Emits one CSV row per repetition on stdout and a
// median/IQR summary per configuration on stderr, e.g.:
//
//   ./bench_main --queue=ms-hp --workload=pairs --threads=1,2,4,8,16
//   ./bench_main --queue=ms-hp --workload=pc --ratio=3:1 --threads=8,16
//
// Workloads (phase 3b):
//
//   pairs  Every thread alternates enqueue / dequeue-until-success, so
//          the queue stays near-empty — maximum head/tail contention.
//   pc     P producers : C consumers, split from the thread count by
//          --ratio (proportionally, each side at least 1). N:1 is where
//          the tail-lagging/helping path actually gets exercised; 1:N
//          lives on the empty-queue path.
//   enq    Enqueue only — isolates the tail path and the allocator.
//   deq    Dequeue only from a queue prefilled before timing starts —
//          isolates the head path.
//
// Thread counts deliberately sweep past hardware concurrency: a
// preempted lock holder stalls everyone, a preempted CAS loser stalls
// no one, and oversubscription is the regime where that distinction is
// visible.
//
// Dequeue latency deliberately includes empty-retry spins: "time until I
// hold an item" is the number a consumer experiences.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "concurrentqueue.h"
#include "harness.hpp"
#include "lfq/hazard_pointer.hpp"
#include "lfq/ms_queue.hpp"
#include "lfq/mutex_queue.hpp"

namespace {

// moodycamel speaks a different dialect (out-param try_dequeue); the
// adapter narrows it to the same API surface the other queues expose so
// the trial code cannot accidentally favor anyone.
struct moodycamel_queue {
  moodycamel::ConcurrentQueue<std::uint64_t> q;
  void enqueue(std::uint64_t v) { q.enqueue(v); }
  std::optional<std::uint64_t> try_dequeue() {
    std::uint64_t v;
    if (q.try_dequeue(v)) return v;
    return std::nullopt;
  }
};

struct options {
  std::string queue = "mutex";
  std::string workload = "pairs";
  unsigned ratio_p = 1;  // pc workload: producer share
  unsigned ratio_c = 1;  // pc workload: consumer share
  std::vector<unsigned> threads = {1, 2, 4, 8};
  std::size_t ops_per_thread = 100000;
  unsigned reps = 10;
  unsigned warmup = 2;
};

struct trial_result {
  double seconds = 0.0;
  bool pinned = false;
  std::size_t ops_total = 0;
  std::vector<std::uint64_t> enq_ns;  // merged + sorted
  std::vector<std::uint64_t> deq_ns;
};

struct worker_out {
  std::vector<std::uint64_t> enq_ns;
  std::vector<std::uint64_t> deq_ns;
};

// Shared trial skeleton: spawn workers, line them up on the barrier,
// time from release to last join, merge per-thread samples.
template <class SpawnBody>
trial_result run_threads(unsigned nthreads, SpawnBody&& body) {
  lfq::bench::spin_barrier barrier;
  std::atomic<bool> all_pinned{true};
  std::vector<worker_out> out(nthreads);

  std::vector<std::thread> workers;
  workers.reserve(nthreads);
  for (unsigned t = 0; t < nthreads; ++t) {
    workers.emplace_back([&, t] {
      if (!lfq::bench::pin_to_core(t)) {
        all_pinned.store(false, std::memory_order_relaxed);
      }
      barrier.worker_arrive_and_wait();
      body(t, out[t]);
    });
  }

  barrier.driver_wait_for(nthreads);
  std::uint64_t start = lfq::bench::now_ns();
  barrier.driver_release();
  for (auto& w : workers) w.join();
  std::uint64_t stop = lfq::bench::now_ns();

  trial_result r;
  r.seconds = static_cast<double>(stop - start) * 1e-9;
  r.pinned = all_pinned.load();
  for (auto& w : out) {
    r.enq_ns.insert(r.enq_ns.end(), w.enq_ns.begin(), w.enq_ns.end());
    r.deq_ns.insert(r.deq_ns.end(), w.deq_ns.begin(), w.deq_ns.end());
  }
  std::sort(r.enq_ns.begin(), r.enq_ns.end());
  std::sort(r.deq_ns.begin(), r.deq_ns.end());
  return r;
}

template <class Q>
trial_result run_pairs_trial(unsigned nthreads, std::size_t ops) {
  Q queue;
  trial_result r = run_threads(nthreads, [&](unsigned t, worker_out& out) {
    out.enq_ns.reserve(ops);  // preallocate: never allocate mid-measurement
    out.deq_ns.reserve(ops);
    for (std::size_t i = 0; i < ops; ++i) {
      std::uint64_t value =
          (static_cast<std::uint64_t>(t) << 32) | static_cast<std::uint64_t>(i);
      std::uint64_t t0 = lfq::bench::now_ns();
      queue.enqueue(value);
      std::uint64_t t1 = lfq::bench::now_ns();
      out.enq_ns.push_back(t1 - t0);

      std::uint64_t got = 0;
      for (;;) {
        auto v = queue.try_dequeue();
        if (v) {
          got = *v;
          break;
        }
      }
      std::uint64_t t2 = lfq::bench::now_ns();
      out.deq_ns.push_back(t2 - t1);
      lfq::bench::do_not_optimize(got);
    }
  });
  r.ops_total = nthreads * ops * 2;  // one enqueue + one dequeue each
  return r;
}

template <class Q>
trial_result run_pc_trial(unsigned producers, unsigned consumers,
                          std::size_t ops_per_producer) {
  Q queue;
  const std::size_t total = producers * ops_per_producer;
  std::atomic<std::size_t> consumed{0};

  trial_result r = run_threads(
      producers + consumers, [&](unsigned t, worker_out& out) {
        if (t < producers) {
          out.enq_ns.reserve(ops_per_producer);
          for (std::size_t i = 0; i < ops_per_producer; ++i) {
            std::uint64_t value = (static_cast<std::uint64_t>(t) << 32) |
                                  static_cast<std::uint64_t>(i);
            std::uint64_t t0 = lfq::bench::now_ns();
            queue.enqueue(value);
            std::uint64_t t1 = lfq::bench::now_ns();
            out.enq_ns.push_back(t1 - t0);
          }
        } else {
          out.deq_ns.reserve(total / consumers + 1);
          for (;;) {
            std::uint64_t t0 = lfq::bench::now_ns();
            std::uint64_t got = 0;
            bool have = false;
            // Retry until an item lands or the run is over; only a
            // successful grab is a latency sample.
            while (consumed.load(std::memory_order_relaxed) < total) {
              auto v = queue.try_dequeue();
              if (v) {
                got = *v;
                have = true;
                consumed.fetch_add(1, std::memory_order_relaxed);
                break;
              }
            }
            if (!have) break;
            std::uint64_t t1 = lfq::bench::now_ns();
            out.deq_ns.push_back(t1 - t0);
            lfq::bench::do_not_optimize(got);
          }
        }
      });
  r.ops_total = total * 2;
  return r;
}

template <class Q>
trial_result run_enq_trial(unsigned nthreads, std::size_t ops) {
  Q queue;
  trial_result r = run_threads(nthreads, [&](unsigned t, worker_out& out) {
    out.enq_ns.reserve(ops);
    for (std::size_t i = 0; i < ops; ++i) {
      std::uint64_t value =
          (static_cast<std::uint64_t>(t) << 32) | static_cast<std::uint64_t>(i);
      std::uint64_t t0 = lfq::bench::now_ns();
      queue.enqueue(value);
      std::uint64_t t1 = lfq::bench::now_ns();
      out.enq_ns.push_back(t1 - t0);
    }
  });
  r.ops_total = nthreads * ops;
  return r;
}

template <class Q>
trial_result run_deq_trial(unsigned nthreads, std::size_t ops) {
  Q queue;
  const std::size_t total = nthreads * ops;
  // Prefill before the barrier: filling is setup, not measurement.
  for (std::size_t i = 0; i < total; ++i) {
    queue.enqueue(static_cast<std::uint64_t>(i));
  }
  std::atomic<std::size_t> consumed{0};
  trial_result r = run_threads(nthreads, [&](unsigned, worker_out& out) {
    out.deq_ns.reserve(ops * 2);
    for (;;) {
      std::uint64_t t0 = lfq::bench::now_ns();
      std::uint64_t got = 0;
      bool have = false;
      while (consumed.load(std::memory_order_relaxed) < total) {
        auto v = queue.try_dequeue();
        if (v) {
          got = *v;
          have = true;
          consumed.fetch_add(1, std::memory_order_relaxed);
          break;
        }
      }
      if (!have) break;
      std::uint64_t t1 = lfq::bench::now_ns();
      out.deq_ns.push_back(t1 - t0);
      lfq::bench::do_not_optimize(got);
    }
  });
  r.ops_total = total;
  return r;
}

// Proportional ratio split with each side guaranteed a thread. The
// actual split is embedded in the CSV workload label (e.g. pc_2p6c) —
// the ratio alone would hide the rounding.
void split_ratio(const options& opt, unsigned nthreads, unsigned& producers,
                 unsigned& consumers) {
  const double share = static_cast<double>(opt.ratio_p) /
                       static_cast<double>(opt.ratio_p + opt.ratio_c);
  auto p = static_cast<unsigned>(
      share * static_cast<double>(nthreads) + 0.5);
  if (p < 1) p = 1;
  if (p > nthreads - 1) p = nthreads - 1;
  producers = p;
  consumers = nthreads - p;
}

template <class Q>
void run_suite(const options& opt) {
  std::printf(
      "queue,workload,threads,rep,ops_total,seconds,throughput_ops_s,pinned,"
      "enq_p50_ns,enq_p99_ns,enq_p999_ns,deq_p50_ns,deq_p99_ns,deq_p999_ns\n");

  for (unsigned nthreads : opt.threads) {
    char workload_label[32];
    if (opt.workload == "pc") {
      if (nthreads < 2) {
        std::fprintf(stderr,
                     "# skipping threads=%u: pc needs at least 1P + 1C\n",
                     nthreads);
        continue;
      }
      unsigned p = 0, c = 0;
      split_ratio(opt, nthreads, p, c);
      std::snprintf(workload_label, sizeof workload_label, "pc_%up%uc", p, c);
    } else {
      std::snprintf(workload_label, sizeof workload_label, "%s",
                    opt.workload.c_str());
    }

    std::vector<double> throughputs;
    for (unsigned rep = 0; rep < opt.warmup + opt.reps; ++rep) {
      trial_result r;
      if (opt.workload == "pairs") {
        r = run_pairs_trial<Q>(nthreads, opt.ops_per_thread);
      } else if (opt.workload == "pc") {
        unsigned p = 0, c = 0;
        split_ratio(opt, nthreads, p, c);
        r = run_pc_trial<Q>(p, c, opt.ops_per_thread);
      } else if (opt.workload == "enq") {
        r = run_enq_trial<Q>(nthreads, opt.ops_per_thread);
      } else {
        r = run_deq_trial<Q>(nthreads, opt.ops_per_thread);
      }
      if (rep < opt.warmup) continue;  // warmup reps are discarded

      double throughput = static_cast<double>(r.ops_total) / r.seconds;
      throughputs.push_back(throughput);

      using lfq::bench::percentile_sorted;
      std::printf(
          "%s,%s,%u,%u,%zu,%.6f,%.0f,%d,%llu,%llu,%llu,%llu,%llu,%llu\n",
          opt.queue.c_str(), workload_label, nthreads, rep - opt.warmup,
          r.ops_total, r.seconds, throughput, r.pinned ? 1 : 0,
          (unsigned long long)percentile_sorted(r.enq_ns, 0.50),
          (unsigned long long)percentile_sorted(r.enq_ns, 0.99),
          (unsigned long long)percentile_sorted(r.enq_ns, 0.999),
          (unsigned long long)percentile_sorted(r.deq_ns, 0.50),
          (unsigned long long)percentile_sorted(r.deq_ns, 0.99),
          (unsigned long long)percentile_sorted(r.deq_ns, 0.999));
      std::fflush(stdout);
    }
    auto s = lfq::bench::summarize(throughputs);
    std::fprintf(stderr,
                 "# %s %s threads=%u  throughput median=%.2fM ops/s  "
                 "IQR=[%.2fM, %.2fM]\n",
                 opt.queue.c_str(), workload_label, nthreads, s.median * 1e-6,
                 s.q1 * 1e-6, s.q3 * 1e-6);
  }
}

std::vector<unsigned> parse_thread_list(const char* s) {
  std::vector<unsigned> out;
  for (const char* p = s; *p != '\0';) {
    char* end = nullptr;
    unsigned long v = std::strtoul(p, &end, 10);
    if (end == p || v == 0) break;
    out.push_back(static_cast<unsigned>(v));
    p = (*end == ',') ? end + 1 : end;
  }
  return out;
}

options parse_args(int argc, char** argv) {
  options opt;
  for (int i = 1; i < argc; ++i) {
    const char* a = argv[i];
    auto value_of = [&](const char* key) -> const char* {
      std::size_t n = std::strlen(key);
      return std::strncmp(a, key, n) == 0 ? a + n : nullptr;
    };
    if (const char* v = value_of("--queue=")) {
      opt.queue = v;
    } else if (const char* v = value_of("--workload=")) {
      opt.workload = v;
    } else if (const char* v = value_of("--ratio=")) {
      char* end = nullptr;
      opt.ratio_p = static_cast<unsigned>(std::strtoul(v, &end, 10));
      opt.ratio_c = (end != nullptr && *end == ':')
                        ? static_cast<unsigned>(std::strtoul(end + 1, nullptr, 10))
                        : 0;
    } else if (const char* v = value_of("--threads=")) {
      opt.threads = parse_thread_list(v);
    } else if (const char* v = value_of("--ops=")) {
      opt.ops_per_thread = std::strtoull(v, nullptr, 10);
    } else if (const char* v = value_of("--reps=")) {
      opt.reps = static_cast<unsigned>(std::strtoul(v, nullptr, 10));
    } else if (const char* v = value_of("--warmup=")) {
      opt.warmup = static_cast<unsigned>(std::strtoul(v, nullptr, 10));
    } else {
      std::fprintf(stderr, "unknown argument: %s\n", a);
      std::exit(2);
    }
  }
  const bool workload_known = opt.workload == "pairs" || opt.workload == "pc" ||
                              opt.workload == "enq" || opt.workload == "deq";
  if (opt.threads.empty() || opt.ops_per_thread == 0 || opt.reps == 0 ||
      !workload_known || opt.ratio_p == 0 || opt.ratio_c == 0) {
    std::fprintf(stderr, "invalid options\n");
    std::exit(2);
  }
  return opt;
}

}  // namespace

int main(int argc, char** argv) {
  options opt = parse_args(argc, argv);
  if (opt.queue == "mutex") {
    run_suite<lfq::mutex_queue<std::uint64_t>>(opt);
  } else if (opt.queue == "ms") {
    // Phase 1: leaky reclaimer — memory grows for the duration of a trial.
    // Kept as the baseline that prices what safe reclamation costs.
    run_suite<lfq::ms_queue<std::uint64_t>>(opt);
  } else if (opt.queue == "ms-hp") {
    // Phase 2: hazard-pointer reclamation. Same queue, safe memory.
    run_suite<lfq::ms_queue<std::uint64_t, lfq::hp_reclaimer>>(opt);
  } else if (opt.queue == "ms-unpadded") {
    // False-sharing experiment: head_ and tail_ adjacent, same line.
    run_suite<lfq::ms_queue<std::uint64_t, lfq::leaky_reclaimer, false>>(opt);
  } else if (opt.queue == "ms-hp-unpadded") {
    run_suite<lfq::ms_queue<std::uint64_t, lfq::hp_reclaimer, false>>(opt);
  } else if (opt.queue == "moodycamel") {
    // The honest comparison: a production block-based design. Losing to
    // it is expected; diagnosing why is the point.
    run_suite<moodycamel_queue>(opt);
  } else {
    std::fprintf(stderr,
                 "unknown queue: %s (available: mutex, ms, ms-hp, "
                 "ms-unpadded, ms-hp-unpadded, moodycamel)\n",
                 opt.queue.c_str());
    return 2;
  }
  return 0;
}
