#pragma once

#include "exchange/core/types.hpp"

#include <optional>
#include <variant>

namespace exchange {

struct NewInput {
    OrderId id;
    InstrumentId instrument;
    Side side;
    OrderType type;
    TimeInForce tif;
    std::optional<PriceTicks> price;
    Quantity quantity;
};

struct CancelInput {
    OrderId id;
    OrderGeneration generation;
};

// Phase 2: quantity is desired remaining demand, not the original submitted total.
// Replacement preserves the lifecycle generation even when it loses FIFO priority.
struct ReplaceInput {
    OrderId id;
    OrderGeneration generation;
    PriceTicks new_price;
    Quantity new_remaining;
};

using CommandInput = std::variant<NewInput, CancelInput, ReplaceInput>;

struct CommandEnvelope {
    EpochId epoch;
    IngressSequence ingress;
    SessionId session;
    CorrelationId correlation;
    CommandInput input;
};

} // namespace exchange
