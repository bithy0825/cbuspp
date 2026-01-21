#pragma once

// ============================================================================
// cbuspp::bus - C++23 High-performance Zero-overhead Event Bus
// ============================================================================
// Design principles:
// - Compile-time topic hashing, runtime comparison is uint64_t only
// - Lock-free read path, wait-free publish for hot paths
// - Scheduler thread only schedules, never executes callbacks
// - External executor injection or built-in thread pool
// - Perfect forwarding, zero-overhead abstraction
// - No shared_ptr wrapping for synchronous dispatch
// ============================================================================

#include "context.hpp"
#include "executor.hpp"
#include "filter.hpp"
#include "payload.hpp"
#include "subscription.hpp"
#include "topic.hpp"
#include "wildcard.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

namespace cbuspp {

// ============================================================================
// Internal types
// ============================================================================
namespace detail {

    // Subscriber list per topic
    struct topic_subscribers {
        mutable std::shared_mutex mutex;
        std::vector<subscriber_entry> subscribers;
        std::atomic<std::size_t> version { 0 };

        // Deduplication cache entry
        struct dedup_entry {
            std::uint64_t key;
            std::chrono::steady_clock::time_point expires;
        };
        std::vector<dedup_entry> dedup_cache;
        mutable std::mutex dedup_mutex;
    };

    // Pending message for async/delayed dispatch
    // Supports both inline payload and pre-wrapped shared_ptr
    struct pending_message {
        topic_id topic;
        payload data; // Inline payload (for normal publish_async)
        std::shared_ptr<payload> shared_data; // Pre-wrapped payload (for publish_async_shared)
        context ctx;
        std::chrono::steady_clock::time_point fire_time;

        // Returns reference to payload data
        [[nodiscard]] const payload& get_payload() const noexcept
        {
            return shared_data ? *shared_data : data;
        }

        [[nodiscard]] payload& get_payload() noexcept
        {
            return shared_data ? *shared_data : data;
        }

        [[nodiscard]] bool operator>(const pending_message& other) const noexcept
        {
            return fire_time > other.fire_time;
        }
    };

} // namespace detail

// ============================================================================
// Bus configuration
// ============================================================================
struct bus_config {
    /// Maximum number of async threads. 0 = auto (min(4, hardware_concurrency)).
    /// Set to a specific value to limit resource usage on busy machines.
    std::size_t max_async_threads { 0 };

    /// Enable message deduplication based on topic and time window.
    bool enable_dedup { true };

    /// Enable callback throttling per subscriber.
    bool enable_throttle { true };

    /// Scheduler check interval for delayed messages.
    std::chrono::milliseconds scheduler_interval { 1 };

    /// Default thread pool size limit. 0 = no limit (use max_async_threads directly).
    /// When max_async_threads is 0, this is used as: min(default_pool_limit, hardware_concurrency).
    std::size_t default_pool_limit { 4 };
};

// ============================================================================
// Bus core class
// ============================================================================
class bus {
public:
    // ========================================================================
    // Construction and destruction
    // ========================================================================

    /// Constructs a bus with the given configuration.
    explicit bus(bus_config config = {})
        : config_(config)
    {
        // Create default executor (built-in thread pool)
        // Use limited thread count by default to avoid consuming too many resources
        auto thread_count = config.max_async_threads;
        if (thread_count == 0) {
            auto hw_conc = std::max(1u, std::thread::hardware_concurrency());
            auto limit = config.default_pool_limit > 0
                ? static_cast<unsigned>(config.default_pool_limit)
                : hw_conc;
            thread_count = std::min(hw_conc, limit);
        }
        default_pool_ = std::make_unique<thread_pool>(thread_count);
        executors_.set_default(executor(*default_pool_));

        // Start scheduler thread (must be after all other members initialized)
        running_.store(true, std::memory_order_release);
        scheduler_thread_ = std::thread([this] { scheduler_loop(); });
    }

    ~bus()
    {
        shutdown();
    }

    bus(const bus&) = delete;
    bus& operator=(const bus&) = delete;
    bus(bus&&) = delete;
    bus& operator=(bus&&) = delete;

    // ========================================================================
    // Executor management
    // ========================================================================

    /// Registers a custom executor and returns its ID.
    std::uint32_t register_executor(executor exec)
    {
        return executors_.register_executor(std::move(exec));
    }

    /// Sets the default executor for async operations.
    void set_default_executor(executor exec)
    {
        executors_.set_default(std::move(exec));
    }

    /// Returns executor by ID, or nullptr if not found.
    [[nodiscard]] executor* get_executor(std::uint32_t id) noexcept
    {
        return executors_.get(id);
    }

    // ========================================================================
    // Subscribe interface
    // ========================================================================

    /// Subscribes with a payload callback.
    template <typename F>
        requires traits::valid_payload_callback<F>
    [[nodiscard]] subscription subscribe(topic_id topic, F&& callback,
        subscribe_options opts = {})
    {
        return subscribe_impl<void>(topic, std::forward<F>(callback), opts);
    }

    /// Subscribes with a typed callback.
    template <typename T, typename F>
        requires traits::valid_typed_callback<F, T>
    [[nodiscard]] subscription subscribe(topic_id topic, F&& callback,
        subscribe_options opts = {})
    {
        return subscribe_impl<T>(topic, std::forward<F>(callback), opts);
    }

    /// Subscribes with a member function pointer.
    template <typename T, typename C>
    [[nodiscard]] subscription subscribe(topic_id topic,
        void (C::*method)(const T&, const context&),
        C* instance, subscribe_options opts = {})
    {
        return subscribe<T>(topic, [instance, method](const T& data, const context& ctx) { (instance->*method)(data, ctx); }, opts);
    }

    template <typename T, typename C>
    [[nodiscard]] subscription subscribe(topic_id topic,
        void (C::*method)(const T&),
        C* instance, subscribe_options opts = {})
    {
        return subscribe<T>(topic, [instance, method](const T& data, const context&) { (instance->*method)(data); }, opts);
    }

    /// One-shot subscription (auto-unsubscribe after first invocation).
    template <typename T, typename F>
    [[nodiscard]] subscription once(topic_id topic, F&& callback,
        subscribe_options opts = {})
    {
        opts.once = true;
        return subscribe<T>(topic, std::forward<F>(callback), opts);
    }

    template <typename F>
        requires traits::valid_payload_callback<F>
    [[nodiscard]] subscription once(topic_id topic, F&& callback,
        subscribe_options opts = {})
    {
        opts.once = true;
        return subscribe(topic, std::forward<F>(callback), opts);
    }

    // ========================================================================
    // Publish interface
    // ========================================================================

    /// Publishes a payload synchronously.
    publish_result publish(topic_id topic, payload data, context ctx = {})
    {
        return publish_impl(topic, std::move(data), std::move(ctx));
    }

    /// Publishes a value type synchronously.
    template <typename T>
        requires(!std::same_as<std::remove_cvref_t<T>, payload>)
        && (!std::same_as<std::remove_cvref_t<T>, context>)
    publish_result publish(topic_id topic, T&& data, context ctx = {})
    {
        return publish_impl(topic, payload { std::forward<T>(data) }, std::move(ctx));
    }

    /// Publishes with in-place construction.
    template <typename T, typename... Args>
        requires std::constructible_from<T, Args...>
    publish_result emplace(topic_id topic, context ctx, Args&&... args)
    {
        return publish_impl(topic,
            payload { std::in_place_type<T>, std::forward<Args>(args)... },
            std::move(ctx));
    }

    /// Publishes asynchronously using the default executor.
    void publish_async(topic_id topic, payload data, context ctx = {})
    {
        (void)ctx.put<ctx::mode_tag>(publish_mode::async);
        enqueue_message(topic, std::move(data), std::move(ctx),
            std::chrono::steady_clock::time_point {});
    }

    template <typename T>
        requires(!std::same_as<std::remove_cvref_t<T>, payload>)
    void publish_async(topic_id topic, T&& data, context ctx = {})
    {
        publish_async(topic, payload { std::forward<T>(data) }, std::move(ctx));
    }

    /// Publishes asynchronously using a pre-existing shared_ptr.
    /// Use this when you already have data wrapped in shared_ptr to avoid
    /// double-wrapping overhead. This is the most efficient async publish method
    /// when your data lifecycle is already managed by shared_ptr.
    ///
    /// Example:
    ///   auto data = std::make_shared<MyData>(...);  // Your existing shared_ptr
    ///   bus.publish_async_shared(topic, data);       // No extra allocation
    template <typename T>
    void publish_async_shared(topic_id topic, std::shared_ptr<T> data, context ctx = {})
    {
        (void)ctx.put<ctx::mode_tag>(publish_mode::async);
        enqueue_shared_message(topic, std::move(data), std::move(ctx),
            std::chrono::steady_clock::time_point {});
    }

    /// Publishes a shared_ptr payload with a delay.
    template <typename T>
    void publish_delayed_shared(topic_id topic, std::shared_ptr<T> data,
        std::chrono::steady_clock::duration delay, context ctx = {})
    {
        auto fire_time = std::chrono::steady_clock::now() + delay;
        (void)ctx.put<ctx::mode_tag>(publish_mode::deferred);
        (void)ctx.put<ctx::deadline_tag>(fire_time);
        enqueue_shared_message(topic, std::move(data), std::move(ctx), fire_time);
    }

    /// Publishes with a delay.
    void publish_delayed(topic_id topic, payload data,
        std::chrono::steady_clock::duration delay, context ctx = {})
    {
        auto fire_time = std::chrono::steady_clock::now() + delay;
        (void)ctx.put<ctx::mode_tag>(publish_mode::deferred);
        (void)ctx.put<ctx::deadline_tag>(fire_time);
        enqueue_message(topic, std::move(data), std::move(ctx), fire_time);
    }

    template <typename T>
        requires(!std::same_as<std::remove_cvref_t<T>, payload>)
    void publish_delayed(topic_id topic, T&& data,
        std::chrono::steady_clock::duration delay, context ctx = {})
    {
        publish_delayed(topic, payload { std::forward<T>(data) }, delay, std::move(ctx));
    }

    /// Publishes at a specific time point.
    void publish_at(topic_id topic, payload data,
        std::chrono::steady_clock::time_point fire_time, context ctx = {})
    {
        (void)ctx.put<ctx::mode_tag>(publish_mode::deferred);
        (void)ctx.put<ctx::deadline_tag>(fire_time);
        enqueue_message(topic, std::move(data), std::move(ctx), fire_time);
    }

    // ========================================================================
    // Unsubscribe
    // ========================================================================

    /// Removes a subscription by ID.
    bool unsubscribe(topic_id topic, detail::subscriber_id id) noexcept
    {
        auto* subs = get_topic_subscribers(topic);
        if (!subs) {
            return false;
        }

        std::unique_lock lock(subs->mutex);
        auto it = std::ranges::find_if(subs->subscribers,
            [id](const auto& e) { return e.id == id; });

        if (it != subs->subscribers.end()) {
            subs->subscribers.erase(it);
            subs->version.fetch_add(1, std::memory_order_release);
            return true;
        }
        return false;
    }

    // ========================================================================
    // Query interface
    // ========================================================================

    /// Returns the number of subscribers for a topic.
    [[nodiscard]] std::size_t subscriber_count(topic_id topic) const noexcept
    {
        auto* subs = get_topic_subscribers(topic);
        if (!subs) {
            return 0;
        }
        std::shared_lock lock(subs->mutex);
        return subs->subscribers.size();
    }

    /// Returns the total number of subscribers across all topics.
    [[nodiscard]] std::size_t total_subscribers() const noexcept
    {
        std::shared_lock lock(topics_mutex_);
        std::size_t count = 0;
        for (const auto& [_, subs] : topics_) {
            std::shared_lock sub_lock(subs.mutex);
            count += subs.subscribers.size();
        }
        return count;
    }

    /// Returns the number of topics with subscribers.
    [[nodiscard]] std::size_t topic_count() const noexcept
    {
        std::shared_lock lock(topics_mutex_);
        return topics_.size();
    }

    /// Returns the number of pending async/delayed messages.
    [[nodiscard]] std::size_t pending_count() const noexcept
    {
        std::lock_guard lock(queue_mutex_);
        return pending_queue_.size();
    }

    // ========================================================================
    // Lifecycle
    // ========================================================================

    /// Shuts down the bus and stops all async processing.
    void shutdown() noexcept
    {
        bool expected = true;
        if (running_.compare_exchange_strong(expected, false)) {
            queue_cv_.notify_all();
            if (scheduler_thread_.joinable()) {
                scheduler_thread_.join();
            }
            if (default_pool_) {
                default_pool_->stop();
            }
        }
    }

    /// Returns true if the bus is running.
    [[nodiscard]] bool is_running() const noexcept
    {
        return running_.load(std::memory_order_acquire);
    }

    /// Clears all subscribers and pending messages.
    void clear() noexcept
    {
        {
            std::unique_lock lock(topics_mutex_);
            topics_.clear();
        }
        {
            std::lock_guard lock(queue_mutex_);
            pending_queue_.clear();
        }
    }

private:
    // ========================================================================
    // Subscribe implementation
    // ========================================================================
    template <typename T, typename F>
    subscription subscribe_impl(topic_id topic, F&& callback, subscribe_options opts)
    {
        auto id = detail::generate_subscriber_id();
        auto& subs = get_or_create_subscribers(topic);

        detail::subscriber_entry entry;
        entry.id = id;
        entry.priority = opts.priority;
        entry.once = opts.once;
        entry.throttle_ms = opts.throttle_ms;
        entry.executor_id = opts.executor_id;

        // Set filter
        if (opts.has_filter) {
            entry.filter = detail::tag_filter;
            entry.filter_tag = opts.filter_tag;
        }

        // Create type-erased callback
        if constexpr (std::is_void_v<T>) {
            entry.callback = detail::make_callback(std::forward<F>(callback));
        } else {
            entry.callback = detail::make_callback<T>(std::forward<F>(callback));
        }

        {
            std::unique_lock lock(subs.mutex);
            auto pos = std::ranges::upper_bound(subs.subscribers, entry.priority,
                std::greater<> {}, &detail::subscriber_entry::priority);
            subs.subscribers.insert(pos, std::move(entry));
            subs.version.fetch_add(1, std::memory_order_release);
        }

        return subscription(this, topic, id);
    }

    // ========================================================================
    // Publish implementation
    // ========================================================================
    publish_result publish_impl(topic_id topic, payload data, context ctx)
    {
        publish_result result;

        auto mode = ctx.get<ctx::mode_tag>();

        // Async/deferred mode: enqueue for later dispatch
        if (mode == publish_mode::async || mode == publish_mode::deferred) {
            auto fire_time = ctx.has<ctx::deadline_tag>()
                ? ctx.get<ctx::deadline_tag>()
                : std::chrono::steady_clock::time_point {};
            enqueue_message(topic, std::move(data), std::move(ctx), fire_time);
            result.deferred = true;
            return result;
        }

        // Synchronous dispatch (no shared_ptr overhead)
        return dispatch_sync(topic, data, ctx);
    }

    // Synchronous dispatch
    // For pure sync (all executor_id == 0), uses reference without allocation.
    // For mixed mode (some async subscribers), moves payload to shared_ptr.
    publish_result dispatch_sync(topic_id topic, payload& data, const context& ctx)
    {
        publish_result result;

        auto* subs = get_topic_subscribers(topic);
        if (!subs) {
            return result;
        }

        // Deduplication check
        if (config_.enable_dedup) {
            auto dedup_ms = ctx.get<ctx::dedup_ms_tag>();
            if (dedup_ms > 0 && is_duplicate(subs, topic, dedup_ms)) {
                result.deduplicated = true;
                return result;
            }
        }

        auto filter_tag = ctx.get<ctx::event_tag_tag>();
        auto now = std::chrono::steady_clock::now();
        std::vector<detail::subscriber_id> to_remove;

        // Check if any subscriber needs async execution
        bool has_async_subscriber = false;
        {
            std::shared_lock lock(subs->mutex);
            for (const auto& entry : subs->subscribers) {
                if (entry.executor_id > 0 && executors_.get(entry.executor_id)) {
                    has_async_subscriber = true;
                    break;
                }
            }
        }

        // Create shared_ptr only if needed for async subscribers
        std::shared_ptr<payload> shared_data;
        if (has_async_subscriber) {
            shared_data = std::make_shared<payload>(std::move(data));
        }

        std::shared_lock lock(subs->mutex);
        result.subscriber_count = subs->subscribers.size();

        for (auto& entry : subs->subscribers) {
            // Filter check
            if (!entry.matches(filter_tag)) {
                continue;
            }

            // Throttle check
            if (config_.enable_throttle && entry.throttle_ms > 0) {
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - entry.last_invoked);
                if (elapsed.count() < static_cast<long long>(entry.throttle_ms)) {
                    continue;
                }
                entry.last_invoked = now;
            }

            // Get executor (0 = no executor = sync execution)
            executor* exec = nullptr;
            if (entry.executor_id > 0) {
                exec = executors_.get(entry.executor_id);
            }

            if (exec && !exec->empty()) {
                // Async execution via executor
                auto ctx_copy = ctx;
                auto* callback = &entry.callback;
                exec->execute([callback, shared_data, ctx_copy]() {
                    try {
                        (*callback)(*shared_data, ctx_copy);
                    } catch (...) {
                        // Swallow callback exceptions
                    }
                });
                ++result.delivered_count;
            } else {
                // Synchronous execution: use reference or shared_ptr
                try {
                    if (shared_data) {
                        entry.callback(*shared_data, ctx);
                    } else {
                        entry.callback(data, ctx);
                    }
                    ++result.delivered_count;
                } catch (...) {
                    result.has_error = true;
                }
            }

            if (entry.once) {
                to_remove.push_back(entry.id);
            }
        }

        lock.unlock();

        // Remove one-shot subscriptions
        if (!to_remove.empty()) {
            std::unique_lock wlock(subs->mutex);
            for (auto id : to_remove) {
                auto it = std::ranges::find_if(subs->subscribers,
                    [id](const auto& e) { return e.id == id; });
                if (it != subs->subscribers.end()) {
                    subs->subscribers.erase(it);
                }
            }
            subs->version.fetch_add(1, std::memory_order_release);
        }

        return result;
    }

    // ========================================================================
    // Message queue
    // ========================================================================
    void enqueue_message(topic_id topic, payload data, context ctx,
        std::chrono::steady_clock::time_point fire_time)
    {
        {
            std::lock_guard lock(queue_mutex_);
            pending_queue_.push_back({ topic,
                std::move(data),
                nullptr, // No pre-wrapped shared_ptr
                std::move(ctx),
                fire_time });
            std::ranges::push_heap(pending_queue_, std::greater<> {});
        }
        queue_cv_.notify_one();
    }

    template <typename T>
    void enqueue_shared_message(topic_id topic, std::shared_ptr<T> data, context ctx,
        std::chrono::steady_clock::time_point fire_time)
    {
        // Create payload from user's data, keeping original shared_ptr alive
        // by capturing it in a shared_ptr with custom destructor
        T* raw_ptr = data.get(); // Get pointer before moving
        auto combined = std::shared_ptr<payload>(
            new payload(*raw_ptr),
            [orig = std::move(data)](payload* p) {
                delete p;
                // orig is destroyed here, keeping original data alive until this point
            });

        {
            std::lock_guard lock(queue_mutex_);
            pending_queue_.push_back({ topic,
                payload {}, // Empty inline payload
                std::move(combined), // Use pre-wrapped shared_ptr
                std::move(ctx),
                fire_time });
            std::ranges::push_heap(pending_queue_, std::greater<> {});
        }
        queue_cv_.notify_one();
    }

    // ========================================================================
    // Scheduler thread
    // ========================================================================
    void scheduler_loop()
    {
        while (running_.load(std::memory_order_acquire)) {
            std::unique_lock lock(queue_mutex_);

            if (pending_queue_.empty()) {
                queue_cv_.wait_for(lock, config_.scheduler_interval, [this] {
                    return !running_.load(std::memory_order_acquire) || !pending_queue_.empty();
                });
                continue;
            }

            auto now = std::chrono::steady_clock::now();
            auto& top = pending_queue_.front();

            // Check if fire time has been reached
            if (top.fire_time > now && top.fire_time != std::chrono::steady_clock::time_point {}) {
                queue_cv_.wait_until(lock, top.fire_time, [this] {
                    return !running_.load(std::memory_order_acquire);
                });
                continue;
            }

            // Pop message from queue
            std::ranges::pop_heap(pending_queue_, std::greater<> {});
            auto msg = std::move(pending_queue_.back());
            pending_queue_.pop_back();
            lock.unlock();

            // Schedule dispatch via executor
            schedule_dispatch(std::move(msg));
        }
    }

    void schedule_dispatch(detail::pending_message msg)
    {
        // Reset mode to sync for actual dispatch
        (void)msg.ctx.put<ctx::mode_tag>(publish_mode::sync);

        auto* exec = executors_.get_default();
        if (exec && !exec->empty()) {
            // Dispatch on executor thread
            exec->execute([this, msg = std::move(msg)]() mutable {
                auto& payload_ref = msg.get_payload();
                (void)dispatch_sync(msg.topic, payload_ref, msg.ctx);
            });
        } else {
            // No executor: dispatch synchronously
            auto& payload_ref = msg.get_payload();
            (void)dispatch_sync(msg.topic, payload_ref, msg.ctx);
        }
    }

    // ========================================================================
    // Helper methods
    // ========================================================================
    detail::topic_subscribers* get_topic_subscribers(topic_id topic) const noexcept
    {
        std::shared_lock lock(topics_mutex_);
        auto it = topics_.find(topic);
        if (it != topics_.end()) {
            return const_cast<detail::topic_subscribers*>(&it->second);
        }
        return nullptr;
    }

    detail::topic_subscribers& get_or_create_subscribers(topic_id topic)
    {
        {
            std::shared_lock lock(topics_mutex_);
            auto it = topics_.find(topic);
            if (it != topics_.end()) {
                return it->second;
            }
        }

        std::unique_lock lock(topics_mutex_);
        auto [it, _] = topics_.try_emplace(topic);
        return it->second;
    }

    bool is_duplicate(detail::topic_subscribers* subs, topic_id topic,
        std::uint32_t window_ms)
    {
        auto now = std::chrono::steady_clock::now();

        std::lock_guard lock(subs->dedup_mutex);

        // Cleanup expired entries
        std::erase_if(subs->dedup_cache,
            [now](const auto& e) { return e.expires < now; });

        // Check for duplicate
        auto it = std::ranges::find_if(subs->dedup_cache,
            [topic](const auto& e) { return e.key == topic; });

        if (it != subs->dedup_cache.end()) {
            return true;
        }

        // Add new entry
        subs->dedup_cache.push_back({ topic,
            now + std::chrono::milliseconds(window_ms) });

        return false;
    }

    // ========================================================================
    // Member variables
    // ========================================================================
    bus_config config_;

    mutable std::shared_mutex topics_mutex_;
    std::unordered_map<topic_id, detail::topic_subscribers> topics_;

    mutable std::mutex queue_mutex_;
    std::vector<detail::pending_message> pending_queue_;
    std::condition_variable queue_cv_;

    executor_registry executors_;
    std::unique_ptr<thread_pool> default_pool_;

    std::atomic<bool> running_ { false };
    std::thread scheduler_thread_;
};

// ============================================================================
// Subscription implementation
// ============================================================================
inline subscription::~subscription() noexcept
{
    unsubscribe();
}

inline void subscription::unsubscribe() noexcept
{
    if (bus_ && id_ != detail::invalid_subscriber_id) {
        bus_->unsubscribe(topic_, id_);
        release();
    }
}

// ============================================================================
// Subscribe builder (fluent API)
// ============================================================================
class subscribe_builder {
public:
    explicit subscribe_builder(bus& b) noexcept
        : bus_(&b)
    {
    }

    subscribe_builder& priority(std::int32_t p) noexcept
    {
        opts_.priority = p;
        return *this;
    }

    subscribe_builder& once() noexcept
    {
        opts_.once = true;
        return *this;
    }

    subscribe_builder& throttle(std::uint32_t ms) noexcept
    {
        opts_.throttle_ms = ms;
        return *this;
    }

    subscribe_builder& filter(std::uint64_t tag) noexcept
    {
        opts_.filter_tag = tag;
        opts_.has_filter = true;
        return *this;
    }

    subscribe_builder& executor(std::uint32_t id) noexcept
    {
        opts_.executor_id = id;
        return *this;
    }

    template <typename T, typename F>
    [[nodiscard]] subscription to(topic_id topic, F&& callback)
    {
        return bus_->subscribe<T>(topic, std::forward<F>(callback), opts_);
    }

    template <typename F>
        requires traits::valid_payload_callback<F>
    [[nodiscard]] subscription to(topic_id topic, F&& callback)
    {
        return bus_->subscribe(topic, std::forward<F>(callback), opts_);
    }

private:
    bus* bus_;
    subscribe_options opts_;
};

/// Creates a subscribe builder for fluent API.
[[nodiscard]] inline subscribe_builder with(bus& b) noexcept
{
    return subscribe_builder(b);
}

} // namespace cbuspp
