// Sweep driver. Emits one CSV row per repetition on stdout and a
// median/IQR summary per configuration on stderr, e.g.:
//
//   ./bench_main --queue=mutex --threads=1,2,4,8 --ops=100000 --reps=10
//
// Workload (phase 0): symmetric pairs. Every thread alternates
// enqueue / dequeue-until-success, so producers == consumers == N and the
// queue stays near-empty (maximum head/tail contention). Ratio workloads
// (1:N, N:1) arrive with the phase-3 methodology work.
//
// Dequeue latency deliberately includes empty-retry spins: "time until I
// hold an item" is the number a consumer experiences.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "harness.hpp"
#include "lfq/hazard_pointer.hpp"
#include "lfq/ms_queue.hpp"
#include "lfq/mutex_queue.hpp"

namespace {

struct options {
  std::string queue = "mutex";
  std::vector<unsigned> threads = {1, 2, 4, 8};
  std::size_t ops_per_thread = 100000;
  unsigned reps = 10;
  unsigned warmup = 2;
};

struct trial_result {
  double seconds = 0.0;
  bool pinned = false;
  std::vector<std::uint64_t> enq_ns;  // merged + sorted
  std::vector<std::uint64_t> deq_ns;
};

template <class Q>
trial_result run_trial(unsigned nthreads, std::size_t ops) {
  Q queue;
  lfq::bench::spin_barrier barrier;
  std::atomic<bool> all_pinned{true};

  struct worker_out {
    std::vector<std::uint64_t> enq_ns;
    std::vector<std::uint64_t> deq_ns;
  };
  std::vector<worker_out> out(nthreads);

  std::vector<std::thread> workers;
  workers.reserve(nthreads);
  for (unsigned t = 0; t < nthreads; ++t) {
    workers.emplace_back([&, t] {
      if (!lfq::bench::pin_to_core(t)) {
        all_pinned.store(false, std::memory_order_relaxed);
      }
      std::vector<std::uint64_t> enq_ns;
      std::vector<std::uint64_t> deq_ns;
      enq_ns.reserve(ops);  // preallocate: never allocate mid-measurement
      deq_ns.reserve(ops);

      barrier.worker_arrive_and_wait();
      for (std::size_t i = 0; i < ops; ++i) {
        std::uint64_t value =
            (static_cast<std::uint64_t>(t) << 32) | static_cast<std::uint64_t>(i);

        std::uint64_t t0 = lfq::bench::now_ns();
        queue.enqueue(value);
        std::uint64_t t1 = lfq::bench::now_ns();
        enq_ns.push_back(t1 - t0);

        std::uint64_t got = 0;
        for (;;) {
          auto r = queue.try_dequeue();
          if (r) {
            got = *r;
            break;
          }
        }
        std::uint64_t t2 = lfq::bench::now_ns();
        deq_ns.push_back(t2 - t1);
        lfq::bench::do_not_optimize(got);
      }
      out[t].enq_ns = std::move(enq_ns);
      out[t].deq_ns = std::move(deq_ns);
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
void run_suite(const options& opt) {
  std::printf(
      "queue,workload,threads,rep,ops_total,seconds,throughput_ops_s,pinned,"
      "enq_p50_ns,enq_p99_ns,enq_p999_ns,deq_p50_ns,deq_p99_ns,deq_p999_ns\n");

  for (unsigned nthreads : opt.threads) {
    std::vector<double> throughputs;
    for (unsigned rep = 0; rep < opt.warmup + opt.reps; ++rep) {
      trial_result r = run_trial<Q>(nthreads, opt.ops_per_thread);
      if (rep < opt.warmup) continue;  // warmup reps are discarded

      // One enqueue + one dequeue both count as ops.
      auto ops_total = static_cast<double>(opt.ops_per_thread) * nthreads * 2.0;
      double throughput = ops_total / r.seconds;
      throughputs.push_back(throughput);

      using lfq::bench::percentile_sorted;
      std::printf(
          "%s,pairs,%u,%u,%.0f,%.6f,%.0f,%d,%llu,%llu,%llu,%llu,%llu,%llu\n",
          opt.queue.c_str(), nthreads, rep - opt.warmup, ops_total, r.seconds,
          throughput, r.pinned ? 1 : 0,
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
                 "# %s threads=%u  throughput median=%.2fM ops/s  "
                 "IQR=[%.2fM, %.2fM]\n",
                 opt.queue.c_str(), nthreads, s.median * 1e-6, s.q1 * 1e-6,
                 s.q3 * 1e-6);
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
  if (opt.threads.empty() || opt.ops_per_thread == 0 || opt.reps == 0) {
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
  } else {
    std::fprintf(stderr, "unknown queue: %s (available: mutex, ms, ms-hp)\n",
                 opt.queue.c_str());
    return 2;
  }
  return 0;
}
