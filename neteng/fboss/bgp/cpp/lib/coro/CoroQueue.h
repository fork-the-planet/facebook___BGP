/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <memory>
#include <optional>
#include <queue>
#include <utility>

#include <folly/coro/Baton.h>
#include <folly/coro/Mutex.h>
#include <folly/coro/Task.h>

#include <folly/Function.h>

namespace facebook {
namespace nettools {
namespace bgplib {

template <typename ElemT>
class CoroQueue {
 public:
  struct QueueClosed {};

  template <typename T>
  folly::coro::Task<folly::Expected<size_t, QueueClosed>> push(
      T&& data) noexcept {
    std::optional<std::reference_wrapper<folly::coro::Baton>> baton;
    size_t size;
    {
      std::unique_lock<folly::coro::Mutex> lock{
          co_await mutex_.co_scoped_lock()};
      if (closed_) {
        co_return folly::makeUnexpected(QueueClosed{});
      }
      queue_.push(std::forward<T>(data));
      if (!waitList_.empty()) {
        baton = waitList_.front();
        waitList_.pop();
      }
      size = queue_.size();
    }
    if (baton.has_value()) {
      baton->get().post();
    }
    co_return size;
  }

  folly::coro::Task<bool> empty() noexcept {
    std::unique_lock<folly::coro::Mutex> lock{co_await mutex_.co_scoped_lock()};
    co_return queue_.empty();
  }

  folly::coro::Task<bool> closed() const noexcept {
    std::unique_lock<folly::coro::Mutex> lock{co_await mutex_.co_scoped_lock()};
    co_return closed_;
  }

  folly::coro::Task<size_t> size() const noexcept {
    std::unique_lock<folly::coro::Mutex> lock{co_await mutex_.co_scoped_lock()};
    co_return queue_.size();
  }

  folly::coro::Task<folly::Expected<std::optional<ElemT>, QueueClosed>>
  try_pop() noexcept(std::is_nothrow_move_constructible<ElemT>::value) {
    std::unique_lock<folly::coro::Mutex> lock{co_await mutex_.co_scoped_lock()};
    if (closed_ && queue_.empty()) {
      co_return folly::makeUnexpected(QueueClosed{});
    }
    if (queue_.empty()) {
      co_return std::nullopt;
    }
    auto front = std::move(queue_.front());
    queue_.pop();
    co_return front;
  }

  folly::coro::Task<folly::Expected<ElemT, QueueClosed>>
  wait_and_pop() noexcept(std::is_nothrow_move_constructible<ElemT>::value) {
    co_await mutex_.co_lock();
    // spin & sleep, waiting until queue is not empty
    while (queue_.empty() && !closed_) {
      folly::coro::Baton waitBaton;
      waitList_.emplace(waitBaton);
      mutex_.unlock();
      co_await waitBaton;
      // the push() code will pop us from baton queue
      co_await mutex_.co_lock();
    }
    if (closed_ && queue_.empty()) {
      mutex_.unlock();
      co_return folly::makeUnexpected(QueueClosed{});
    }
    // we hold the mutex_ locked here, and queue_ is not empty()
    auto front = std::move(queue_.front());
    queue_.pop();
    mutex_.unlock();
    co_return front;
  }

  folly::coro::Task<folly::Expected<std::queue<ElemT>, QueueClosed>>
  wait_and_pop_all() noexcept(
      std::is_nothrow_move_constructible<ElemT>::value) {
    co_await mutex_.co_lock();
    // spin & sleep, waiting until queue is not empty
    while (queue_.empty() && !closed_) {
      folly::coro::Baton waitBaton;
      waitList_.emplace(waitBaton);
      mutex_.unlock();
      co_await waitBaton;
      // the push() code will pop us from baton queue
      co_await mutex_.co_lock();
    }
    if (closed_ && queue_.empty()) {
      mutex_.unlock();
      co_return folly::makeUnexpected(QueueClosed{});
    }
    // we hold the mutex_ locked here, and queue_ is not empty()
    std::queue<ElemT> elems;
    queue_.swap(elems);
    mutex_.unlock();
    co_return elems;
  }

  // closing still means some data may remain in queue
  folly::coro::Task<void> close() {
    std::vector<std::reference_wrapper<folly::coro::Baton>> waiters;
    {
      std::unique_lock<folly::coro::Mutex> lock{
          co_await mutex_.co_scoped_lock()};
      closed_ = true;
      while (!waitList_.empty()) {
        waiters.push_back(waitList_.front());
        waitList_.pop();
      }
    }
    for (auto& waiter : waiters) {
      waiter.get().post();
    }
  }

  // used for unittesting to check queue size, etc
  std::queue<ElemT> const& getRawQueueUnsafe() {
    return queue_;
  }

 private:
  // the mutex protects all internal data
  mutable folly::coro::Mutex mutex_;
  bool closed_{false};
  std::queue<ElemT> queue_;
  // waiters deposit batons here to be notified
  std::queue<std::reference_wrapper<folly::coro::Baton>> waitList_;
};

} // namespace bgplib
} // namespace nettools
} // namespace facebook
