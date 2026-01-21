#pragma once

// ============================================================================
// cbuspp::executor - Executor abstraction and built-in thread pool
// ============================================================================
// Design principles:
// - Scheduler thread only schedules, never executes callbacks
// - External executor injection for custom thread pool integration
// - Built-in thread pool as default executor
// - Small Buffer Optimization (SBO) to avoid heap allocation for small executors
// - Zero virtual function overhead using type-erased callable
// ============================================================================

#include <array>
#include <concepts>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace cbuspp {

// ============================================================================
// Task type
// ============================================================================

using task_t = std::move_only_function<void()>;

// ============================================================================
// Executor concept
// ============================================================================

/// An executor is any type that can execute tasks.
template <typename E>
concept executor_like = requires(E& e, task_t t) {
    { e.execute(std::move(t)) };
};

/// An executor that can report its thread count.
template <typename E>
concept sized_executor = executor_like<E> && requires(const E& e) {
    { e.size() } -> std::convertible_to<std::size_t>;
};

// ============================================================================
// Executor wrapper (type-erased with SBO)
// ============================================================================

class executor {
public:
    /// Small buffer size for SBO (fits reference wrapper + vtable pointer).
    static constexpr std::size_t sbo_size = sizeof(void*) * 3;
    static constexpr std::size_t sbo_align = alignof(std::max_align_t);

    executor() noexcept = default;

    /// Constructs from any executor-like object (stores by reference).
    template <executor_like E>
        requires(!std::same_as<std::decay_t<E>, executor>)
    explicit executor(E& e) noexcept
    {
        static_assert(sizeof(ref_model<E>) <= sbo_size,
            "Executor reference model exceeds SBO size");
        ::new (storage()) ref_model<E>(e);
        ops_ = &ref_model<E>::ops;
    }

    ~executor()
    {
        destroy();
    }

    executor(const executor& other)
        : ops_(other.ops_)
    {
        if (ops_) {
            ops_->copy(storage(), other.storage());
        }
    }

    executor& operator=(const executor& other)
    {
        if (this != &other) {
            destroy();
            ops_ = other.ops_;
            if (ops_) {
                ops_->copy(storage(), other.storage());
            }
        }
        return *this;
    }

    executor(executor&& other) noexcept
        : ops_(std::exchange(other.ops_, nullptr))
    {
        if (ops_) {
            ops_->move(storage(), other.storage());
        }
    }

    executor& operator=(executor&& other) noexcept
    {
        if (this != &other) {
            destroy();
            ops_ = std::exchange(other.ops_, nullptr);
            if (ops_) {
                ops_->move(storage(), other.storage());
            }
        }
        return *this;
    }

    /// Executes a task on this executor.
    void execute(task_t task) const
    {
        if (ops_) {
            ops_->execute(storage(), std::move(task));
        } else {
            // No executor: execute synchronously
            task();
        }
    }

    /// Returns true if executor is set.
    [[nodiscard]] explicit operator bool() const noexcept
    {
        return ops_ != nullptr;
    }

    /// Returns true if no executor is set.
    [[nodiscard]] bool empty() const noexcept
    {
        return ops_ == nullptr;
    }

private:
    // Type-erased operations table
    struct ops_table {
        void (*execute)(const void* self, task_t task);
        void (*copy)(void* dst, const void* src);
        void (*move)(void* dst, void* src) noexcept;
        void (*destroy)(void* self) noexcept;
    };

    // Reference-based model (stores std::reference_wrapper<E>)
    template <typename E>
    struct ref_model {
        std::reference_wrapper<E> ref;

        explicit ref_model(E& e) noexcept
            : ref(e)
        {
        }

        static void do_execute(const void* self, task_t task)
        {
            static_cast<const ref_model*>(self)->ref.get().execute(std::move(task));
        }

        static void do_copy(void* dst, const void* src)
        {
            ::new (dst) ref_model(*static_cast<const ref_model*>(src));
        }

        static void do_move(void* dst, void* src) noexcept
        {
            ::new (dst) ref_model(std::move(*static_cast<ref_model*>(src)));
            static_cast<ref_model*>(src)->~ref_model();
        }

        static void do_destroy(void* self) noexcept
        {
            static_cast<ref_model*>(self)->~ref_model();
        }

        static constexpr ops_table ops {
            &do_execute, &do_copy, &do_move, &do_destroy
        };
    };

    void destroy() noexcept
    {
        if (ops_) {
            ops_->destroy(storage());
            ops_ = nullptr;
        }
    }

    [[nodiscard]] void* storage() noexcept
    {
        return static_cast<void*>(buffer_);
    }

    [[nodiscard]] const void* storage() const noexcept
    {
        return static_cast<const void*>(buffer_);
    }

    alignas(sbo_align) std::byte buffer_[sbo_size] {};
    const ops_table* ops_ { nullptr };
};

// ============================================================================
// Inline executor (executes in calling thread)
// ============================================================================

struct inline_executor {
    void execute(task_t task) const
    {
        task();
    }

    [[nodiscard]] static constexpr std::size_t size() noexcept
    {
        return 0;
    }
};

// ============================================================================
// Thread pool (built-in executor)
// ============================================================================

class thread_pool {
public:
    /// Constructs a thread pool with specified thread count.
    /// @param thread_count Number of worker threads (0 = hardware concurrency).
    explicit thread_pool(std::size_t thread_count = 0)
        : thread_count_(thread_count == 0 ? default_thread_count() : thread_count)
    {
        start();
    }

    ~thread_pool()
    {
        stop();
    }

    thread_pool(const thread_pool&) = delete;
    thread_pool& operator=(const thread_pool&) = delete;
    thread_pool(thread_pool&&) = delete;
    thread_pool& operator=(thread_pool&&) = delete;

    /// Submits a task for execution.
    void execute(task_t task)
    {
        {
            std::lock_guard lock(mutex_);
            if (!running_) {
                task(); // Pool stopped, execute synchronously
                return;
            }
            tasks_.push(std::move(task));
        }
        cv_.notify_one();
    }

    /// Returns the number of worker threads.
    [[nodiscard]] std::size_t size() const noexcept
    {
        return thread_count_;
    }

    /// Returns the number of pending tasks.
    [[nodiscard]] std::size_t pending() const noexcept
    {
        std::lock_guard lock(mutex_);
        return tasks_.size();
    }

    /// Stops the thread pool and waits for all threads to finish.
    void stop()
    {
        {
            std::lock_guard lock(mutex_);
            if (!running_) {
                return;
            }
            running_ = false;
        }
        cv_.notify_all();

        for (auto& t : threads_) {
            if (t.joinable()) {
                t.join();
            }
        }
        threads_.clear();
    }

    /// Starts the thread pool if not already running.
    void start()
    {
        std::lock_guard lock(mutex_);
        if (running_) {
            return;
        }
        running_ = true;
        threads_.reserve(thread_count_);
        for (std::size_t i = 0; i < thread_count_; ++i) {
            threads_.emplace_back([this] { worker_loop(); });
        }
    }

    /// Returns the default thread count based on hardware.
    [[nodiscard]] static std::size_t default_thread_count() noexcept
    {
        auto n = std::thread::hardware_concurrency();
        return n > 0 ? n : 4;
    }

private:
    void worker_loop()
    {
        while (true) {
            task_t task;
            {
                std::unique_lock lock(mutex_);
                cv_.wait(lock, [this] {
                    return !running_ || !tasks_.empty();
                });

                if (!running_ && tasks_.empty()) {
                    return;
                }

                if (!tasks_.empty()) {
                    task = std::move(tasks_.front());
                    tasks_.pop();
                }
            }

            if (task) {
                task();
            }
        }
    }

    std::size_t thread_count_;
    std::vector<std::thread> threads_;
    std::queue<task_t> tasks_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool running_ { false };
};

// ============================================================================
// Single-thread executor (sequential execution)
// ============================================================================

class single_thread_executor {
public:
    single_thread_executor()
        : running_(true)
        , thread_([this] { worker_loop(); })
    {
    }

    ~single_thread_executor()
    {
        stop();
    }

    single_thread_executor(const single_thread_executor&) = delete;
    single_thread_executor& operator=(const single_thread_executor&) = delete;

    /// Submits a task for sequential execution.
    void execute(task_t task)
    {
        {
            std::lock_guard lock(mutex_);
            if (!running_) {
                task();
                return;
            }
            tasks_.push(std::move(task));
        }
        cv_.notify_one();
    }

    [[nodiscard]] static constexpr std::size_t size() noexcept
    {
        return 1;
    }

    /// Stops the executor thread.
    void stop()
    {
        {
            std::lock_guard lock(mutex_);
            if (!running_) {
                return;
            }
            running_ = false;
        }
        cv_.notify_all();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

private:
    void worker_loop()
    {
        while (true) {
            task_t task;
            {
                std::unique_lock lock(mutex_);
                cv_.wait(lock, [this] {
                    return !running_ || !tasks_.empty();
                });

                if (!running_ && tasks_.empty()) {
                    return;
                }

                if (!tasks_.empty()) {
                    task = std::move(tasks_.front());
                    tasks_.pop();
                }
            }

            if (task) {
                task();
            }
        }
    }

    std::queue<task_t> tasks_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool running_ { false };
    std::thread thread_;
};

// ============================================================================
// Execution policy
// ============================================================================

enum class execution_policy : std::uint8_t {
    sync, // Execute in publisher's thread
    async, // Execute via default executor
    deferred, // Execute after deadline via executor
    strand // Execute sequentially for same topic
};

// ============================================================================
// Executor registry
// ============================================================================

class executor_registry {
public:
    static constexpr std::size_t max_executors = 16;
    static constexpr std::uint32_t default_executor_id = 0;

    executor_registry() = default;

    /// Registers an executor and returns its ID.
    std::uint32_t register_executor(executor exec)
    {
        std::lock_guard lock(mutex_);
        if (count_ >= max_executors) {
            return default_executor_id;
        }
        auto id = count_++;
        executors_[id] = std::move(exec);
        return static_cast<std::uint32_t>(id);
    }

    /// Returns executor by ID, or nullptr if not found.
    [[nodiscard]] executor* get(std::uint32_t id) noexcept
    {
        if (id >= count_) {
            return nullptr;
        }
        return &executors_[id];
    }

    /// Sets the default executor (ID 0).
    void set_default(executor exec)
    {
        std::lock_guard lock(mutex_);
        executors_[default_executor_id] = std::move(exec);
        if (count_ == 0) {
            count_ = 1;
        }
    }

    /// Returns the default executor.
    [[nodiscard]] executor* get_default() noexcept
    {
        return get(default_executor_id);
    }

    /// Returns the number of registered executors.
    [[nodiscard]] std::size_t size() const noexcept
    {
        return count_;
    }

private:
    std::array<executor, max_executors> executors_;
    std::size_t count_ { 0 };
    std::mutex mutex_;
};

// ============================================================================
// Factory functions
// ============================================================================

/// Creates an inline (synchronous) executor.
[[nodiscard]] inline executor make_inline_executor()
{
    static inline_executor instance;
    return executor(instance);
}

} // namespace cbuspp
