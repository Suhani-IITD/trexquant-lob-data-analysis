#pragma once

#include "exchange/core/types.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace exchange {

[[nodiscard]] std::optional<std::uint64_t> checked_add(std::uint64_t lhs,
                                                     std::uint64_t rhs) noexcept;
[[nodiscard]] std::optional<std::uint64_t> checked_subtract(std::uint64_t lhs,
                                                          std::uint64_t rhs) noexcept;
[[nodiscard]] std::optional<std::uint64_t> checked_multiply(std::uint64_t lhs,
                                                          std::uint64_t rhs) noexcept;

// Positive tick prices only. A valid index is relative to an inclusive configured range.
[[nodiscard]] std::optional<std::size_t> tick_index(PriceTicks price, PriceTicks minimum,
                                                   PriceTicks maximum) noexcept;

} // namespace exchange
