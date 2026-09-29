#pragma once

#include "exchange/core/types.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>

namespace exchange {

// Header-only to allow inlining without LTO on fill, level-total and sequence updates.
[[nodiscard]] constexpr std::optional<std::uint64_t> checked_add(std::uint64_t lhs,
                                                               std::uint64_t rhs) noexcept {
    if (rhs > std::numeric_limits<std::uint64_t>::max() - lhs) {
        return std::nullopt;
    }
    return lhs + rhs;
}

[[nodiscard]] constexpr std::optional<std::uint64_t> checked_subtract(std::uint64_t lhs,
                                                                    std::uint64_t rhs) noexcept {
    if (rhs > lhs) {
        return std::nullopt;
    }
    return lhs - rhs;
}

[[nodiscard]] constexpr std::optional<std::uint64_t> checked_multiply(std::uint64_t lhs,
                                                                    std::uint64_t rhs) noexcept {
    if (rhs != 0 && lhs > std::numeric_limits<std::uint64_t>::max() / rhs) {
        return std::nullopt;
    }
    return lhs * rhs;
}

// Positive tick prices only. A valid index is relative to an inclusive configured range.
[[nodiscard]] constexpr std::optional<std::size_t> tick_index(PriceTicks price, PriceTicks minimum,
                                                             PriceTicks maximum) noexcept {
    if (minimum.value() <= 0 || maximum < minimum || price < minimum || price > maximum) {
        return std::nullopt;
    }
    // Both endpoints are positive and ordered, so signed subtraction cannot overflow.
    const auto offset = static_cast<std::uint64_t>(price.value() - minimum.value());
    if (!std::in_range<std::size_t>(offset)) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(offset);
}

} // namespace exchange
