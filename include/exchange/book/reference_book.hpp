#pragma once

#include "exchange/core/config.hpp"
#include "exchange/core/snapshot.hpp"

#include <functional>
#include <list>
#include <map>
#include <optional>

namespace exchange {

struct RestingOrder {
    OrderId id;
    OrderGeneration generation;
    SessionId owner;
    Quantity remaining;
    IngressSequence priority;
};
struct PriceLevel {
    PriceTicks price;
    Quantity total;
    std::list<RestingOrder> fifo;
};
using BidLevels = std::map<PriceTicks, PriceLevel, std::greater<PriceTicks>>;
using AskLevels = std::map<PriceTicks, PriceLevel, std::less<PriceTicks>>;

class ReferenceBook {
public:
    explicit ReferenceBook(InstrumentConfig config);
    ReferenceBook(const ReferenceBook&) = delete;
    ReferenceBook& operator=(const ReferenceBook&) = delete;
    ReferenceBook(ReferenceBook&&) = delete;
    ReferenceBook& operator=(ReferenceBook&&) = delete;

    [[nodiscard]] std::optional<PriceTicks> best_bid() const noexcept;
    [[nodiscard]] std::optional<PriceTicks> best_ask() const noexcept;
    [[nodiscard]] BookSnapshot snapshot() const;
    [[nodiscard]] BookDepthSnapshot depth_snapshot() const;

private:
    friend class ReferenceEngine;
    InstrumentConfig config_;
    BidLevels bids_;
    AskLevels asks_;
};

} // namespace exchange
