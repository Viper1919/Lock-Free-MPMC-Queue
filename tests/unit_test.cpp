// Single-threaded semantics: empty, one, many, FIFO order, interleaving.
//
// Deliberately framework-free — a dependency-less repo builds anywhere CI
// runs, and these checks don't need fixtures.

#include <cstdio>
#include <string>

#include "lfq/ms_queue.hpp"
#include "lfq/mutex_queue.hpp"

static int failures = 0;

#define CHECK(cond)                                              \
  do {                                                           \
    if (!(cond)) {                                               \
      ++failures;                                                \
      std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
    }                                                            \
  } while (0)

template <class Q>
static void test_int_semantics(const char* name) {
  std::printf("[ int      ] %s\n", name);
  Q q;

  CHECK(q.empty());
  CHECK(!q.try_dequeue().has_value());
  CHECK(!q.try_dequeue().has_value());  // empty twice: no state corruption

  q.enqueue(42);
  CHECK(!q.empty());
  auto v = q.try_dequeue();
  CHECK(v.has_value() && *v == 42);
  CHECK(q.empty());
  CHECK(!q.try_dequeue().has_value());  // back to empty after drain

  // FIFO over many elements.
  constexpr int kMany = 10000;
  for (int i = 0; i < kMany; ++i) q.enqueue(i);
  for (int i = 0; i < kMany; ++i) {
    auto r = q.try_dequeue();
    CHECK(r.has_value() && *r == i);
  }
  CHECK(q.empty());

  // Interleaved enqueue/dequeue keeps order.
  q.enqueue(1);
  q.enqueue(2);
  CHECK(*q.try_dequeue() == 1);
  q.enqueue(3);
  CHECK(*q.try_dequeue() == 2);
  CHECK(*q.try_dequeue() == 3);
  CHECK(q.empty());
}

template <class Q>
static void test_string_semantics(const char* name) {
  std::printf("[ string   ] %s\n", name);
  Q q;
  q.enqueue(std::string("first"));
  q.enqueue(std::string("second, long enough to defeat SSO and touch the heap"));
  auto a = q.try_dequeue();
  auto b = q.try_dequeue();
  CHECK(a.has_value() && *a == "first");
  CHECK(b.has_value() &&
        *b == "second, long enough to defeat SSO and touch the heap");
  CHECK(!q.try_dequeue().has_value());
}

int main() {
  test_int_semantics<lfq::mutex_queue<int>>("mutex_queue");
  test_string_semantics<lfq::mutex_queue<std::string>>("mutex_queue");
  test_int_semantics<lfq::ms_queue<int>>("ms_queue");
  test_string_semantics<lfq::ms_queue<std::string>>("ms_queue");

  if (failures == 0) {
    std::printf("unit_test: all checks passed\n");
    return 0;
  }
  std::printf("unit_test: %d check(s) FAILED\n", failures);
  return 1;
}
