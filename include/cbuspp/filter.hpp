#pragma once

// ============================================================================
// cbuspp::filter - Compile-time filters
// ============================================================================

#include "context.hpp"

#include <concepts>
#include <tuple>

namespace cbuspp {
namespace filter {

    // ============================================================================
    // Filter concept
    // ============================================================================
    template <typename F>
    concept predicate = requires(const F& f, const context& c) {
        { f(c) } -> std::convertible_to<bool>;
    };

    // ============================================================================
    // Basic filters
    // ============================================================================

    // Always pass
    struct always_pass {
        [[nodiscard]] constexpr bool operator()(const context&) const noexcept
        {
            return true;
        }
    };

    // Always reject
    struct always_reject {
        [[nodiscard]] constexpr bool operator()(const context&) const noexcept
        {
            return false;
        }
    };

    // ============================================================================
    // Priority filters
    // ============================================================================
    template <priority MinPriority>
    struct priority_at_least {
        [[nodiscard]] constexpr bool operator()(const context& ctx) const noexcept
        {
            return ctx.get<ctx::priority_tag>() >= MinPriority;
        }
    };

    template <priority MaxPriority>
    struct priority_at_most {
        [[nodiscard]] constexpr bool operator()(const context& ctx) const noexcept
        {
            return ctx.get<ctx::priority_tag>() <= MaxPriority;
        }
    };

    template <priority ExactPriority>
    struct priority_equals {
        [[nodiscard]] constexpr bool operator()(const context& ctx) const noexcept
        {
            return ctx.get<ctx::priority_tag>() == ExactPriority;
        }
    };

    // Runtime priority filter
    struct priority_filter {
        priority min_priority;

        [[nodiscard]] constexpr bool operator()(const context& ctx) const noexcept
        {
            return ctx.get<ctx::priority_tag>() >= min_priority;
        }
    };

    // ============================================================================
    // User filters
    // ============================================================================
    template <std::uint64_t UserId>
    struct user_equals {
        [[nodiscard]] constexpr bool operator()(const context& ctx) const noexcept
        {
            return ctx.get<ctx::user_id_tag>() == UserId;
        }
    };

    struct user_filter {
        std::uint64_t user_id;

        [[nodiscard]] constexpr bool operator()(const context& ctx) const noexcept
        {
            return ctx.get<ctx::user_id_tag>() == user_id;
        }
    };

    // ============================================================================
    // Tag filters
    // ============================================================================
    template <std::uint64_t Tag>
    struct tag_equals {
        [[nodiscard]] constexpr bool operator()(const context& ctx) const noexcept
        {
            return ctx.get<ctx::event_tag_tag>() == Tag;
        }
    };

    struct tag_filter {
        std::uint64_t tag;

        [[nodiscard]] constexpr bool operator()(const context& ctx) const noexcept
        {
            return ctx.get<ctx::event_tag_tag>() == tag;
        }
    };

    // ============================================================================
    // Field existence check
    // ============================================================================
    template <ctx::field_tag Tag>
    struct has_field {
        [[nodiscard]] constexpr bool operator()(const context& ctx) const noexcept
        {
            return ctx.has<Tag>();
        }
    };

    // ============================================================================
    // Combination filters
    // ============================================================================

    // Logical AND
    template <predicate... Filters>
    struct all_of {
        std::tuple<Filters...> filters;

        constexpr all_of() = default;

        explicit constexpr all_of(Filters... fs) noexcept
            : filters(std::move(fs)...)
        {
        }

        [[nodiscard]] constexpr bool operator()(const context& ctx) const noexcept
        {
            return std::apply([&ctx](const auto&... fs) {
                return (fs(ctx) && ...);
            },
                filters);
        }
    };

    // Logical OR
    template <predicate... Filters>
    struct any_of {
        std::tuple<Filters...> filters;

        constexpr any_of() = default;

        explicit constexpr any_of(Filters... fs) noexcept
            : filters(std::move(fs)...)
        {
        }

        [[nodiscard]] constexpr bool operator()(const context& ctx) const noexcept
        {
            return std::apply([&ctx](const auto&... fs) {
                return (fs(ctx) || ...);
            },
                filters);
        }
    };

    // Logical NOT
    template <predicate Filter>
    struct negate {
        Filter filter;

        constexpr negate() = default;

        explicit constexpr negate(Filter f) noexcept
            : filter(std::move(f))
        {
        }

        [[nodiscard]] constexpr bool operator()(const context& ctx) const noexcept
        {
            return !filter(ctx);
        }
    };

    // ============================================================================
    // Factory functions
    // ============================================================================

    template <predicate... Fs>
    [[nodiscard]] constexpr auto make_all_of(Fs... fs) noexcept
    {
        return all_of<Fs...>(std::move(fs)...);
    }

    template <predicate... Fs>
    [[nodiscard]] constexpr auto make_any_of(Fs... fs) noexcept
    {
        return any_of<Fs...>(std::move(fs)...);
    }

    template <predicate F>
    [[nodiscard]] constexpr auto make_negate(F f) noexcept
    {
        return negate<F>(std::move(f));
    }

    // ============================================================================
    // Operator overloads
    // ============================================================================

    template <predicate L, predicate R>
    [[nodiscard]] constexpr auto operator&&(L lhs, R rhs) noexcept
    {
        return all_of<L, R>(std::move(lhs), std::move(rhs));
    }

    template <predicate L, predicate R>
    [[nodiscard]] constexpr auto operator||(L lhs, R rhs) noexcept
    {
        return any_of<L, R>(std::move(lhs), std::move(rhs));
    }

    template <predicate F>
    [[nodiscard]] constexpr auto operator!(F f) noexcept
    {
        return negate<F>(std::move(f));
    }

    // ============================================================================
    // Convenience aliases
    // ============================================================================
    using high_priority = priority_at_least<priority::high>;
    using critical_only = priority_equals<priority::critical>;
    using low_priority = priority_at_most<priority::low>;

} // namespace filter
} // namespace cbuspp
