/*
 * Copyright (c) Facebook, Inc. and its affiliates.
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

#include <folly/Try.h>
#include <folly/executors/ManualExecutor.h>
#include <folly/experimental/coro/Task.h>
#include <folly/experimental/coro/Traits.h>
#include <folly/experimental/coro/ViaIfAsync.h>
#include <folly/experimental/coro/detail/Malloc.h>
#include <folly/experimental/coro/detail/Traits.h>
#include <folly/fibers/Baton.h>
#include <folly/synchronization/Baton.h>

#include <cassert>
#include <exception>
#include <experimental/coroutine>
#include <type_traits>
#include <utility>

namespace folly {
namespace coro {

namespace detail {

template <typename T>
class BlockingWaitTask;

class BlockingWaitPromiseBase {
  struct FinalAwaiter {
    bool await_ready() noexcept {
      return false;
    }
    template <typename Promise>
    void await_suspend(
        std::experimental::coroutine_handle<Promise> coro) noexcept {
      BlockingWaitPromiseBase& promise = coro.promise();
      promise.baton_.post();
    }
    void await_resume() noexcept {}
  };

 public:
  BlockingWaitPromiseBase() noexcept = default;

  static void* operator new(std::size_t size) {
    return ::folly_coro_async_malloc(size);
  }

  static void operator delete(void* ptr, std::size_t size) {
    ::folly_coro_async_free(ptr, size);
  }

  std::experimental::suspend_always initial_suspend() {
    return {};
  }

  FinalAwaiter final_suspend() noexcept {
    return {};
  }

  bool done() const noexcept {
    return baton_.ready();
  }

  void wait() noexcept {
    baton_.wait();
  }

 private:
  folly::fibers::Baton baton_;
};

template <typename T>
class BlockingWaitPromise final : public BlockingWaitPromiseBase {
 public:
  BlockingWaitPromise() noexcept = default;

  ~BlockingWaitPromise() = default;

  BlockingWaitTask<T> get_return_object() noexcept;

  void unhandled_exception() noexcept {
    result_->emplaceException(
        folly::exception_wrapper::from_exception_ptr(std::current_exception()));
  }

  template <
      typename U,
      std::enable_if_t<std::is_convertible<U, T>::value, int> = 0>
  void return_value(U&& value) noexcept(
      std::is_nothrow_constructible<T, U&&>::value) {
    result_->emplace(static_cast<U&&>(value));
  }

  void setTry(folly::Try<T>* result) noexcept {
    result_ = &result;
  }

 private:
  folly::Try<T>* result_;
};

template <typename T>
class BlockingWaitPromise<T&> final : public BlockingWaitPromiseBase {
 public:
  BlockingWaitPromise() noexcept = default;

  ~BlockingWaitPromise() = default;

  BlockingWaitTask<T&> get_return_object() noexcept;

  void unhandled_exception() noexcept {
    result_->emplaceException(
        folly::exception_wrapper::from_exception_ptr(std::current_exception()));
  }

  auto yield_value(T&& value) noexcept {
    result_->emplace(std::ref(value));
    return final_suspend();
  }

  auto yield_value(T& value) noexcept {
    result_->emplace(std::ref(value));
    return final_suspend();
  }

#if 0
  void return_value(T& value) noexcept {
    result_->emplace(std::ref(value));
  }
#endif

  void return_void() {
    // This should never be reachable.
    // The coroutine should either have suspended at co_yield or should have
    // thrown an exception and skipped over the implicit co_return and
    // gone straight to unhandled_exception().
    std::abort();
  }

  void setTry(folly::Try<std::reference_wrapper<T>>* result) noexcept {
    result_ = result;
  }

 private:
  folly::Try<std::reference_wrapper<T>>* result_;
};

template <>
class BlockingWaitPromise<void> final : public BlockingWaitPromiseBase {
 public:
  BlockingWaitPromise() = default;

  BlockingWaitTask<void> get_return_object() noexcept;

  void return_void() noexcept {}

  void unhandled_exception() noexcept {
    result_->emplaceException(
        exception_wrapper::from_exception_ptr(std::current_exception()));
  }

  void setTry(folly::Try<void>* result) noexcept {
    result_ = result;
  }

 private:
  folly::Try<void>* result_;
};

template <typename T>
class BlockingWaitTask {
 public:
  using promise_type = BlockingWaitPromise<T>;
  using handle_t = std::experimental::coroutine_handle<promise_type>;

  explicit BlockingWaitTask(handle_t coro) noexcept : coro_(coro) {}

  BlockingWaitTask(BlockingWaitTask&& other) noexcept
      : coro_(std::exchange(other.coro_, {})) {}

  BlockingWaitTask& operator=(BlockingWaitTask&& other) noexcept = delete;

  ~BlockingWaitTask() {
    if (coro_) {
      coro_.destroy();
    }
  }

  folly::Try<detail::lift_lvalue_reference_t<T>> getAsTry() && {
    folly::Try<detail::lift_lvalue_reference_t<T>> result;
    auto& promise = coro_.promise();
    promise.setTry(&result);
    {
      RequestContextScopeGuard guard{RequestContext::saveContext()};
      coro_.resume();
    }
    promise.wait();
    return result;
  }

  T get() && {
    return std::move(*this).getAsTry().value();
  }

  T getVia(folly::DrivableExecutor* executor) && {
    folly::Try<detail::lift_lvalue_reference_t<T>> result;
    auto& promise = coro_.promise();
    promise.setTry(&result);
    executor->add(
        [coro = coro_, rctx = RequestContext::saveContext()]() mutable {
          RequestContextScopeGuard guard{std::move(rctx)};
          coro.resume();
        });
    while (!promise.done()) {
      executor->drive();
    }
    return std::move(result).value();
  }

 private:
  handle_t coro_;
};

template <typename T>
inline BlockingWaitTask<T>
BlockingWaitPromise<T>::get_return_object() noexcept {
  return BlockingWaitTask<T>{
      std::experimental::coroutine_handle<BlockingWaitPromise<T>>::from_promise(
          *this)};
}

template <typename T>
inline BlockingWaitTask<T&>
BlockingWaitPromise<T&>::get_return_object() noexcept {
  return BlockingWaitTask<T&>{std::experimental::coroutine_handle<
      BlockingWaitPromise<T&>>::from_promise(*this)};
}

inline BlockingWaitTask<void>
BlockingWaitPromise<void>::get_return_object() noexcept {
  return BlockingWaitTask<void>{std::experimental::coroutine_handle<
      BlockingWaitPromise<void>>::from_promise(*this)};
}

template <
    typename Awaitable,
    typename Result = await_result_t<Awaitable>,
    std::enable_if_t<!std::is_lvalue_reference<Result>::value, int> = 0>
auto makeBlockingWaitTask(Awaitable&& awaitable)
    -> BlockingWaitTask<detail::decay_rvalue_reference_t<Result>> {
  co_return co_await static_cast<Awaitable&&>(awaitable);
}

template <
    typename Awaitable,
    typename Result = await_result_t<Awaitable>,
    std::enable_if_t<std::is_lvalue_reference<Result>::value, int> = 0>
auto makeBlockingWaitTask(Awaitable&& awaitable)
    -> BlockingWaitTask<detail::decay_rvalue_reference_t<Result>> {
  co_yield co_await static_cast<Awaitable&&>(awaitable);
}

template <
    typename Awaitable,
    typename Result = await_result_t<Awaitable>,
    std::enable_if_t<std::is_void<Result>::value, int> = 0>
BlockingWaitTask<void> makeRefBlockingWaitTask(Awaitable&& awaitable) {
  co_await static_cast<Awaitable&&>(awaitable);
}

template <
    typename Awaitable,
    typename Result = await_result_t<Awaitable>,
    std::enable_if_t<!std::is_void<Result>::value, int> = 0>
auto makeRefBlockingWaitTask(Awaitable&& awaitable)
    -> BlockingWaitTask<std::add_lvalue_reference_t<Result>> {
  co_yield co_await static_cast<Awaitable&&>(awaitable);
}

class BlockingWaitExecutor final : public folly::DrivableExecutor {
 public:
  ~BlockingWaitExecutor() {
    while (keepAliveCount_.load() > 0) {
      drive();
    }
  }

  void add(Func func) override {
    auto wQueue = queue_.wlock();
    bool empty = wQueue->empty();
    wQueue->push_back(std::move(func));
    if (empty) {
      baton_.post();
    }
  }

  void drive() override {
    baton_.wait();
    baton_.reset();

    folly::fibers::runInMainContext([&]() {
      std::vector<Func> funcs;
      queue_.swap(funcs);
      for (auto& func : funcs) {
        std::exchange(func, nullptr)();
      }
    });
  }

 private:
  bool keepAliveAcquire() override {
    auto keepAliveCount =
        keepAliveCount_.fetch_add(1, std::memory_order_relaxed);
    DCHECK(keepAliveCount >= 0);
    return true;
  }

  void keepAliveRelease() override {
    auto keepAliveCount = keepAliveCount_.load(std::memory_order_relaxed);
    do {
      DCHECK(keepAliveCount > 0);
      if (keepAliveCount == 1) {
        add([this] {
          // the final count *must* be released from this executor or else if we
          // are mid-destructor we have a data race
          keepAliveCount_.fetch_sub(1, std::memory_order_relaxed);
        });
        return;
      }
    } while (!keepAliveCount_.compare_exchange_weak(
        keepAliveCount,
        keepAliveCount - 1,
        std::memory_order_release,
        std::memory_order_relaxed));
  }

  folly::Synchronized<std::vector<Func>> queue_;
  fibers::Baton baton_;

  std::atomic<ssize_t> keepAliveCount_{0};
};

} // namespace detail

/// blockingWait(Awaitable&&) -> await_result_t<Awaitable>
///
/// This function co_awaits the passed awaitable object and blocks the current
/// thread until the operation completes.
///
/// This is useful for launching an asynchronous operation from the top-level
/// main() function or from unit-tests.
///
/// WARNING:
/// Avoid using this function within any code that might run on the thread
/// of an executor as this can potentially lead to deadlock if the operation
/// you are waiting on needs to do some work on that executor in order to
/// complete.
template <typename Awaitable>
auto blockingWait(Awaitable&& awaitable)
    -> detail::decay_rvalue_reference_t<await_result_t<Awaitable>> {
  return static_cast<std::add_rvalue_reference_t<await_result_t<Awaitable>>>(
      detail::makeRefBlockingWaitTask(static_cast<Awaitable&&>(awaitable))
          .get());
}

template <typename SemiAwaitable>
auto blockingWait(SemiAwaitable&& awaitable, folly::DrivableExecutor* executor)
    -> detail::decay_rvalue_reference_t<semi_await_result_t<SemiAwaitable>> {
  return static_cast<
      std::add_rvalue_reference_t<semi_await_result_t<SemiAwaitable>>>(
      detail::makeRefBlockingWaitTask(
          folly::coro::co_viaIfAsync(
              folly::getKeepAliveToken(executor),
              static_cast<SemiAwaitable&&>(awaitable)))
          .getVia(executor));
}

template <
    typename SemiAwaitable,
    std::enable_if_t<!is_awaitable_v<SemiAwaitable>, int> = 0>
auto blockingWait(SemiAwaitable&& awaitable)
    -> detail::decay_rvalue_reference_t<semi_await_result_t<SemiAwaitable>> {
  std::exception_ptr eptr;
  {
    detail::BlockingWaitExecutor executor;
    try {
      return blockingWait(static_cast<SemiAwaitable&&>(awaitable), &executor);
    } catch (...) {
      eptr = std::current_exception();
    }
  }
  std::rethrow_exception(eptr);
}

} // namespace coro
} // namespace folly
