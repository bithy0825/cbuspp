#pragma once

// ============================================================================
// cbuspp::wildcard - Topic wildcard matching
// ============================================================================
// Supports:
// - * matches a single level
// - # matches any level (must be at the end)
// - Compile-time match validation
// ============================================================================

#include "topic.hpp"

#include <array>
#include <cstddef>
#include <string_view>

namespace cbuspp {
namespace wildcard {

    // ============================================================================
    // Topic segment splitter
    // ============================================================================
    template <std::size_t MaxSegments = 16>
    struct segments {
        std::array<std::string_view, MaxSegments> data {};
        std::size_t count { 0 };

        constexpr segments() noexcept = default;

        constexpr explicit segments(std::string_view topic, char delimiter = '/') noexcept
        {
            std::size_t start = 0;
            for (std::size_t i = 0; i <= topic.size() && count < MaxSegments; ++i) {
                if (i == topic.size() || topic[i] == delimiter) {
                    if (i > start) {
                        data[count++] = topic.substr(start, i - start);
                    }
                    start = i + 1;
                }
            }
        }

        [[nodiscard]] constexpr std::string_view operator[](std::size_t i) const noexcept
        {
            return data[i];
        }

        [[nodiscard]] constexpr std::size_t size() const noexcept
        {
            return count;
        }

        [[nodiscard]] constexpr bool empty() const noexcept
        {
            return count == 0;
        }
    };

    // ============================================================================
    // Match algorithm
    // ============================================================================

    // Check if the pattern contains wildcards
    [[nodiscard]] constexpr bool is_wildcard_pattern(std::string_view pattern) noexcept
    {
        for (char c : pattern) {
            if (c == '*' || c == '#') {
                return true;
            }
        }
        return false;
    }

    // Wildcard matching
    // * matches a single level
    // # matches any level (must be at the end)
    [[nodiscard]] constexpr bool matches(std::string_view pattern, std::string_view topic) noexcept
    {
        // Fast path: no wildcards
        if (!is_wildcard_pattern(pattern)) {
            return pattern == topic;
        }

        segments pat_segs(pattern);
        segments topic_segs(topic);

        std::size_t pi = 0, ti = 0;

        while (pi < pat_segs.size() && ti < topic_segs.size()) {
            auto pat_seg = pat_segs[pi];
            auto topic_seg = topic_segs[ti];

            if (pat_seg == "#") {
                // # matches all remaining levels
                return true;
            }

            if (pat_seg == "*") {
                // * matches a single level
                ++pi;
                ++ti;
                continue;
            }

            // Exact match
            if (pat_seg != topic_seg) {
                return false;
            }

            ++pi;
            ++ti;
        }

        // Check if both matched completely
        if (pi < pat_segs.size()) {
            // Pattern has remaining, check if it's #
            return pat_segs[pi] == "#" && pi + 1 == pat_segs.size();
        }

        return ti == topic_segs.size();
    }

    // ============================================================================
    // Compile-time matching
    // ============================================================================

    template <detail::fixed_string Pattern, detail::fixed_string Topic>
    inline constexpr bool matches_v = matches(Pattern.view(), Topic.view());

    // ============================================================================
    // Pattern types
    // ============================================================================
    enum class pattern_type : std::uint8_t {
        exact, // Exact match
        single_level, // Contains *
        multi_level // Contains #
    };

    [[nodiscard]] constexpr pattern_type detect_type(std::string_view pattern) noexcept
    {
        for (char c : pattern) {
            if (c == '#') {
                return pattern_type::multi_level;
            }
        }
        for (char c : pattern) {
            if (c == '*') {
                return pattern_type::single_level;
            }
        }
        return pattern_type::exact;
    }

    // ============================================================================
    // Wildcard Topic
    // ============================================================================
    class pattern {
    public:
        constexpr pattern() noexcept = default;

        constexpr explicit pattern(std::string_view pat) noexcept
            : pattern_(pat)
            , type_(detect_type(pat))
        {
        }

        // Check if the pattern matches the topic
        [[nodiscard]] constexpr bool operator()(std::string_view topic) const noexcept
        {
            return matches(pattern_, topic);
        }

        // Get the pattern string
        [[nodiscard]] constexpr std::string_view str() const noexcept
        {
            return pattern_;
        }

        // Get the pattern type
        [[nodiscard]] constexpr pattern_type type() const noexcept
        {
            return type_;
        }

        // Check if it is a wildcard pattern
        [[nodiscard]] constexpr bool is_wildcard() const noexcept
        {
            return type_ != pattern_type::exact;
        }

    private:
        std::string_view pattern_;
        pattern_type type_ { pattern_type::exact };
    };

    // ============================================================================
    // Compile-time pattern
    // ============================================================================
    template <detail::fixed_string Pattern>
    struct static_pattern {
        static constexpr std::string_view value = Pattern.view();
        static constexpr pattern_type type = detect_type(value);

        [[nodiscard]] static constexpr bool match(std::string_view topic) noexcept
        {
            return matches(value, topic);
        }

        [[nodiscard]] constexpr bool operator()(std::string_view topic) const noexcept
        {
            return match(topic);
        }
    };

    // ============================================================================
    // User-defined literal
    // ============================================================================
    template <detail::fixed_string Pattern>
    [[nodiscard]] consteval auto operator""_pattern() noexcept
    {
        return static_pattern<Pattern> {};
    }

} // namespace wildcard
} // namespace cbuspp
