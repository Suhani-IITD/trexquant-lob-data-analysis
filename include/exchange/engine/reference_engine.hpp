#pragma once

#include "exchange/book/reference_book.hpp"
#include "exchange/core/commands.hpp"
#include "exchange/events/command_result.hpp"

#include <cstddef>
#include <functional>
#include <unordered_map>
#include <variant>
#include <vector>

namespace exchange {

struct BidLocation { BidLevels::iterator level; };
struct AskLocation { AskLevels::iterator level; };
struct OrderLocation {
    InstrumentId instrument;
    std::variant<BidLocation, AskLocation> level;
    std::list<RestingOrder>::iterator order;
};
struct OrderIdHash {
    std::size_t operator()(OrderId id) const noexcept {
        return std::hash<std::uint64_t>{}(id.value());
    }
};

class ReferenceEngine {
public:
    explicit ReferenceEngine(EngineConfig config, EpochId epoch = EpochId{1});
    ReferenceEngine(const ReferenceEngine&) = delete;
    ReferenceEngine& operator=(const ReferenceEngine&) = delete;
    ReferenceEngine(ReferenceEngine&&) = delete;
    ReferenceEngine& operator=(ReferenceEngine&&) = delete;

    [[nodiscard]] CommandResult process(const CommandEnvelope& command);
    // Replaces output values while retaining vector capacity. Invalid envelopes leave
    // output untouched; preparation failure leaves empty output and unchanged engine.
    // References into output expire on its next use. Single owner, no concurrent calls.
    void process_into(const CommandEnvelope& command, CommandResult& output);
    [[nodiscard]] EngineSnapshot snapshot() const;
    [[nodiscard]] EngineDepthSnapshot depth_snapshot() const;
    [[nodiscard]] IngressSequence last_ingress() const noexcept { return last_ingress_; }
    [[nodiscard]] MarketSequence last_market() const noexcept { return last_market_; }
    [[nodiscard]] EpochId epoch() const noexcept { return epoch_; }
    [[nodiscard]] std::size_t active_orders() const noexcept { return active_.size(); }
    void validate() const;

private:
    struct PlannedFill {
        OrderId maker;
        PriceTicks price;
        Quantity executed;
    };
#ifdef EXCHANGE_REUSE_FILL_BUFFER
    // Retain measured scratch capacity, never iterators; OFF builds provide a baseline.
    // Clear before every plan, including after a rejected/failed preparation.
    std::vector<PlannedFill> fill_buffer_;
#endif
    using ActiveIndex = std::unordered_map<OrderId, OrderLocation, OrderIdHash>;
    void process_new(const CommandEnvelope&, const NewInput&, CommandResult&);
    void process_cancel(const CommandEnvelope&, const CancelInput&, CommandResult&);
    // Phase 2: New and priority-losing Replace share one preflight/prepare/commit path.
    void process_replace(const CommandEnvelope&, const ReplaceInput&, CommandResult&);
    // The caller has already looked up the instrument's book; pass it rather than search again.
    void match_order(const CommandEnvelope&, const NewInput&, ReferenceBook&, CommandResult&,
                              std::optional<OrderLocation> replaced_order = std::nullopt);
    void reject(const CommandEnvelope&, OrderIdentity, ExecutionStatus, RejectReason, CommandResult&) const;
    // Unlinks an order, erasing its level if emptied. With keep, the FIFO node moves
    // there and the index entry survives, so a resting replacement allocates neither.
    void remove_order(const OrderLocation&, std::list<RestingOrder>* keep = nullptr);
    void emit_event(CommandResult&, InstrumentId, MarketEvent);
    void emit_report(CommandResult&, const CommandEnvelope&, SessionId, OrderIdentity,
                     ExecutionStatus, Quantity, Quantity, std::optional<PriceTicks>);

    EngineConfig config_;
    EpochId epoch_;
    std::map<InstrumentId, ReferenceBook> books_;
    ActiveIndex active_;
    // Empty between commands; holds one prepared node only while a command is staged.
    ActiveIndex staging_index_;
    std::size_t active_levels_{};
    IngressSequence last_ingress_{};
    MarketSequence last_market_{};
};

class AdmissionDriver {
public:
    explicit AdmissionDriver(EngineConfig config, EpochId epoch = EpochId{1});
    [[nodiscard]] CommandResult submit(SessionId, CorrelationId, CommandInput);
    [[nodiscard]] const ReferenceEngine& engine() const noexcept { return engine_; }

private:
    ReferenceEngine engine_;
};

} // namespace exchange
