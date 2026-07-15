#pragma once

#include <mutex>
#include <optional>
#include <queue>
#include <utility>

namespace lfq {

// The control group: std::queue under a std::mutex.
//
// Every number the lock-free queue produces is measured against this.
// Uncontended mutexes are fast — if the MS queue does not beat this at
// low thread counts, that is a finding, not a bug in the benchmark.
template <class T>
class mutex_queue {
 public:
  void enqueue(T value) {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.push(std::move(value));
  }

  std::optional<T> try_dequeue() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (queue_.empty()) return std::nullopt;
    std::optional<T> value(std::move(queue_.front()));
    queue_.pop();
    return value;
  }

  bool empty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.empty();
  }

 private:
  mutable std::mutex mutex_;
  std::queue<T> queue_;
};

}  // namespace lfq
