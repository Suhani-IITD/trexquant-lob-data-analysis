#include "exchange/core/checked_arithmetic.hpp"

#include <limits>
#include <utility>

namespace exchange {

std::optional<std::uint64_t> checked_add(std::uint64_t lhs, std::uint64_t rhs) noexcept {
    if (rhs > std::numeric_limits<std::uint64_t>::max() - lhs) {
        return std::nullopt;
    }
    return lhs + rhs;
}

std::optional<std::uint64_t> checked_subtract(std::uint64_t lhs, std::uint64_t rhs) noexcept {
    if (rhs > lhs) {
        return std::nullopt;
    }
    return lhs - rhs;
}

std::optional<std::uint64_t> checked_multiply(std::uint64_t lhs, std::uint64_t rhs) noexcept {
    if (rhs != 0 && lhs > std::numeric_limits<std::uint64_t>::max() / rhs) {
        return std::nullopt;
    }
    return lhs * rhs;
}

std::optional<std::size_t> tick_index(PriceTicks price, PriceTicks minimum,
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
