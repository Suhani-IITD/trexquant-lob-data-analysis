#pragma once

#include "exchange/core/types.hpp"

#include <cstdint>
#include <optional>
#include <variant>
#include <vector>

namespace exchange {

enum class RejectReason : std::uint16_t {
    InvalidField,
    InvalidInstrument,
    InvalidPrice,
    InvalidQuantity,
    DuplicateOrderId,
    UnknownOrder,
    StaleOrder,
    NotOwner,
    ResourceExhausted,
    UnsupportedOrderType
};
enum class ExecutionStatus : std::uint8_t {
    Accepted, Rejected, PartialFill, Fill, Cancelled, CancelRejected, ReplaceRejected,
    // Phase 2: IOC expiry is private output; replacement acknowledges before any fills.
    Expired, Replaced
};
struct OrderIdentity {
    OrderId id;
    OrderGeneration generation;
    bool operator==(const OrderIdentity&) const = default;
};
struct ExecutionReport {
    IngressSequence cause;
    std::uint64_t index;
    CorrelationId correlation; // Correlation of the triggering command, including maker fills.
    SessionId recipient;
    OrderIdentity order;
    ExecutionStatus status;
    Quantity last_quantity;
    Quantity leaves_quantity;
    std::optional<PriceTicks> last_price;
    std::optional<RejectReason> rejection;
    bool operator==(const ExecutionReport&) const = default;
};
struct AddEvent {
    OrderIdentity order;
    Side side;
    PriceTicks price;
    Quantity remaining;
    IngressSequence priority;
    bool operator==(const AddEvent&) const = default;
};
struct TradeEvent {
    OrderIdentity maker;
    OrderIdentity taker;
    PriceTicks price;
    Quantity executed;
    bool operator==(const TradeEvent&) const = default;
};
struct CancelEvent {
    OrderIdentity order;
    Quantity cancelled;
    bool operator==(const CancelEvent&) const = default;
};
// Phase 2: reductions preserve the existing FIFO position.
struct ReduceEvent {
    OrderIdentity order;
    Quantity new_remaining;
    bool operator==(const ReduceEvent&) const = default;
};
using MarketEvent = std::variant<AddEvent, TradeEvent, CancelEvent, ReduceEvent>;
struct SequencedEvent {
    MarketSequence sequence;
    IngressSequence cause;
    InstrumentId instrument;
    MarketEvent event;
    bool operator==(const SequencedEvent&) const = default;
};
struct CommandResult {
    IngressSequence ingress;
    std::vector<ExecutionReport> reports;
    std::vector<SequencedEvent> events;
    MarketSequence market_before;
    MarketSequence market_after;
    bool operator==(const CommandResult&) const = default;
};

} // namespace exchange
