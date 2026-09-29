#pragma once

// Phase 2: shared, dependency-free helpers; all checks remain active in Release.
#include "exchange/engine/reference_engine.hpp"

#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace phase2_tests {
using namespace exchange;

inline void check(bool condition, std::string_view description) {
    if (!condition) {
        throw std::runtime_error(std::string{description});
    }
}

inline EngineConfig configuration() {
    return {{{InstrumentId{1}, "XYZ", PriceTicks{1}, PriceTicks{10000},
              Quantity{1000000}, Quantity{10000000}}}, {10000, 10000}};
}

inline NewInput limit(std::uint64_t id, Side side, std::int64_t price, std::uint64_t quantity,
                      TimeInForce tif = TimeInForce::GTC) {
    return {OrderId{id}, InstrumentId{1}, side, OrderType::Limit, tif,
            PriceTicks{price}, Quantity{quantity}};
}

inline NewInput market(std::uint64_t id, Side side, std::uint64_t quantity) {
    return {OrderId{id}, InstrumentId{1}, side, OrderType::Market, TimeInForce::IOC,
            std::nullopt, Quantity{quantity}};
}

inline ReplaceInput replace(std::uint64_t id, std::uint64_t generation,
                            std::int64_t price, std::uint64_t remaining) {
    return {OrderId{id}, OrderGeneration{generation}, PriceTicks{price}, Quantity{remaining}};
}

inline CommandResult submit(ReferenceEngine& engine, CommandInput input,
                            std::uint64_t session = 1) {
    const auto ingress = engine.last_ingress().value() + 1;
    auto result = engine.process({engine.epoch(), IngressSequence{ingress}, SessionId{session},
                                  CorrelationId{ingress}, std::move(input)});
    engine.validate();
    check(result.ingress == IngressSequence{ingress}, "result ingress");
    for (std::size_t index = 0; index < result.reports.size(); ++index) {
        const auto& report = result.reports[index];
        check(report.index == index && report.cause == result.ingress &&
              report.correlation == CorrelationId{ingress}, "report ordering/cause/correlation");
    }
    for (std::size_t index = 0; index < result.events.size(); ++index) {
        const auto& event = result.events[index];
        check(event.sequence.value() == result.market_before.value() + index + 1 &&
              event.cause == result.ingress, "contiguous events and cause");
    }
    check(result.market_after.value() == result.market_before.value() + result.events.size(),
          "market watermark includes exactly the emitted events");
    return result;
}

inline void expect_rejection(const CommandResult& result, RejectReason reason,
                              ExecutionStatus status = ExecutionStatus::ReplaceRejected) {
    check(result.reports.size() == 1 && result.reports.front().status == status &&
          result.reports.front().rejection == reason && result.events.empty() &&
          result.market_before == result.market_after, "exact rejection shape");
}
} // namespace phase2_tests
