#pragma once

#include <compare>
#include <cstdint>

namespace exchange {

// Values preserve raw input for later business validation; construction is not validation.
template <typename Tag, typename Representation>
class StrongValue {
public:
    constexpr StrongValue() noexcept = default;
    explicit constexpr StrongValue(Representation value) noexcept : value_(value) {}

    [[nodiscard]] constexpr Representation value() const noexcept { return value_; }
    auto operator<=>(const StrongValue&) const = default;

private:
    Representation value_{};
};

using OrderId = StrongValue<struct OrderIdTag, std::uint64_t>;
using InstrumentId = StrongValue<struct InstrumentIdTag, std::uint32_t>;
using PriceTicks = StrongValue<struct PriceTicksTag, std::int64_t>;
using Quantity = StrongValue<struct QuantityTag, std::uint64_t>;
using IngressSequence = StrongValue<struct IngressSequenceTag, std::uint64_t>;
using MarketSequence = StrongValue<struct MarketSequenceTag, std::uint64_t>;
using OrderGeneration = StrongValue<struct OrderGenerationTag, std::uint64_t>;
using EpochId = StrongValue<struct EpochIdTag, std::uint64_t>;
using SessionId = StrongValue<struct SessionIdTag, std::uint64_t>;
using CorrelationId = StrongValue<struct CorrelationIdTag, std::uint64_t>;

enum class Side : std::uint8_t { Buy = 1, Sell = 2 };
enum class TimeInForce : std::uint8_t { GTC = 1, IOC = 2 };
enum class OrderType : std::uint8_t { Limit = 1, Market = 2 };

} // namespace exchange
