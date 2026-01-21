#pragma once

#include <bit>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <source_location>
#include <utility>

namespace cbuspp {

// ============================================================================
// Publish mode and priority enumerations
// ============================================================================
enum class publish_mode : std::uint8_t {
    sync = 0,
    async = 1,
    deferred = 2
};

enum class priority : std::uint8_t {
    lowest = 0,
    low = 25,
    normal = 50,
    high = 75,
    highest = 100,
    critical = 255
};

// ============================================================================
// Context field tag definitions
// ============================================================================
namespace ctx {

    // Field index (bit flag position)
    enum class field_index : std::uint16_t {
        trace_id = 0,
        timestamp = 1,
        deadline = 2, // Also used for delayed publish target time
        user_id = 3,
        session_id = 4,
        priority = 5,
        mode = 6,
        retry_count = 7,
        span_id = 8,
        parent_id = 9,
        correlation = 10,
        source_line = 11,
        // Bus advanced feature fields
        throttle_ms = 12,
        dedup_ms = 13,
        event_tag = 14,
        // Reserved for extension...
        max_fields = 16
    };

    // Bit mask type
    using field_mask = std::uint16_t;

    [[nodiscard]] consteval field_mask to_mask(field_index idx) noexcept
    {
        return static_cast<field_mask>(1u << static_cast<std::uint16_t>(idx));
    }

    // ========================================================================
    // Field type tags (for put<Tag>(value))
    // ========================================================================

    struct trace_id_tag {
        using value_type = std::uint64_t;
        static constexpr field_index index = field_index::trace_id;
        static constexpr value_type default_value = 0;
    };

    struct timestamp_tag {
        using value_type = std::chrono::steady_clock::time_point;
        static constexpr field_index index = field_index::timestamp;
        static constexpr value_type default_value {};
    };

    struct deadline_tag {
        using value_type = std::chrono::steady_clock::time_point;
        static constexpr field_index index = field_index::deadline;
        static constexpr value_type default_value {};
    };

    struct user_id_tag {
        using value_type = std::uint64_t;
        static constexpr field_index index = field_index::user_id;
        static constexpr value_type default_value = 0;
    };

    struct session_id_tag {
        using value_type = std::uint64_t;
        static constexpr field_index index = field_index::session_id;
        static constexpr value_type default_value = 0;
    };

    struct priority_tag {
        using value_type = cbuspp::priority;
        static constexpr field_index index = field_index::priority;
        static constexpr value_type default_value = cbuspp::priority::normal;
    };

    struct mode_tag {
        using value_type = publish_mode;
        static constexpr field_index index = field_index::mode;
        static constexpr value_type default_value = publish_mode::sync;
    };

    struct retry_count_tag {
        using value_type = std::uint8_t;
        static constexpr field_index index = field_index::retry_count;
        static constexpr value_type default_value = 0;
    };

    struct span_id_tag {
        using value_type = std::uint64_t;
        static constexpr field_index index = field_index::span_id;
        static constexpr value_type default_value = 0;
    };

    struct parent_id_tag {
        using value_type = std::uint64_t;
        static constexpr field_index index = field_index::parent_id;
        static constexpr value_type default_value = 0;
    };

    struct correlation_tag {
        using value_type = std::uint64_t;
        static constexpr field_index index = field_index::correlation;
        static constexpr value_type default_value = 0;
    };

    struct source_line_tag {
        using value_type = std::uint32_t;
        static constexpr field_index index = field_index::source_line;
        static constexpr value_type default_value = 0;
    };

    // ========================================================================
    // Bus advanced feature field tags
    // ========================================================================

    // Throttle window (milliseconds)
    struct throttle_ms_tag {
        using value_type = std::uint32_t;
        static constexpr field_index index = field_index::throttle_ms;
        static constexpr value_type default_value = 0;
    };

    // Deduplication window (milliseconds)
    struct dedup_ms_tag {
        using value_type = std::uint32_t;
        static constexpr field_index index = field_index::dedup_ms;
        static constexpr value_type default_value = 0;
    };

    // Event tag (for filtering)
    struct event_tag_tag {
        using value_type = std::uint64_t;
        static constexpr field_index index = field_index::event_tag;
        static constexpr value_type default_value = 0;
    };

    // Executor index (for specifying callback execution thread)
    struct executor_hint_tag {
        using value_type = std::uint32_t;
        static constexpr field_index index = field_index(15);
        static constexpr value_type default_value = 0;
    };

    // Field tag concept
    template <typename Tag>
    concept field_tag = requires {
        typename Tag::value_type;
        { Tag::index } -> std::convertible_to<field_index>;
        { Tag::default_value } -> std::convertible_to<typename Tag::value_type>;
    };

} // namespace ctx

// ============================================================================
// Compact optional field storage
// ============================================================================
namespace detail {

    // Maximum alignment
    inline constexpr std::size_t context_storage_align = alignof(std::max_align_t);

    // ========================================================================
    // Compile-time field layout calculation
    // ========================================================================

    // Compile-time table of all field types and their indices
    template <std::size_t Idx>
    struct field_type_at;

#define CBUSPP_FIELD_TYPE(IDX, TYPE) \
    template <>                      \
    struct field_type_at<IDX> {      \
        using type = TYPE;           \
    }

    CBUSPP_FIELD_TYPE(0, std::uint64_t); // trace_id
    CBUSPP_FIELD_TYPE(1, std::chrono::steady_clock::time_point); // timestamp
    CBUSPP_FIELD_TYPE(2, std::chrono::steady_clock::time_point); // deadline
    CBUSPP_FIELD_TYPE(3, std::uint64_t); // user_id
    CBUSPP_FIELD_TYPE(4, std::uint64_t); // session_id
    CBUSPP_FIELD_TYPE(5, cbuspp::priority); // priority
    CBUSPP_FIELD_TYPE(6, publish_mode); // mode
    CBUSPP_FIELD_TYPE(7, std::uint8_t); // retry_count
    CBUSPP_FIELD_TYPE(8, std::uint64_t); // span_id
    CBUSPP_FIELD_TYPE(9, std::uint64_t); // parent_id
    CBUSPP_FIELD_TYPE(10, std::uint64_t); // correlation
    CBUSPP_FIELD_TYPE(11, std::uint32_t); // source_line
    CBUSPP_FIELD_TYPE(12, std::uint32_t); // throttle_ms
    CBUSPP_FIELD_TYPE(13, std::uint32_t); // dedup_ms
    CBUSPP_FIELD_TYPE(14, std::uint64_t); // event_tag
    CBUSPP_FIELD_TYPE(15, std::uint32_t); // executor_hint

#undef CBUSPP_FIELD_TYPE

    inline constexpr std::size_t max_field_count = 16;

    // Alignment helper
    consteval std::size_t align_up(std::size_t offset, std::size_t alignment) noexcept
    {
        return (offset + alignment - 1) & ~(alignment - 1);
    }

    // Compile-time calculation of each field's offset
    template <std::size_t Idx>
    consteval std::size_t compute_field_offset() noexcept
    {
        if constexpr (Idx == 0) {
            return 0;
        } else {
            using prev_type = typename field_type_at<Idx - 1>::type;
            using curr_type = typename field_type_at<Idx>::type;
            std::size_t prev_end = compute_field_offset<Idx - 1>() + sizeof(prev_type);
            return align_up(prev_end, alignof(curr_type));
        }
    }

    // Calculate total storage size (last field end position + alignment)
    consteval std::size_t compute_storage_size() noexcept
    {
        constexpr std::size_t last_idx = 15; // executor_hint
        using last_type = typename field_type_at<last_idx>::type;
        std::size_t end = compute_field_offset<last_idx>() + sizeof(last_type);
        return align_up(end, context_storage_align);
    }

    inline constexpr std::size_t context_storage_size = compute_storage_size();

    // Get offset by index
    template <std::size_t Idx>
    inline constexpr std::size_t field_offset_at = compute_field_offset<Idx>();

    // Get offset by tag
    template <ctx::field_tag Tag>
    inline constexpr std::size_t field_offset_v = field_offset_at<static_cast<std::size_t>(Tag::index)>;

} // namespace detail

// ============================================================================
// Context class: on-demand allocation, zero-overhead
// ============================================================================
class context {
public:
    // Default constructor: no fields set, no memory allocation (except mask)
    constexpr context() noexcept = default;

    // Aggregate initialization style construction
    struct init_data {
        std::optional<std::uint64_t> trace_id {};
        std::optional<std::chrono::steady_clock::time_point> timestamp {};
        std::optional<std::chrono::steady_clock::time_point> deadline {};
        std::optional<std::uint64_t> user_id {};
        std::optional<std::uint64_t> session_id {};
        std::optional<priority> priority_val {};
        std::optional<publish_mode> mode {};
        std::optional<std::uint8_t> retry_count {};
        std::optional<std::uint64_t> span_id {};
        std::optional<std::uint64_t> parent_id {};
        std::optional<std::uint64_t> correlation {};
        std::optional<std::uint32_t> source_line {};
    };

    // Construct from init_data (for aggregate initialization)
    constexpr explicit context(const init_data& data) noexcept
    {
        if (data.trace_id)
            set_field<ctx::trace_id_tag>(*data.trace_id);
        if (data.timestamp)
            set_field<ctx::timestamp_tag>(*data.timestamp);
        if (data.deadline)
            set_field<ctx::deadline_tag>(*data.deadline);
        if (data.user_id)
            set_field<ctx::user_id_tag>(*data.user_id);
        if (data.session_id)
            set_field<ctx::session_id_tag>(*data.session_id);
        if (data.priority_val)
            set_field<ctx::priority_tag>(*data.priority_val);
        if (data.mode)
            set_field<ctx::mode_tag>(*data.mode);
        if (data.retry_count)
            set_field<ctx::retry_count_tag>(*data.retry_count);
        if (data.span_id)
            set_field<ctx::span_id_tag>(*data.span_id);
        if (data.parent_id)
            set_field<ctx::parent_id_tag>(*data.parent_id);
        if (data.correlation)
            set_field<ctx::correlation_tag>(*data.correlation);
        if (data.source_line)
            set_field<ctx::source_line_tag>(*data.source_line);
    }

    // Copy and move
    context(const context& other) noexcept
        : mask_(other.mask_)
    {
        if (mask_ != 0) {
            std::memcpy(storage_, other.storage_, sizeof(storage_));
        }
    }

    context(context&& other) noexcept
        : mask_(other.mask_)
    {
        if (mask_ != 0) {
            std::memcpy(storage_, other.storage_, sizeof(storage_));
            other.mask_ = 0;
        }
    }

    context& operator=(const context& other) noexcept
    {
        if (this != &other) {
            mask_ = other.mask_;
            if (mask_ != 0) {
                std::memcpy(storage_, other.storage_, sizeof(storage_));
            }
        }
        return *this;
    }

    context& operator=(context&& other) noexcept
    {
        if (this != &other) {
            mask_ = other.mask_;
            if (mask_ != 0) {
                std::memcpy(storage_, other.storage_, sizeof(storage_));
                other.mask_ = 0;
            }
        }
        return *this;
    }

    ~context() = default;

    // ========================================================================
    // Chainable put<Tag>(value)
    // ========================================================================
    template <ctx::field_tag Tag>
    [[nodiscard]] constexpr context& put(typename Tag::value_type value) & noexcept
    {
        set_field<Tag>(value);
        return *this;
    }

    template <ctx::field_tag Tag>
    [[nodiscard]] constexpr context&& put(typename Tag::value_type value) && noexcept
    {
        set_field<Tag>(value);
        return std::move(*this);
    }

    // Convenience: auto-fill current timestamp
    [[nodiscard]] context& put_timestamp_now() & noexcept
    {
        set_field<ctx::timestamp_tag>(std::chrono::steady_clock::now());
        return *this;
    }

    [[nodiscard]] context&& put_timestamp_now() && noexcept
    {
        set_field<ctx::timestamp_tag>(std::chrono::steady_clock::now());
        return std::move(*this);
    }

    // Convenience: fill source location
    [[nodiscard]] context& put_source(
        std::source_location loc = std::source_location::current()) & noexcept
    {
        set_field<ctx::source_line_tag>(static_cast<std::uint32_t>(loc.line()));
        return *this;
    }

    [[nodiscard]] context&& put_source(
        std::source_location loc = std::source_location::current()) && noexcept
    {
        set_field<ctx::source_line_tag>(static_cast<std::uint32_t>(loc.line()));
        return std::move(*this);
    }

    // ========================================================================
    // Get field value
    // ========================================================================
    template <ctx::field_tag Tag>
    [[nodiscard]] constexpr typename Tag::value_type get() const noexcept
    {
        if (has<Tag>()) {
            return get_field<Tag>();
        }
        return Tag::default_value;
    }

    template <ctx::field_tag Tag>
    [[nodiscard]] constexpr std::optional<typename Tag::value_type> get_optional() const noexcept
    {
        if (has<Tag>()) {
            return get_field<Tag>();
        }
        return std::nullopt;
    }

    // ========================================================================
    // Check if field is set
    // ========================================================================
    template <ctx::field_tag Tag>
    [[nodiscard]] constexpr bool has() const noexcept
    {
        return (mask_ & ctx::to_mask(Tag::index)) != 0;
    }

    // Check if completely empty
    [[nodiscard]] constexpr bool empty() const noexcept
    {
        return mask_ == 0;
    }

    // Get number of set fields
    [[nodiscard]] constexpr std::size_t field_count() const noexcept
    {
        return static_cast<std::size_t>(std::popcount(mask_));
    }

    // Get bit mask (for debugging)
    [[nodiscard]] constexpr ctx::field_mask mask() const noexcept
    {
        return mask_;
    }

private:
    template <ctx::field_tag Tag>
    constexpr void set_field(typename Tag::value_type value) noexcept
    {
        constexpr std::size_t offset = detail::field_offset_v<Tag>;
        static_assert(offset + sizeof(typename Tag::value_type) <= sizeof(storage_),
            "Field offset exceeds storage size");

        std::memcpy(storage_ + offset, &value, sizeof(value));
        mask_ |= ctx::to_mask(Tag::index);
    }

    template <ctx::field_tag Tag>
    [[nodiscard]] constexpr typename Tag::value_type get_field() const noexcept
    {
        constexpr std::size_t offset = detail::field_offset_v<Tag>;
        typename Tag::value_type value {};
        std::memcpy(&value, storage_ + offset, sizeof(value));
        return value;
    }

    // Bit mask: marks which fields are set
    ctx::field_mask mask_ { 0 };

    // Compact storage: only used when fields are set
    alignas(detail::context_storage_align)
        std::byte storage_[detail::context_storage_size] {};
};

// ============================================================================
// Context factory function (for aggregate initialization style)
// ============================================================================
[[nodiscard]] inline constexpr context make_context(context::init_data data = {}) noexcept
{
    return context { data };
}

// ============================================================================
// Convenience aliases
// ============================================================================
namespace ctx {
    // Export tag instances for more concise syntax
    inline constexpr trace_id_tag trace_id {};
    inline constexpr timestamp_tag timestamp {};
    inline constexpr deadline_tag deadline {}; // Also used for delayed publish
    inline constexpr user_id_tag user_id {};
    inline constexpr session_id_tag session_id {};
    inline constexpr priority_tag priority {};
    inline constexpr mode_tag mode {};
    inline constexpr retry_count_tag retry_count {};
    inline constexpr span_id_tag span_id {};
    inline constexpr parent_id_tag parent_id {};
    inline constexpr correlation_tag correlation {};
    inline constexpr source_line_tag source_line {};
    // Bus advanced feature fields
    inline constexpr throttle_ms_tag throttle_ms {};
    inline constexpr dedup_ms_tag dedup_ms {};
    inline constexpr event_tag_tag event_tag {};
    inline constexpr executor_hint_tag executor_hint {};
} // namespace ctx

} // namespace cbuspp
