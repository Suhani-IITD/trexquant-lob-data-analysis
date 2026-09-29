#pragma once

#include "exchange/core/types.hpp"

#include <vector>

namespace exchange {

struct OrderSnapshot {
    OrderId id;
    OrderGeneration generation;
    Quantity remaining;
    IngressSequence priority;
    bool operator==(const OrderSnapshot&) const = default;
};
struct LevelSnapshot {
    Side side;
    PriceTicks price;
    Quantity total;
    std::vector<OrderSnapshot> fifo;
    bool operator==(const LevelSnapshot&) const = default;
};
struct BookSnapshot {
    InstrumentId instrument;
    std::vector<LevelSnapshot> bids;
    std::vector<LevelSnapshot> asks;
    bool operator==(const BookSnapshot&) const = default;
};
struct EngineSnapshot {
    std::vector<BookSnapshot> books;
    bool operator==(const EngineSnapshot&) const = default;
};

// Aggregate observations own price/quantity values but omit per-order FIFO copies.
struct DepthLevelSnapshot {
    PriceTicks price;
    Quantity total;
    bool operator==(const DepthLevelSnapshot&) const = default;
};
struct BookDepthSnapshot {
    InstrumentId instrument;
    std::vector<DepthLevelSnapshot> bids;
    std::vector<DepthLevelSnapshot> asks;
    bool operator==(const BookDepthSnapshot&) const = default;
};
struct EngineDepthSnapshot {
    std::vector<BookDepthSnapshot> books;
    bool operator==(const EngineDepthSnapshot&) const = default;
};

} // namespace exchange
