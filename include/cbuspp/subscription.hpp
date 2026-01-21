#pragma once

// ============================================================================
// cbuspp::subscription - Subscription types and callback wrappers
// ============================================================================
// Design principles:
// - Zero virtual function overhead using std::move_only_function
// - Type-safe callback wrapping with compile-time dispatch
// - RAII-based subscription lifecycle management
// - Minimal memory footprint per subscriber
// ============================================================================

#include "context.hpp"
#include "payload.hpp"
#include "topic.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <vector>

namespace cbuspp {

// Forward declaration
class bus;

// ============================================================================
// Subscriber ID
// ============================================================================
namespace detail {

    using subscriber_id = std::uint64_t;
    inline constexpr subscriber_id invalid_subscriber_id = 0;

    inline std::atomic<subscriber_id> g_next_subscriber_id { 1 };

    [[nodiscard]] inline subscriber_id generate_subscriber_id() noexcept
    {
        return g_next_subscriber_id.fetch_add(1, std::memory_order_relaxed);
    }

} // namespace detail

// ============================================================================
// Callback signature detection
// ============================================================================
namespace traits {

    // Detects callback accepting (T&, context&)
    template <typename F, typename T>
    concept typed_callback = requires(F f, T& t, const context& c) {
        { f(t, c) };
    };

    // Detects callback accepting (payload&, context&)
    template <typename F>
    concept payload_callback = requires(F f, const payload& p, const context& c) {
        { f(p, c) };
    };

    // Detects callback accepting (T&) only
    template <typename F, typename T>
    concept typed_callback_simple = requires(F f, T& t) {
        { f(t) };
    } && !typed_callback<F, T>;

    // Detects callback accepting (payload&) only
    template <typename F>
    concept payload_callback_simple = requires(F f, const payload& p) {
        { f(p) };
    } && !payload_callback<F>;

    // Any valid typed callback
    template <typename F, typename T>
    concept valid_typed_callback = typed_callback<F, T> || typed_callback_simple<F, T>;

    // Any valid payload callback
    template <typename F>
    concept valid_payload_callback = payload_callback<F> || payload_callback_simple<F>;

} // namespace traits

// ============================================================================
// Callback function type (type-erased, no virtual dispatch)
// ============================================================================
namespace detail {

    // Callback signature: void(const payload&, const context&)
    using callback_fn = std::move_only_function<void(const payload&, const context&)>;

    // Filter function type
    using filter_fn = bool (*)(std::uint64_t filter_tag, std::uint64_t event_tag) noexcept;

    // Default filter: no filtering
    inline constexpr filter_fn no_filter = [](std::uint64_t, std::uint64_t) noexcept {
        return true;
    };

    // Tag-based filter
    inline constexpr filter_fn tag_filter = [](std::uint64_t filter_tag, std::uint64_t event_tag) noexcept {
        return filter_tag == event_tag;
    };

    // Creates a type-erased callback from a typed callback
    template <typename T, typename F>
        requires traits::valid_typed_callback<F, T>
    [[nodiscard]] callback_fn make_callback(F&& func)
    {
        return [f = std::forward<F>(func)](const payload& p, const context& ctx) mutable {
            if constexpr (traits::typed_callback<F, T>) {
                f(const_cast<payload&>(p).as<T>(), ctx);
            } else {
                f(const_cast<payload&>(p).as<T>());
            }
        };
    }

    // Creates a type-erased callback from a payload callback
    template <typename F>
        requires traits::valid_payload_callback<F>
    [[nodiscard]] callback_fn make_callback(F&& func)
    {
        return [f = std::forward<F>(func)](const payload& p, const context& ctx) mutable {
            if constexpr (traits::payload_callback<F>) {
                f(p, ctx);
            } else {
                f(p);
            }
        };
    }

} // namespace detail

// ============================================================================
// Subscribe options
// ============================================================================
struct subscribe_options {
    std::int32_t priority { 0 }; // Higher priority executes first
    bool once { false }; // One-shot subscription
    std::uint32_t throttle_ms { 0 }; // Throttle interval in milliseconds
    std::uint64_t filter_tag { 0 }; // Filter tag for selective dispatch
    bool has_filter { false }; // Whether filter is enabled
    std::uint32_t executor_id { 0 }; // Executor ID (0 = sync execution)

    [[nodiscard]] constexpr subscribe_options& with_priority(std::int32_t p) noexcept
    {
        priority = p;
        return *this;
    }

    [[nodiscard]] constexpr subscribe_options& with_once() noexcept
    {
        once = true;
        return *this;
    }

    [[nodiscard]] constexpr subscribe_options& with_throttle(std::uint32_t ms) noexcept
    {
        throttle_ms = ms;
        return *this;
    }

    [[nodiscard]] constexpr subscribe_options& with_filter(std::uint64_t tag) noexcept
    {
        filter_tag = tag;
        has_filter = true;
        return *this;
    }

    [[nodiscard]] constexpr subscribe_options& with_executor(std::uint32_t id) noexcept
    {
        executor_id = id;
        return *this;
    }
};

// ============================================================================
// Subscriber entry (internal representation)
// ============================================================================
namespace detail {

    struct subscriber_entry {
        subscriber_id id { invalid_subscriber_id };
        callback_fn callback; // Type-erased callback
        filter_fn filter { no_filter }; // Filter function pointer
        std::uint64_t filter_tag { 0 }; // Tag for filter matching
        std::int32_t priority { 0 }; // Execution priority
        std::uint32_t throttle_ms { 0 }; // Throttle interval
        std::uint32_t executor_id { 0 }; // Target executor
        std::chrono::steady_clock::time_point last_invoked {}; // For throttling
        bool once { false }; // One-shot flag

        [[nodiscard]] bool matches(std::uint64_t event_tag) const noexcept
        {
            return filter(filter_tag, event_tag);
        }

        [[nodiscard]] bool operator<(const subscriber_entry& other) const noexcept
        {
            return priority > other.priority; // Higher priority first
        }
    };

} // namespace detail

// ============================================================================
// Subscription handle (RAII)
// ============================================================================
class [[nodiscard]] subscription {
    friend class bus;

    bus* bus_ { nullptr };
    topic_id topic_ { invalid_topic_id };
    detail::subscriber_id id_ { detail::invalid_subscriber_id };

    subscription(bus* b, topic_id t, detail::subscriber_id id) noexcept
        : bus_(b)
        , topic_(t)
        , id_(id)
    {
    }

public:
    subscription() noexcept = default;

    ~subscription() noexcept;

    subscription(const subscription&) = delete;
    subscription& operator=(const subscription&) = delete;

    subscription(subscription&& other) noexcept
        : bus_(std::exchange(other.bus_, nullptr))
        , topic_(std::exchange(other.topic_, invalid_topic_id))
        , id_(std::exchange(other.id_, detail::invalid_subscriber_id))
    {
    }

    subscription& operator=(subscription&& other) noexcept
    {
        if (this != &other) {
            unsubscribe();
            bus_ = std::exchange(other.bus_, nullptr);
            topic_ = std::exchange(other.topic_, invalid_topic_id);
            id_ = std::exchange(other.id_, detail::invalid_subscriber_id);
        }
        return *this;
    }

    /// Cancels this subscription.
    void unsubscribe() noexcept;

    /// Releases ownership without unsubscribing.
    void release() noexcept
    {
        bus_ = nullptr;
        topic_ = invalid_topic_id;
        id_ = detail::invalid_subscriber_id;
    }

    /// Returns true if subscription is active.
    [[nodiscard]] bool valid() const noexcept
    {
        return id_ != detail::invalid_subscriber_id;
    }

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return valid();
    }

    /// Returns the subscriber ID.
    [[nodiscard]] detail::subscriber_id id() const noexcept
    {
        return id_;
    }

    /// Returns the topic ID.
    [[nodiscard]] topic_id topic() const noexcept
    {
        return topic_;
    }
};

// ============================================================================
// Subscription group (manages multiple subscriptions)
// ============================================================================
class subscription_group {
public:
    subscription_group() = default;

    /// Adds a subscription to the group.
    subscription_group& add(subscription sub)
    {
        if (sub.valid()) {
            subs_.push_back(std::move(sub));
        }
        return *this;
    }

    /// Adds a subscription using += operator.
    subscription_group& operator+=(subscription sub)
    {
        return add(std::move(sub));
    }

    /// Unsubscribes all subscriptions and clears the group.
    void unsubscribe_all() noexcept
    {
        for (auto& sub : subs_) {
            sub.unsubscribe();
        }
        subs_.clear();
    }

    /// Releases ownership of all subscriptions without unsubscribing.
    void clear() noexcept
    {
        for (auto& sub : subs_) {
            sub.release();
        }
        subs_.clear();
    }

    /// Returns the number of subscriptions in the group.
    [[nodiscard]] std::size_t size() const noexcept
    {
        return subs_.size();
    }

    /// Returns true if the group is empty.
    [[nodiscard]] bool empty() const noexcept
    {
        return subs_.empty();
    }

    ~subscription_group()
    {
        unsubscribe_all();
    }

    subscription_group(const subscription_group&) = delete;
    subscription_group& operator=(const subscription_group&) = delete;
    subscription_group(subscription_group&&) = default;
    subscription_group& operator=(subscription_group&&) = default;

private:
    std::vector<subscription> subs_;
};

// ============================================================================
// Publish result
// ============================================================================
struct [[nodiscard]] publish_result {
    std::size_t subscriber_count { 0 }; // Total matching subscribers
    std::size_t delivered_count { 0 }; // Successfully delivered count
    bool has_error { false }; // Whether any callback threw
    bool deferred { false }; // Whether delivery was deferred
    bool deduplicated { false }; // Whether message was deduplicated

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return delivered_count > 0 || deferred;
    }
};

} // namespace cbuspp
