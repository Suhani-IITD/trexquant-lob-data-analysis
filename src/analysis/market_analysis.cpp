#include "exchange/analysis/market_analysis.hpp"
#include "exchange/core/checked_arithmetic.hpp"
#include <algorithm>
#include <stdexcept>

namespace exchange {
namespace {
std::uint64_t sum(std::uint64_t a, std::uint64_t b) {
    const auto value = checked_add(a, b);
    if (!value) { throw std::overflow_error("Analysis aggregate overflow"); }
    return *value;
}
template<class Level>
std::uint64_t depth(const std::vector<Level>& levels) {
    std::uint64_t total = 0;
    for (const auto& level : levels) { total = sum(total, level.total.value()); }
    return total;
}
}
std::optional<long double> TradeStatistics::vwap_ticks() const noexcept {
    if (volume == 0) { return std::nullopt; }
    return static_cast<long double>(notional_ticks) / static_cast<long double>(volume);
}
MarketAnalysis::MarketAnalysis(std::span<const InstrumentId> instruments) {
    if (instruments.empty()) { throw std::invalid_argument("Analysis needs instruments"); }
    for (const auto id : instruments) {
        if (id.value() == 0 || !instrument_indices_.try_emplace(id, instrument_indices_.size()).second) {
            throw std::invalid_argument("Invalid or duplicate analysis instrument");
        }
    }
    trades_.resize(instruments.size());
    staged_.resize(instruments.size());
}
void MarketAnalysis::consume(const CommandResult& result) {
    const auto next = checked_add(last_ingress_.value(), 1);
    if (!next || result.ingress.value() != *next) {
        throw std::invalid_argument("Analysis requires each command result in order");
    }
    // Fixed instrument set: reset transaction scratch without per-result allocation.
    // A late overflow may dirty scratch, but cannot change committed statistics.
    std::copy(trades_.begin(), trades_.end(), staged_.begin());
    for (const auto& event : result.events) {
        auto found = instrument_indices_.find(event.instrument);
        if (found == instrument_indices_.end()) { throw std::invalid_argument("Unknown analysis instrument"); }
        if (const auto* trade = std::get_if<TradeEvent>(&event.event)) {
            if (trade->price.value() <= 0 || trade->executed.value() == 0) {
                throw std::invalid_argument("Invalid analysis trade");
            }
            const auto notional = checked_multiply(static_cast<std::uint64_t>(trade->price.value()), trade->executed.value());
            if (!notional) { throw std::overflow_error("Analysis notional overflow"); }
            auto& stats = staged_[found->second];
            stats.count = sum(stats.count, 1);
            stats.volume = sum(stats.volume, trade->executed.value());
            stats.notional_ticks = sum(stats.notional_ticks, *notional);
        }
    }
    trades_.swap(staged_);
    last_ingress_ = result.ingress;
}
const TradeStatistics& MarketAnalysis::trades(InstrumentId instrument) const {
    return trades_[instrument_indices_.at(instrument)];
}
namespace {
template<class Book>
BookObservation observe_book(const Book& book, IngressSequence sequence, const TradeStatistics& trades) {
    BookObservation observation{};
    observation.command_sequence = sequence;
    observation.instrument = book.instrument;
    observation.trades = trades;
    observation.bid_depth = depth(book.bids);
    observation.ask_depth = depth(book.asks);
    const auto bid_size = book.bids.empty() ? 0 : book.bids.front().total.value();
    const auto ask_size = book.asks.empty() ? 0 : book.asks.front().total.value();
    if (!book.bids.empty()) { observation.best_bid = book.bids.front().price; }
    if (!book.asks.empty()) { observation.best_ask = book.asks.front().price; }
    if (observation.best_bid && observation.best_ask) {
        const auto bid = static_cast<long double>(observation.best_bid->value());
        const auto ask = static_cast<long double>(observation.best_ask->value());
        observation.spread_ticks = ask - bid;
        observation.midpoint_ticks = bid / 2 + ask / 2;
    }
    // Convert before addition/subtraction: two valid uint64 depths can overflow
    // integer addition, and the numerator may be negative.
    const auto bid = static_cast<long double>(bid_size);
    const auto ask = static_cast<long double>(ask_size);
    if (bid_size != 0 || ask_size != 0) { observation.top_imbalance = (bid - ask) / (bid + ask); }
    return observation;
}
} // namespace
BookObservation MarketAnalysis::observe(const BookSnapshot& book) const {
    return observe_book(book, last_ingress_, trades(book.instrument));
}
BookObservation MarketAnalysis::observe(const BookDepthSnapshot& book) const {
    return observe_book(book, last_ingress_, trades(book.instrument));
}
} // namespace exchange
