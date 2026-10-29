// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <cstdint>
#include <functional>

// Strong typedefs for identifiers that are otherwise all uint64_t and thus
// trivially interchangeable at call sites.
//
// Rule: a value of one ID type is never implicitly convertible to another,
// and never implicitly convertible to/from its raw representation.

namespace duckemu {

namespace detail {

template <typename Tag, typename Rep>
class StrongId {
public:
    using rep_type = Rep;

    constexpr StrongId() noexcept = default;
    constexpr explicit StrongId(Rep value) noexcept : value_(value) {}

    [[nodiscard]] constexpr Rep value() const noexcept { return value_; }
    [[nodiscard]] constexpr bool is_null() const noexcept { return value_ == Rep{0}; }

    friend constexpr bool operator==(StrongId lhs, StrongId rhs) noexcept {
        return lhs.value_ == rhs.value_;
    }
    friend constexpr bool operator!=(StrongId lhs, StrongId rhs) noexcept {
        return lhs.value_ != rhs.value_;
    }
    friend constexpr bool operator<(StrongId lhs, StrongId rhs) noexcept {
        return lhs.value_ < rhs.value_;
    }
    friend constexpr bool operator<=(StrongId lhs, StrongId rhs) noexcept {
        return lhs.value_ <= rhs.value_;
    }
    friend constexpr bool operator>(StrongId lhs, StrongId rhs) noexcept {
        return lhs.value_ > rhs.value_;
    }
    friend constexpr bool operator>=(StrongId lhs, StrongId rhs) noexcept {
        return lhs.value_ >= rhs.value_;
    }

    // GuestVA arithmetic: address + byte offset.
    friend constexpr StrongId operator+(StrongId id, Rep offset) noexcept {
        return StrongId{static_cast<Rep>(id.value_ + offset)};
    }
    friend constexpr StrongId operator-(StrongId id, Rep offset) noexcept {
        return StrongId{static_cast<Rep>(id.value_ - offset)};
    }
    friend constexpr Rep operator-(StrongId lhs, StrongId rhs) noexcept {
        return static_cast<Rep>(lhs.value_ - rhs.value_);
    }

private:
    Rep value_{};
};

} // namespace detail

struct GuestVATag;
struct SignerIdTag;
struct RuntimeKeyTag;
struct RuntimeThreadIdTag;

// Guest virtual address (what the ARM64 code sees).
using GuestVA = detail::StrongId<GuestVATag, std::uint64_t>;
// Identifier of a signer configuration.
using SignerId = detail::StrongId<SignerIdTag, std::uint64_t>;
// Opaque key selecting a RuntimeInstance / worker affinity.
using RuntimeKey = detail::StrongId<RuntimeKeyTag, std::uint64_t>;
// Identifier of a guest thread inside one RuntimeInstance.
using RuntimeThreadId = detail::StrongId<RuntimeThreadIdTag, std::uint32_t>;

} // namespace duckemu

namespace std {

template <typename Tag, typename Rep>
struct hash<duckemu::detail::StrongId<Tag, Rep>> {
    size_t operator()(duckemu::detail::StrongId<Tag, Rep> id) const noexcept {
        return std::hash<Rep>{}(id.value());
    }
};

} // namespace std
