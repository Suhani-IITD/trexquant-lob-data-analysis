#include "exchange/book/reference_book.hpp"

#include <utility>

namespace exchange {
namespace {
template <class Levels>
std::vector<LevelSnapshot> snapshot_levels(const Levels& levels, Side side) {
    std::vector<LevelSnapshot> result;
    result.reserve(levels.size());
    for (const auto& [price, level] : levels) {
        LevelSnapshot output{side, price, level.total, {}};
        output.fifo.reserve(level.fifo.size());
        for (const auto& order : level.fifo) {
            output.fifo.push_back({order.id, order.generation, order.remaining, order.priority});
        }
        result.push_back(std::move(output));
    }
    return result;
}
} // namespace

ReferenceBook::ReferenceBook(InstrumentConfig config) : config_(std::move(config)) {}

std::optional<PriceTicks> ReferenceBook::best_bid() const noexcept {
    if (bids_.empty()) {
        return std::nullopt;
    }
    return bids_.begin()->first;
}

std::optional<PriceTicks> ReferenceBook::best_ask() const noexcept {
    if (asks_.empty()) {
        return std::nullopt;
    }
    return asks_.begin()->first;
}

BookSnapshot ReferenceBook::snapshot() const {
    return {config_.id, snapshot_levels(bids_, Side::Buy), snapshot_levels(asks_, Side::Sell)};
}

BookDepthSnapshot ReferenceBook::depth_snapshot() const {
    const auto copy = [](const auto& levels) {
        std::vector<DepthLevelSnapshot> output;
        output.reserve(levels.size());
        for (const auto& [price, level] : levels) { output.push_back({price, level.total}); }
        return output;
    };
    return {config_.id, copy(bids_), copy(asks_)};
}

} // namespace exchange
