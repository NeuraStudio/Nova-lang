// NovaConcurrency.hpp — Nova Language Concurrency Engine
// Provides low-level atomics, high-level threading (ThreadPool, Mutex/RwLock),
// Actor Model Mailboxes, Go-style Channels (MPMC), and Async/Await runners.

#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <shared_mutex>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>

namespace nova::concurrency {

// ============================================================================
// Low-Level: Atomics & Memory Ordering
// ============================================================================

// Memory orderings matching the C++ memory model for lock-free programming
enum class MemoryOrder {
    Relaxed = static_cast<int>(std::memory_order_relaxed),
    Acquire = static_cast<int>(std::memory_order_acquire),
    Release = static_cast<int>(std::memory_order_release),
    AcqRel  = static_cast<int>(std::memory_order_acq_rel),
    SeqCst  = static_cast<int>(std::memory_order_seq_cst)
};

template <typename T>
class Atomic {
    static_assert(std::is_trivially_copyable_v<T>, "Atomic type must be trivially copyable.");
public:
    Atomic() noexcept = default;
    explicit Atomic(T desired) noexcept : atomic_val(desired) {}

    void store(T desired, MemoryOrder order = MemoryOrder::SeqCst) noexcept {
        atomic_val.store(desired, static_cast<std::memory_order>(order));
    }

    [[nodiscard]] T load(MemoryOrder order = MemoryOrder::SeqCst) const noexcept {
        return atomic_val.load(static_cast<std::memory_order>(order));
    }

    T exchange(T desired, MemoryOrder order = MemoryOrder::SeqCst) noexcept {
        return atomic_val.exchange(desired, static_cast<std::memory_order>(order));
    }

    bool compare_exchange_weak(T& expected, T desired,
                               MemoryOrder success = MemoryOrder::SeqCst,
                               MemoryOrder failure = MemoryOrder::SeqCst) noexcept {
        return atomic_val.compare_exchange_weak(expected, desired,
                                                static_cast<std::memory_order>(success),
                                                static_cast<std::memory_order>(failure));
    }

    bool compare_exchange_strong(T& expected, T desired,
                                 MemoryOrder success = MemoryOrder::SeqCst,
                                 MemoryOrder failure = MemoryOrder::SeqCst) noexcept {
        return atomic_val.compare_exchange_strong(expected, desired,
                                                  static_cast<std::memory_order>(success),
                                                  static_cast<std::memory_order>(failure));
    }

private:
    std::atomic<T> atomic_val;
};

// ============================================================================
// System-Level: Synchronization Primitives
// ============================================================================

class Mutex {
public:
    Mutex() = default;
    void lock() { mtx.lock(); }
    void unlock() { mtx.unlock(); }
    bool try_lock() { return mtx.try_lock(); }

private:
    std::mutex mtx;
};

class RwLock {
public:
    RwLock() = default;
    void lock_read() { rw_mtx.lock_shared(); }
    void unlock_read() { rw_mtx.unlock_shared(); }
    void lock_write() { rw_mtx.lock(); }
    void unlock_write() { rw_mtx.unlock(); }

private:
    std::shared_mutex rw_mtx;
};

// ============================================================================
// System-Level: Thread Pool
// ============================================================================

class ThreadPool {
public:
    explicit ThreadPool(size_t threads);
    ~ThreadPool();

    // Enqueue a task, returning a future for sync retrieval
    template<class F, class... Args>
    auto enqueue(F&& f, Args&&... args) -> std::future<std::invoke_result_t<F, Args...>>;

    void shutdown();

private:
    std::vector<std::thread> workers;
    std::queue<std::function<void()>> tasks;

    std::mutex queue_mutex;
    std::condition_variable condition;
    bool stop;
};

// ============================================================================
// System-Level: Actor Mailbox
// ============================================================================

// Lightweight unbounded message queue tailored for Actor Model processing
template <typename Message>
class ActorMailbox {
public:
    void push(Message msg) {
        {
            std::unique_lock<std::mutex> lock(mtx);
            queue.push(std::move(msg));
        }
        cv.notify_one();
    }

    Message pop() {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait(lock, [this] { return !queue.empty(); });
        Message msg = std::move(queue.front());
        queue.pop();
        return msg;
    }

    bool try_pop(Message& msg) {
        std::unique_lock<std::mutex> lock(mtx);
        if (queue.empty()) return false;
        msg = std::move(queue.front());
        queue.pop();
        return true;
    }

private:
    std::queue<Message> queue;
    std::mutex mtx;
    std::condition_variable cv;
};

// ============================================================================
// High-Level: MPMC Channel (Go-style chan<T>)
// ============================================================================

template <typename T>
class Channel {
public:
    explicit Channel(size_t capacity = 0) 
        : capacity_(capacity), closed_(false) {}

    // Blocks until space is available or channel is closed.
    // Returns true on success, false if closed.
    bool send(T value) {
        std::unique_lock<std::mutex> lock(mtx_);
        cv_push_.wait(lock, [this]() {
            return closed_ || (capacity_ > 0 ? queue_.size() < capacity_ : false);
        });

        if (closed_) return false;

        queue_.push(std::move(value));
        cv_pop_.notify_one();
        return true;
    }

    // Blocks until value is available or channel is closed.
    // Returns true on success, false if closed AND empty.
    bool receive(T& out_value) {
        std::unique_lock<std::mutex> lock(mtx_);
        cv_pop_.wait(lock, [this]() {
            return closed_ || !queue_.empty();
        });

        if (queue_.empty() && closed_) return false;

        out_value = std::move(queue_.front());
        queue_.pop();
        cv_push_.notify_one();
        return true;
    }

    void close() {
        std::unique_lock<std::mutex> lock(mtx_);
        closed_ = true;
        cv_push_.notify_all();
        cv_pop_.notify_all();
    }

    [[nodiscard]] bool is_closed() const {
        std::unique_lock<std::mutex> lock(mtx_);
        return closed_;
    }

private:
    std::queue<T> queue_;
    size_t capacity_;
    bool closed_;
    mutable std::mutex mtx_;
    std::condition_variable cv_push_;
    std::condition_variable cv_pop_;
};

// ============================================================================
// Template Implementations
// ============================================================================

template<class F, class... Args>
auto ThreadPool::enqueue(F&& f, Args&&... args) -> std::future<std::invoke_result_t<F, Args...>> {
    using return_type = std::invoke_result_t<F, Args...>;

    auto task = std::make_shared<std::packaged_task<return_type()>>(
        std::bind(std::forward<F>(f), std::forward<Args>(args)...)
    );

    std::future<return_type> res = task->get_future();
    {
        std::unique_lock<std::mutex> lock(queue_mutex);
        if(stop) throw std::runtime_error("enqueue on stopped ThreadPool");
        tasks.emplace([task]() { (*task)(); });
    }
    condition.notify_one();
    return res;
}

} // namespace nova::concurrency