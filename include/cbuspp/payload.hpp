#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

namespace cbuspp {

namespace traits {

    enum class storage_policy : std::uint8_t {
        trivial,
        borrowed,
        in_situ,
        heap
    };

    inline constexpr std::size_t trivial_threshold = 16;
    inline constexpr std::size_t in_situ_threshold = 64;
    inline constexpr std::size_t storage_alignment = alignof(std::max_align_t);

    template <typename T>
    inline constexpr bool is_span_v = false;

    template <typename E, std::size_t N>
    inline constexpr bool is_span_v<std::span<E, N>> = true;

    template <typename T>
    inline constexpr bool is_ref_wrapper_v = false;

    template <typename U>
    inline constexpr bool is_ref_wrapper_v<std::reference_wrapper<U>> = true;

    template <typename T>
    concept borrowed = std::is_pointer_v<T> || std::same_as<T, std::string_view> || is_span_v<T> || is_ref_wrapper_v<T>;

    template <typename T>
    consteval storage_policy deduce_policy() noexcept
    {
        using raw = std::remove_cvref_t<T>;

        static_assert(std::is_move_constructible_v<raw>,
            "Non-movable types cannot be stored in payload");

        if constexpr (borrowed<raw>) {
            return storage_policy::borrowed;
        } else if constexpr (std::is_trivially_copyable_v<raw> && std::is_trivially_destructible_v<raw> && sizeof(raw) <= trivial_threshold && alignof(raw) <= storage_alignment) {
            return storage_policy::trivial;
        } else if constexpr (std::is_nothrow_move_constructible_v<raw> && sizeof(raw) <= in_situ_threshold && alignof(raw) <= storage_alignment) {
            return storage_policy::in_situ;
        } else {
            return storage_policy::heap;
        }
    }

    template <typename T>
    inline constexpr storage_policy policy_v = deduce_policy<T>();

} // namespace traits

namespace detail {

    using destroy_fn = void (*)(void*) noexcept;
    using move_fn = void (*)(void* dst, void* src) noexcept;

    template <typename T, traits::storage_policy Policy>
    consteval destroy_fn make_destroyer() noexcept
    {
        using raw = std::remove_cvref_t<T>;

        if constexpr (Policy == traits::storage_policy::heap) {
            return [](void* ptr) noexcept {
                delete *static_cast<raw**>(ptr);
            };
        } else if constexpr (Policy == traits::storage_policy::in_situ) {
            if constexpr (std::is_trivially_destructible_v<raw>) {
                return nullptr;
            } else {
                return [](void* ptr) noexcept {
                    std::launder(static_cast<raw*>(ptr))->~raw();
                };
            }
        } else {
            return nullptr;
        }
    }

    template <typename T, traits::storage_policy Policy>
    consteval move_fn make_mover() noexcept
    {
        using raw = std::remove_cvref_t<T>;

        if constexpr (Policy == traits::storage_policy::heap) {
            return [](void* dst, void* src) noexcept {
                *static_cast<raw**>(dst) = *static_cast<raw**>(src);
                *static_cast<raw**>(src) = nullptr;
            };
        } else if constexpr (Policy == traits::storage_policy::in_situ) {
            return [](void* dst, void* src) noexcept {
                ::new (dst) raw(std::move(*std::launder(static_cast<raw*>(src))));
                if constexpr (!std::is_trivially_destructible_v<raw>) {
                    std::launder(static_cast<raw*>(src))->~raw();
                }
            };
        } else {
            return nullptr;
        }
    }

} // namespace detail

class payload {
public:
    constexpr payload() noexcept = default;

    template <typename T>
        requires(!std::same_as<std::remove_cvref_t<T>, payload>)
    explicit payload(T&& value)
        : destroy_ { detail::make_destroyer<T, traits::policy_v<T>>() }
        , move_ { detail::make_mover<T, traits::policy_v<T>>() }
        , has_value_ { true }
    {
        emplace_impl<std::remove_cvref_t<T>>(std::forward<T>(value));
    }

    template <typename T, typename... Args>
        requires std::constructible_from<T, Args...>
    explicit payload(std::in_place_type_t<T>, Args&&... args)
        : destroy_ { detail::make_destroyer<T, traits::policy_v<T>>() }
        , move_ { detail::make_mover<T, traits::policy_v<T>>() }
        , has_value_ { true }
    {
        emplace_impl<T>(std::forward<Args>(args)...);
    }

    ~payload() { reset(); }

    payload(const payload&) = delete;
    payload& operator=(const payload&) = delete;

    payload(payload&& other) noexcept
        : destroy_ { std::exchange(other.destroy_, nullptr) }
        , move_ { std::exchange(other.move_, nullptr) }
        , has_value_ { std::exchange(other.has_value_, false) }
    {
        if (move_) {
            move_(storage_, other.storage_);
        } else {
            std::memcpy(storage_, other.storage_, sizeof(storage_));
        }
    }

    payload& operator=(payload&& other) noexcept
    {
        if (this != &other) [[likely]] {
            reset();
            destroy_ = std::exchange(other.destroy_, nullptr);
            move_ = std::exchange(other.move_, nullptr);
            has_value_ = std::exchange(other.has_value_, false);
            if (move_) {
                move_(storage_, other.storage_);
            } else {
                std::memcpy(storage_, other.storage_, sizeof(storage_));
            }
        }
        return *this;
    }

    template <typename T>
    [[nodiscard]] T& as() noexcept
    {
        return *ptr_to<T>();
    }

    template <typename T>
    [[nodiscard]] const T& as() const noexcept
    {
        return *ptr_to<T>();
    }

    [[nodiscard]] bool empty() const noexcept
    {
        return !has_value_;
    }

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return has_value_;
    }

    void reset() noexcept
    {
        if (has_value_) {
            if (destroy_) {
                destroy_(storage_);
                destroy_ = nullptr;
            }
            move_ = nullptr;
            has_value_ = false;
        }
    }

    template <typename T, typename... Args>
        requires std::constructible_from<T, Args...>
    T& emplace(Args&&... args)
    {
        reset();
        destroy_ = detail::make_destroyer<T, traits::policy_v<T>>();
        move_ = detail::make_mover<T, traits::policy_v<T>>();
        emplace_impl<T>(std::forward<Args>(args)...);
        has_value_ = true;
        return as<T>();
    }

private:
    template <typename T, typename... Args>
    void emplace_impl(Args&&... args)
    {
        constexpr auto policy = traits::policy_v<T>;

        if constexpr (policy == traits::storage_policy::heap) {
            *reinterpret_cast<T**>(storage_) = new T(std::forward<Args>(args)...);
        } else {
            ::new (static_cast<void*>(storage_)) T(std::forward<Args>(args)...);
        }
    }

    template <typename T>
    [[nodiscard]] T* ptr_to() noexcept
    {
        constexpr auto policy = traits::policy_v<T>;

        if constexpr (policy == traits::storage_policy::heap) {
            return *std::launder(reinterpret_cast<T**>(storage_));
        } else {
            return std::launder(reinterpret_cast<T*>(storage_));
        }
    }

    template <typename T>
    [[nodiscard]] const T* ptr_to() const noexcept
    {
        constexpr auto policy = traits::policy_v<T>;

        if constexpr (policy == traits::storage_policy::heap) {
            return *std::launder(reinterpret_cast<T* const*>(storage_));
        } else {
            return std::launder(reinterpret_cast<const T*>(storage_));
        }
    }

    alignas(traits::storage_alignment) std::byte storage_[traits::in_situ_threshold] {};
    detail::destroy_fn destroy_ { nullptr };
    detail::move_fn move_ { nullptr };
    bool has_value_ { false };
};

} // namespace cbuspp
