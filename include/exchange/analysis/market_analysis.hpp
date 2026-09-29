#pragma once

#include "exchange/core/snapshot.hpp"
#include "exchange/events/command_result.hpp"
#include <map>
#include <span>

namespace exchange {

struct TradeStatistics {
    std::uint64_t count{};
    std::uint64_t volume{};
    std::uint64_t notional_ticks{};
    [[nodiscard]] std::optional<long double> vwap_ticks() const noexcept;
    bool operator==(const TradeStatistics&) const = default;
};

struct BookObservation {
    IngressSequence command_sequence;
    InstrumentId instrument;
    TradeStatistics trades;
    std::optional<PriceTicks> best_bid, best_ask;
    std::optional<long double> spread_ticks, midpoint_ticks, top_imbalance;
    std::uint64_t bid_depth{}, ask_depth{}; // Sum across every supplied level.
    bool operator==(const BookObservation&) const = default;
};

// Consumes every engine result once, in ingress order, starting at one.
// No second book: observations read deliberate, current engine snapshots.
class MarketAnalysis {
public:
    explicit MarketAnalysis(std::span<const InstrumentId> instruments);
    // Invalid input/overflow/allocation failure leaves all statistics unchanged.
    void consume(const CommandResult& result);
    // References expire at the next successful consume().
    [[nodiscard]] const TradeStatistics& trades(InstrumentId instrument) const;
    // Caller supplies a snapshot taken immediately after the last consumed command.
    [[nodiscard]] BookObservation observe(const BookSnapshot& book) const;
    [[nodiscard]] BookObservation observe(const BookDepthSnapshot& book) const;
    [[nodiscard]] IngressSequence last_ingress() const noexcept { return last_ingress_; }
private:
    std::map<InstrumentId, std::size_t> instrument_indices_;
    std::vector<TradeStatistics> trades_;
    std::vector<TradeStatistics> staged_; // Fixed-size transaction scratch, allocated at startup.
    IngressSequence last_ingress_{};
};
} // namespace exchange
