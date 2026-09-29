#include "exchange/engine/reference_engine.hpp"

#include <algorithm>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {
using namespace exchange;

class Checks {
public:
    void expect(bool condition, std::string_view message) {
        if (!condition) {
            ++failures_;
            std::cerr << "FAIL: " << message << '\n';
        }
    }
    [[nodiscard]] int result() const noexcept { return failures_ == 0 ? 0 : 1; }
private:
    int failures_{};
};

EngineConfig configuration() {
    return {{{InstrumentId{1}, "XYZ", PriceTicks{1}, PriceTicks{10000}, Quantity{1000000},
              Quantity{10000000}}}, {10000, 10000}};
}

NewInput order(std::uint64_t id, Side side, std::int64_t price, std::uint64_t quantity,
               std::uint32_t instrument = 1) {
    return {OrderId{id}, InstrumentId{instrument}, side, OrderType::Limit, TimeInForce::GTC,
            PriceTicks{price}, Quantity{quantity}};
}

CommandResult submit(ReferenceEngine& engine, CommandInput input, std::uint64_t session = 1) {
    const auto next = engine.last_ingress().value() + 1;
    auto result = engine.process({engine.epoch(), IngressSequence{next}, SessionId{session},
                                  CorrelationId{next}, std::move(input)});
    engine.validate(); // Also runs in Release; never rely on disabled assert().
    return result;
}

void expect_rejection(Checks& checks, const CommandResult& result, RejectReason reason) {
    checks.expect(result.reports.size() == 1, "rejection has one report");
    checks.expect(result.reports.at(0).rejection == reason, "correct rejection reason");
    checks.expect(result.events.empty() && result.market_before == result.market_after,
                  "rejection emits no event or market sequence");
}

void test_config(Checks& checks) {
    checks.expect(validate_config(configuration()).empty(), "valid configuration");
    auto invalid = configuration();
    invalid.capacity.maximum_active_orders = 0;
    invalid.instruments.push_back(invalid.instruments.front());
    invalid.instruments.front().minimum = PriceTicks{0};
    checks.expect(validate_config(invalid).size() == 4, "capacity, range, duplicate ID and symbol");
    bool rejected = false;
    try {
        ReferenceEngine engine{invalid};
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    checks.expect(rejected, "invalid configuration fails before construction");
}

void test_fifo(Checks& checks) {
    ReferenceEngine engine{configuration()};
    static_cast<void>(submit(engine, order(1, Side::Sell, 100, 30)));
    static_cast<void>(submit(engine, order(2, Side::Sell, 100, 40)));
    const auto result = submit(engine, order(3, Side::Buy, 105, 50));
    const std::vector<SequencedEvent> expected{
        {MarketSequence{3}, IngressSequence{3}, InstrumentId{1},
         TradeEvent{{OrderId{1}, OrderGeneration{1}}, {OrderId{3}, OrderGeneration{3}},
                    PriceTicks{100}, Quantity{30}}},
        {MarketSequence{4}, IngressSequence{3}, InstrumentId{1},
         TradeEvent{{OrderId{2}, OrderGeneration{2}}, {OrderId{3}, OrderGeneration{3}},
                    PriceTicks{100}, Quantity{20}}}
    };
    checks.expect(result.events == expected, "exact FIFO maker identities, prices, quantities and sequences");
    const std::vector<ExecutionReport> reports{
        {IngressSequence{3}, 0, CorrelationId{3}, SessionId{1}, {OrderId{3}, OrderGeneration{3}},
         ExecutionStatus::Accepted, Quantity{}, Quantity{50}, std::nullopt, std::nullopt},
        {IngressSequence{3}, 1, CorrelationId{3}, SessionId{1}, {OrderId{1}, OrderGeneration{1}},
         ExecutionStatus::Fill, Quantity{30}, Quantity{}, PriceTicks{100}, std::nullopt},
        {IngressSequence{3}, 2, CorrelationId{3}, SessionId{1}, {OrderId{3}, OrderGeneration{3}},
         ExecutionStatus::PartialFill, Quantity{30}, Quantity{20}, PriceTicks{100}, std::nullopt},
        {IngressSequence{3}, 3, CorrelationId{3}, SessionId{1}, {OrderId{2}, OrderGeneration{2}},
         ExecutionStatus::PartialFill, Quantity{20}, Quantity{20}, PriceTicks{100}, std::nullopt},
        {IngressSequence{3}, 4, CorrelationId{3}, SessionId{1}, {OrderId{3}, OrderGeneration{3}},
         ExecutionStatus::Fill, Quantity{20}, Quantity{}, PriceTicks{100}, std::nullopt}
    };
    checks.expect(result.reports == reports, "exact maker-before-taker report stream");
    const auto state = engine.snapshot();
    checks.expect(state.books.at(0).bids.empty(), "fully filled taker never rests");
    const auto& level = state.books.at(0).asks.at(0);
    checks.expect(level.total == Quantity{20} && level.fifo.size() == 1 &&
                  level.fifo.at(0).id == OrderId{2} && level.fifo.at(0).priority == IngressSequence{2},
                  "partial maker retains FIFO priority and quantity");
}

void test_price_priority(Checks& checks) {
    ReferenceEngine engine{configuration()};
    static_cast<void>(submit(engine, order(1, Side::Sell, 105, 10)));
    static_cast<void>(submit(engine, order(2, Side::Sell, 100, 10)));
    auto result = submit(engine, order(3, Side::Buy, 105, 25));
    checks.expect(std::get<TradeEvent>(result.events.at(0).event).maker.id == OrderId{2},
                  "better ask price precedes older ask");
    checks.expect(std::get<TradeEvent>(result.events.at(1).event).price == PriceTicks{105},
                  "sweep uses each maker's price");
    checks.expect(std::get<AddEvent>(result.events.at(2).event).remaining == Quantity{5},
                  "only unmatched remainder rests");
    static_cast<void>(submit(engine, order(4, Side::Buy, 104, 5)));
    result = submit(engine, order(5, Side::Sell, 104, 10));
    checks.expect(std::get<TradeEvent>(result.events.at(0).event).price == PriceTicks{105},
                  "sell matches highest bid first");
    checks.expect(std::get<TradeEvent>(result.events.at(1).event).price == PriceTicks{104},
                  "sell continues to next bid");
    checks.expect(engine.active_orders() == 0 && engine.snapshot().books.at(0).bids.empty(),
                  "both sides clean up completely filled levels");
}

void test_cancel(Checks& checks) {
    ReferenceEngine engine{configuration()};
    for (std::uint64_t id = 1; id <= 3; ++id) {
        static_cast<void>(submit(engine, order(id, Side::Buy, 100, 10)));
    }
    const auto middle = submit(engine, CancelInput{OrderId{2}, OrderGeneration{2}});
    checks.expect(std::get<CancelEvent>(middle.events.at(0).event).cancelled == Quantity{10},
                  "cancel contains removed remainder");
    auto state = engine.snapshot();
    checks.expect(state.books.at(0).bids.at(0).fifo.at(1).id == OrderId{3},
                  "middle cancellation preserves peers' FIFO");
    static_cast<void>(submit(engine, CancelInput{OrderId{1}, OrderGeneration{1}}));
    static_cast<void>(submit(engine, CancelInput{OrderId{3}, OrderGeneration{3}}));
    checks.expect(engine.active_orders() == 0 && engine.snapshot().books.at(0).bids.empty(),
                  "head and last cancellation remove empty level");
    expect_rejection(checks, submit(engine, CancelInput{OrderId{3}, OrderGeneration{3}}),
                     RejectReason::UnknownOrder);
}

void test_validation(Checks& checks) {
    ReferenceEngine engine{configuration()};
    static_cast<void>(submit(engine, order(1, Side::Buy, 100, 10)));
    const auto before = engine.snapshot();
    expect_rejection(checks, submit(engine, order(1, Side::Sell, 100, 10)), RejectReason::DuplicateOrderId);
    expect_rejection(checks, submit(engine, order(2, Side::Sell, 100, 0)), RejectReason::InvalidQuantity);
    expect_rejection(checks, submit(engine, order(2, Side::Buy, 0, 1)), RejectReason::InvalidPrice);
    expect_rejection(checks, submit(engine, order(2, Side::Buy, 10001, 1)), RejectReason::InvalidPrice);
    expect_rejection(checks, submit(engine, order(2, Side::Buy, 100, 1, 99)), RejectReason::InvalidInstrument);
    expect_rejection(checks, submit(engine, order(0, Side::Buy, 100, 1)), RejectReason::InvalidField);
    auto input = order(2, static_cast<Side>(9), 100, 1);
    expect_rejection(checks, submit(engine, input), RejectReason::InvalidField);
    input = order(2, Side::Buy, 100, 1);
    input.price.reset();
    expect_rejection(checks, submit(engine, input), RejectReason::InvalidPrice);
    input = order(2, Side::Buy, 100, 1);
    // Phase 2: valid IOC/Replace now succeed; retain this Phase 1 rejection-only
    // regression using an unknown TIF, market GTC, and an invalid replacement price.
    input.tif = static_cast<TimeInForce>(9);
    expect_rejection(checks, submit(engine, input), RejectReason::InvalidField);
    input.type = OrderType::Market;
    input.tif = TimeInForce::GTC;
    input.price.reset();
    expect_rejection(checks, submit(engine, input), RejectReason::UnsupportedOrderType);
    expect_rejection(checks, submit(engine, ReplaceInput{OrderId{1}, OrderGeneration{1},
                                                       PriceTicks{0}, Quantity{5}}),
                     RejectReason::InvalidPrice);
    checks.expect(engine.snapshot() == before && engine.last_market() == MarketSequence{1},
                  "all rejected commands preserve book and market sequence");
    checks.expect(engine.last_ingress().value() == 12, "business rejections still consume ingress");
}

void test_generations(Checks& checks) {
    ReferenceEngine engine{configuration()};
    static_cast<void>(submit(engine, order(1, Side::Sell, 100, 10)));
    static_cast<void>(submit(engine, CancelInput{OrderId{1}, OrderGeneration{1}}));
    static_cast<void>(submit(engine, order(1, Side::Sell, 100, 20)));
    const auto before = engine.snapshot();
    expect_rejection(checks, submit(engine, CancelInput{OrderId{1}, OrderGeneration{1}}), RejectReason::StaleOrder);
    expect_rejection(checks, submit(engine, CancelInput{OrderId{1}, OrderGeneration{3}}, 2), RejectReason::NotOwner);
    checks.expect(engine.snapshot() == before, "stale/wrong-owner cancellation cannot remove reused ID");
    const auto trade = submit(engine, order(2, Side::Buy, 100, 20), 2);
    checks.expect(trade.reports.at(1).recipient == SessionId{1} &&
                  trade.reports.at(2).recipient == SessionId{2}, "reports route to maker and taker owners");
    checks.expect(engine.active_orders() == 0, "valid trade completes reused lifecycle");
}

void test_capacity(Checks& checks) {
    auto config = configuration();
    config.capacity = {2, 1};
    ReferenceEngine engine{config};
    static_cast<void>(submit(engine, order(1, Side::Sell, 100, 5)));
    static_cast<void>(submit(engine, order(2, Side::Sell, 100, 5)));
    const auto full = engine.snapshot();
    expect_rejection(checks, submit(engine, order(3, Side::Sell, 100, 1)), RejectReason::ResourceExhausted);
    checks.expect(engine.snapshot() == full, "full active-order limit preserves book");
    const auto matched = submit(engine, order(3, Side::Buy, 100, 2));
    checks.expect(matched.events.size() == 1 && engine.active_orders() == 2,
                  "fully executable incoming order succeeds at active capacity");
    expect_rejection(checks, submit(engine, order(4, Side::Sell, 101, 1)), RejectReason::ResourceExhausted);

    config.capacity = {10, 10};
    config.instruments.front().maximum_level_quantity = Quantity{10};
    ReferenceEngine atomic{config};
    static_cast<void>(submit(atomic, order(1, Side::Sell, 100, 5)));
    const auto before = atomic.snapshot();
    const auto rejected = submit(atomic, order(2, Side::Buy, 100, 25));
    expect_rejection(checks, rejected, RejectReason::ResourceExhausted);
    checks.expect(atomic.snapshot() == before && atomic.last_market() == MarketSequence{1},
                  "oversized resting remainder rejected before executing planned maker fill");

    auto huge = configuration();
    huge.instruments.front().maximum_order_quantity = Quantity{std::numeric_limits<std::uint64_t>::max()};
    huge.instruments.front().maximum_level_quantity = Quantity{std::numeric_limits<std::uint64_t>::max()};
    ReferenceEngine aggregates{huge};
    static_cast<void>(submit(aggregates, order(1, Side::Buy, 100, std::numeric_limits<std::uint64_t>::max())));
    expect_rejection(checks, submit(aggregates, order(2, Side::Buy, 100, 1)), RejectReason::ResourceExhausted);
    checks.expect(aggregates.active_orders() == 1, "aggregate arithmetic never wraps");
}

void test_instruments(Checks& checks) {
    auto config = configuration();
    auto second = config.instruments.front();
    second.id = InstrumentId{2};
    second.symbol = "ABC";
    config.instruments.insert(config.instruments.begin(), second);
    ReferenceEngine engine{config};
    static_cast<void>(submit(engine, order(1, Side::Sell, 100, 10, 2)));
    static_cast<void>(submit(engine, order(2, Side::Buy, 100, 10, 1)));
    const auto state = engine.snapshot();
    checks.expect(state.books.at(0).instrument == InstrumentId{1} && state.books.at(1).instrument == InstrumentId{2},
                  "canonical snapshots sort instrument IDs regardless of config order");
    checks.expect(engine.active_orders() == 2, "different instruments do not match");
    expect_rejection(checks, submit(engine, order(1, Side::Buy, 100, 10, 1)), RejectReason::DuplicateOrderId);
}

void test_ingress(Checks& checks) {
    ReferenceEngine engine{configuration()};
    const auto input = order(1, Side::Buy, 100, 10);
    const auto fails = [&](EpochId epoch, IngressSequence ingress, SessionId session) {
        try {
            static_cast<void>(engine.process({epoch, ingress, session, CorrelationId{1}, input}));
            return false;
        } catch (const std::invalid_argument&) {
            return true;
        }
    };
    checks.expect(fails(EpochId{2}, IngressSequence{1}, SessionId{1}), "wrong epoch rejected");
    checks.expect(fails(EpochId{1}, IngressSequence{2}, SessionId{1}), "ingress gap rejected");
    checks.expect(fails(EpochId{1}, IngressSequence{1}, SessionId{}), "invalid session rejected");
    checks.expect(engine.active_orders() == 0 && engine.last_ingress() == IngressSequence{},
                  "envelope errors do not consume ingress");
    static_cast<void>(submit(engine, input));
    checks.expect(fails(EpochId{1}, IngressSequence{1}, SessionId{1}), "duplicate admission rejected");
}

// Independent, deliberately slow order-vector oracle for valid single-instrument limits/cancels.
struct SlowOrder {
    std::uint64_t id, generation, remaining;
    Side side;
    std::int64_t price;
};

void test_generated(Checks& checks) {
    ReferenceEngine engine{configuration()};
    std::vector<SlowOrder> model;
    std::mt19937_64 random{123456};
    std::uint64_t next_id = 1;
    for (std::uint64_t step = 1; step <= 2000; ++step) {
        if (!model.empty() && random() % 4 == 0) {
            const auto index = static_cast<std::size_t>(random() % model.size());
            const auto victim = model.at(index);
            static_cast<void>(submit(engine, CancelInput{OrderId{victim.id}, OrderGeneration{victim.generation}}));
            model.erase(model.begin() + static_cast<std::ptrdiff_t>(index));
        } else {
            const auto side = random() % 2 == 0 ? Side::Buy : Side::Sell;
            const auto price = std::int64_t{90} + static_cast<std::int64_t>(random() % 21);
            auto remaining = std::uint64_t{1} + random() % 30;
            const auto quantity = remaining;
            const auto id = next_id++;
            while (remaining != 0) {
                auto best = model.end();
                for (auto candidate = model.begin(); candidate != model.end(); ++candidate) {
                    if (candidate->side == side || (side == Side::Buy && candidate->price > price) ||
                        (side == Side::Sell && candidate->price < price)) {
                        continue;
                    }
                    if (best == model.end() ||
                        (side == Side::Buy && candidate->price < best->price) ||
                        (side == Side::Sell && candidate->price > best->price) ||
                        (candidate->price == best->price && candidate->generation < best->generation)) {
                        best = candidate;
                    }
                }
                if (best == model.end()) {
                    break;
                }
                const auto fill = std::min(remaining, best->remaining);
                remaining -= fill;
                best->remaining -= fill;
                if (best->remaining == 0) {
                    model.erase(best);
                }
            }
            if (remaining != 0) {
                model.push_back({id, step, remaining, side, price});
            }
            static_cast<void>(submit(engine, order(id, side, price, quantity)));
        }
        const auto state = engine.snapshot();
        std::size_t observed = 0;
        const auto inspect = [&](const auto& levels) {
            for (const auto& level : levels) {
                for (const auto& actual : level.fifo) {
                    ++observed;
                    const auto expected = std::find_if(model.begin(), model.end(), [&](const auto& item) {
                        return item.id == actual.id.value();
                    });
                    checks.expect(expected != model.end(), "generated order exists in independent model");
                    if (expected != model.end()) {
                        checks.expect(expected->remaining == actual.remaining.value() &&
                                      expected->generation == actual.generation.value() &&
                                      expected->price == level.price.value() && expected->side == level.side,
                                      "generated state matches independent vector oracle");
                    }
                }
            }
        };
        inspect(state.books.at(0).bids);
        inspect(state.books.at(0).asks);
        checks.expect(observed == model.size(), "generated model cardinality");
        if (checks.result() != 0) {
            std::cerr << "Reproduce with seed=123456, command prefix=" << step << '\n';
            break;
        }
    }
}
} // namespace

int main(int argc, char* argv[]) {
    if (argc != 2) {
        return 2;
    }
    Checks checks;
    const std::string_view suite{argv[1]};
    try {
        if (suite == "config") test_config(checks);
        else if (suite == "fifo") test_fifo(checks);
        else if (suite == "price_priority") test_price_priority(checks);
        else if (suite == "cancel") test_cancel(checks);
        else if (suite == "validation") test_validation(checks);
        else if (suite == "generations") test_generations(checks);
        else if (suite == "capacity") test_capacity(checks);
        else if (suite == "instruments") test_instruments(checks);
        else if (suite == "ingress") test_ingress(checks);
        else if (suite == "generated") test_generated(checks);
        else return 2;
    } catch (const std::exception& error) {
        std::cerr << "Unexpected exception in " << suite << ": " << error.what() << '\n';
        return 1;
    }
    return checks.result();
}
