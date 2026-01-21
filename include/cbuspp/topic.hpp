#pragma once

#include <algorithm>
#include <array>
#include <compare>
#include <cstdint>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace cbuspp {

// ============================================================================
// Type Aliases (std style)
// ============================================================================
using topic_id = std::uint64_t;
inline constexpr topic_id invalid_topic_id = 0;

// ============================================================================
// Compile-time FNV-1a Hash
// ============================================================================
namespace detail {

    // FNV-1a 64-bit compile-time hash
    inline constexpr std::uint64_t fnv1a_basis = 14695981039346656037ULL;
    inline constexpr std::uint64_t fnv1a_prime = 1099511628211ULL;

    [[nodiscard]] constexpr std::uint64_t fnv1a_hash(std::string_view sv) noexcept
    {
        std::uint64_t hash = fnv1a_basis;
        for (char c : sv) {
            hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
            hash *= fnv1a_prime;
        }
        // Ensure hash is not 0 (0 is reserved for invalid ID)
        return hash == 0 ? 1 : hash;
    }

    // Compile-time string type for NTTP
    template <std::size_t N>
    struct fixed_string {
        std::array<char, N> data_ {};

        consteval fixed_string(const char (&str)[N]) noexcept
        {
            std::copy_n(str, N, data_.begin());
        }

        [[nodiscard]] consteval std::string_view view() const noexcept
        {
            return { data_.data(), N - 1 };
        }

        [[nodiscard]] consteval std::uint64_t hash() const noexcept
        {
            return fnv1a_hash(view());
        }

        [[nodiscard]] consteval bool operator==(const fixed_string&) const noexcept = default;
    };

    template <std::size_t N>
    fixed_string(const char (&)[N]) -> fixed_string<N>;

} // namespace detail

// ============================================================================
// Compile-time topic_id generation: Generate unique ID from string literal
// ============================================================================
template <detail::fixed_string Name>
struct topic_constant {
    static constexpr std::string_view name = Name.view();
    static constexpr topic_id value = Name.hash();

    [[nodiscard]] consteval operator topic_id() const noexcept { return value; }
};

// ============================================================================
// User-defined literal: Convert string to topic_id at compile-time
// Usage: "usr.login"_topic returns compile-time constant topic_id
// ============================================================================
template <detail::fixed_string Name>
[[nodiscard]] consteval topic_id operator""_topic() noexcept
{
    return topic_constant<Name>::value;
}

// ============================================================================
// Runtime hash (for dynamic strings, should be avoided when possible)
// ============================================================================
[[nodiscard]] inline topic_id runtime_topic_hash(std::string_view sv) noexcept
{
    return detail::fnv1a_hash(sv);
}

// ============================================================================
// Topic class
// ============================================================================
class topic {
public:
    constexpr topic() noexcept = default;

    constexpr explicit topic(topic_id id) noexcept
        : id_ { id }
    {
    }

    // Construct from compile-time constant
    template <detail::fixed_string Name>
    consteval topic(topic_constant<Name>) noexcept
        : id_ { topic_constant<Name>::value }
    {
    }

    [[nodiscard]] constexpr topic_id id() const noexcept
    {
        return id_;
    }

    [[nodiscard]] constexpr bool valid() const noexcept
    {
        return id_ != invalid_topic_id;
    }

    [[nodiscard]] constexpr explicit operator bool() const noexcept
    {
        return valid();
    }

    [[nodiscard]] constexpr explicit operator topic_id() const noexcept
    {
        return id_;
    }

    constexpr std::strong_ordering operator<=>(const topic&) const noexcept = default;
    constexpr bool operator==(const topic&) const noexcept = default;

private:
    topic_id id_ { invalid_topic_id };
};

// ============================================================================
// Topic description info
// ============================================================================
struct topic_info {
    topic_id id { invalid_topic_id };
    std::string_view name {};
    std::optional<std::string_view> description {};
};

// ============================================================================
// Topic Registry singleton
// ============================================================================
class topic_registry {
public:
    topic_registry(const topic_registry&) = delete;
    topic_registry& operator=(const topic_registry&) = delete;

    [[nodiscard]] static topic_registry& instance() noexcept
    {
        static topic_registry inst;
        return inst;
    }

    // Register topic, automatically assign ID (based on hash)
    // Returns the assigned topic_id
    [[nodiscard]] topic_id register_topic(
        std::string_view name,
        std::optional<std::string_view> desc = std::nullopt)
    {
        const topic_id id = detail::fnv1a_hash(name);

        std::unique_lock lock(mutex_);

        if (auto it = topics_.find(id); it != topics_.end()) {
            // Already exists, return directly (idempotent)
            return id;
        }

        topics_.emplace(id, topic_info { id, name, desc });
        return id;
    }

    // Register with user-specified ID (occupy specified ID)
    // Returns true if successful, false if ID is already occupied
    [[nodiscard]] bool register_topic_with_id(
        topic_id id,
        std::string_view name,
        std::optional<std::string_view> desc = std::nullopt)
    {
        if (id == invalid_topic_id) {
            return false;
        }

        std::unique_lock lock(mutex_);

        if (topics_.contains(id)) {
            return false;
        }

        topics_.emplace(id, topic_info { id, name, desc });
        return true;
    }

    // Query topic info
    [[nodiscard]] std::optional<topic_info> find(topic_id id) const
    {
        std::shared_lock lock(mutex_);

        if (auto it = topics_.find(id); it != topics_.end()) {
            return it->second;
        }
        return std::nullopt;
    }

    // Check if ID is registered
    [[nodiscard]] bool contains(topic_id id) const
    {
        std::shared_lock lock(mutex_);
        return topics_.contains(id);
    }

    // Get all registered topics
    [[nodiscard]] std::vector<topic_info> all_topics() const
    {
        std::shared_lock lock(mutex_);
        std::vector<topic_info> result;
        result.reserve(topics_.size());
        for (const auto& [id, info] : topics_) {
            result.push_back(info);
        }
        return result;
    }

private:
    topic_registry() = default;

    mutable std::shared_mutex mutex_;
    std::unordered_map<topic_id, topic_info> topics_;
};

// ============================================================================
// Compile-time Topic collision detection macros
// ============================================================================

// Used for compile-time topic collision detection
// Usage: CBUSPP_TOPIC_UNIQUE("topic1", "topic2", "topic3")
#define CBUSPP_TOPIC_UNIQUE_IMPL_2(a, b)                                              \
    static_assert(::cbuspp::detail::fnv1a_hash(a) != ::cbuspp::detail::fnv1a_hash(b), \
        "Topic hash collision detected: " a " and " b)

#define CBUSPP_TOPIC_UNIQUE_IMPL_3(a, b, c) \
    CBUSPP_TOPIC_UNIQUE_IMPL_2(a, b);       \
    CBUSPP_TOPIC_UNIQUE_IMPL_2(a, c);       \
    CBUSPP_TOPIC_UNIQUE_IMPL_2(b, c)

#define CBUSPP_TOPIC_UNIQUE_IMPL_4(a, b, c, d) \
    CBUSPP_TOPIC_UNIQUE_IMPL_3(a, b, c);       \
    CBUSPP_TOPIC_UNIQUE_IMPL_2(a, d);          \
    CBUSPP_TOPIC_UNIQUE_IMPL_2(b, d);          \
    CBUSPP_TOPIC_UNIQUE_IMPL_2(c, d)

// Define compile-time topic constant with optional collision detection
// Usage: inline constexpr auto my_topic = CBUSPP_TOPIC("usr.login");
#define CBUSPP_TOPIC(str) (::cbuspp::topic_constant<str> {})

} // namespace cbuspp
