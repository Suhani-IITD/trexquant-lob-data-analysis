// Phase 2: contract tests supplement the preserved Phase 1 regression suites.
#include "phase2_test_support.hpp"

#include <algorithm>
#include <iostream>
#include <limits>
#include <random>
#include <tuple>
#include <vector>

namespace {
using namespace phase2_tests;

void test_ioc() {
    for (const auto side : {Side::Buy, Side::Sell}) {
        const auto opposite = side == Side::Buy ? Side::Sell : Side::Buy;
        ReferenceEngine engine{configuration()};
        const auto empty = submit(engine, market(1, side, 20));
        const OrderIdentity empty_identity{OrderId{1}, OrderGeneration{1}};
        check(empty.reports == std::vector<ExecutionReport>{
            {IngressSequence{1}, 0, CorrelationId{1}, SessionId{1}, empty_identity,
             ExecutionStatus::Accepted, Quantity{}, Quantity{20}, std::nullopt, std::nullopt},
            {IngressSequence{1}, 1, CorrelationId{1}, SessionId{1}, empty_identity,
             ExecutionStatus::Expired, Quantity{}, Quantity{}, std::nullopt, std::nullopt}},
             "empty market accepts then expires with zero leaves");
        check(empty.events.empty() && engine.active_orders() == 0, "empty IOC never rests");
        static_cast<void>(submit(engine, limit(2, opposite, 100, 5), 2));
        const auto worse_price = side == Side::Buy ? 101 : 99;
        static_cast<void>(submit(engine, limit(3, opposite, worse_price, 6), 2));
        const auto partial = submit(engine, limit(4, side, 100, 9, TimeInForce::IOC));
        check(partial.reports.size() == 4 && partial.events.size() == 1 &&
              partial.reports[1].status == ExecutionStatus::Fill &&
              partial.reports[1].recipient == SessionId{2} &&
              partial.reports[2].status == ExecutionStatus::PartialFill &&
              partial.reports[2].leaves_quantity == Quantity{4} &&
              partial.reports[3].status == ExecutionStatus::Expired &&
              partial.reports[3].leaves_quantity == Quantity{}, "partial IOC report order/expiry");
        check(std::get<TradeEvent>(partial.events.front().event).price == PriceTicks{100} &&
              engine.active_orders() == 1, "limit IOC respects price and does not rest remainder");
        const auto no_cross = submit(engine, limit(5, side, 100, 1, TimeInForce::IOC));
        check(no_cross.events.empty() && no_cross.reports.back().status == ExecutionStatus::Expired,
              "noncrossing limit IOC expires");
        const auto full = submit(engine, market(6, side, 6));
        check(full.reports.size() == 3 && full.reports.back().status == ExecutionStatus::Fill &&
              engine.active_orders() == 0, "fully filled market has no expiry report");
        static_cast<void>(submit(engine, limit(7, opposite, 100, 3)));
        static_cast<void>(submit(engine, limit(8, opposite, worse_price, 4)));
        const auto sweep = submit(engine, market(9, side, 10));
        check(sweep.events.size() == 2 && sweep.reports.size() == 6 &&
              std::get<TradeEvent>(sweep.events[0].event).maker.id == OrderId{7} &&
              std::get<TradeEvent>(sweep.events[1].event).maker.id == OrderId{8} &&
              sweep.reports.back().status == ExecutionStatus::Expired && engine.active_orders() == 0,
              "market sweeps best prices then expires without Add/Cancel");
    }
}

void test_validation() {
    ReferenceEngine engine{configuration()};
    auto request = market(1, Side::Buy, 1);
    request.price = PriceTicks{0};
    expect_rejection(submit(engine, request), RejectReason::InvalidPrice, ExecutionStatus::Rejected);
    request.price = PriceTicks{100};
    expect_rejection(submit(engine, request), RejectReason::InvalidPrice, ExecutionStatus::Rejected);
    request.tif = TimeInForce::GTC;
    expect_rejection(submit(engine, request), RejectReason::UnsupportedOrderType, ExecutionStatus::Rejected);
    request = market(1, Side::Buy, 0);
    expect_rejection(submit(engine, request), RejectReason::InvalidQuantity, ExecutionStatus::Rejected);
    request = market(1, Side::Buy, 1000001);
    expect_rejection(submit(engine, request), RejectReason::InvalidQuantity, ExecutionStatus::Rejected);
    request = market(1, Side::Buy, 1);
    request.instrument = InstrumentId{99};
    request.price = PriceTicks{100};
    expect_rejection(submit(engine, request), RejectReason::InvalidInstrument, ExecutionStatus::Rejected);
    request.type = static_cast<OrderType>(9);
    expect_rejection(submit(engine, request), RejectReason::InvalidField, ExecutionStatus::Rejected);
    static_cast<void>(submit(engine, limit(1, Side::Buy, 100, 5)));
    expect_rejection(submit(engine, market(1, Side::Sell, 5)), RejectReason::DuplicateOrderId,
                     ExecutionStatus::Rejected);
    check(engine.active_orders() == 1, "invalid market orders cannot consume liquidity");
}

void test_replace_priority() {
    for (const auto side : {Side::Buy, Side::Sell}) {
        ReferenceEngine engine{configuration()};
        static_cast<void>(submit(engine, limit(1, side, 100, 10)));
        static_cast<void>(submit(engine, limit(2, side, 100, 20)));
        const auto reduced = submit(engine, replace(1, 1, 100, 5));
        check(reduced.reports.size() == 1 && reduced.reports.front().status == ExecutionStatus::Replaced &&
              reduced.reports.front().leaves_quantity == Quantity{5} && reduced.events.size() == 1 &&
              std::get<ReduceEvent>(reduced.events.front().event) ==
                  ReduceEvent{{OrderId{1}, OrderGeneration{1}}, Quantity{5}}, "exact reduction output");
        const auto before = engine.snapshot();
        const auto unchanged = submit(engine, replace(1, 1, 100, 5));
        check(unchanged.reports.size() == 1 && unchanged.reports.front().status == ExecutionStatus::Replaced &&
              unchanged.events.empty() && engine.snapshot() == before, "no-op acknowledges without mutation");
        const auto increased = submit(engine, replace(1, 1, 100, 8));
        check(increased.reports.size() == 1 && increased.events.size() == 2 &&
              std::get<CancelEvent>(increased.events[0].event).cancelled == Quantity{5} &&
              std::get<AddEvent>(increased.events[1].event).priority == IngressSequence{5},
              "increase emits Cancel then Add at replacement priority");
        const auto state = engine.snapshot();
        const auto& levels = side == Side::Buy ? state.books[0].bids : state.books[0].asks;
        check(levels[0].fifo[0].id == OrderId{2} && levels[0].fifo[1].id == OrderId{1} &&
              levels[0].fifo[1].generation == OrderGeneration{1} && levels[0].total == Quantity{28},
              "increase moves to FIFO tail but keeps lifecycle");
        const auto fill = submit(engine, market(3, side == Side::Buy ? Side::Sell : Side::Buy, 21));
        check(std::get<TradeEvent>(fill.events[0].event).maker.id == OrderId{2} &&
              std::get<TradeEvent>(fill.events[1].event).maker.id == OrderId{1}, "FIFO after increase");
        const auto after_fill = submit(engine, replace(1, 1, 100, 6));
        check(std::get<ReduceEvent>(after_fill.events[0].event).new_remaining == Quantity{6},
              "replacement specifies remaining quantity after past fills");
        static_cast<void>(submit(engine, CancelInput{OrderId{1}, OrderGeneration{1}}));
        check(engine.active_orders() == 0, "original generation remains cancellable");
    }
    // Single-order level deletion/recreation used to invalidate the retained level iterator.
    ReferenceEngine sole{configuration()};
    static_cast<void>(submit(sole, limit(1, Side::Buy, 100, 5)));
    static_cast<void>(submit(sole, replace(1, 1, 100, 6)));
    check(sole.snapshot().books[0].bids[0].fifo[0].priority == IngressSequence{2},
          "sole-node increase safely recreates level");
}

void test_replace_crossing() {
    for (const auto side : {Side::Buy, Side::Sell}) {
        for (const auto desired : {3ULL, 8ULL}) {
            ReferenceEngine engine{configuration()};
            const auto original_price = side == Side::Buy ? 90 : 110;
            const auto opposite = side == Side::Buy ? Side::Sell : Side::Buy;
            static_cast<void>(submit(engine, limit(1, side, original_price, 7), 2));
            static_cast<void>(submit(engine, limit(2, opposite, 100, 5)));
            const auto result = submit(engine, replace(1, 1, 100, desired), 2);
            const OrderIdentity identity{OrderId{1}, OrderGeneration{1}};
            check(result.reports.size() == 3 && result.reports[0].status == ExecutionStatus::Replaced &&
                  result.reports[0].order == identity && result.reports[1].recipient == SessionId{1} &&
                  result.reports[2].recipient == SessionId{2}, "replace acknowledgement precedes maker/taker fills");
            check(result.events.size() == (desired == 3 ? 2U : 3U) &&
                  std::get<CancelEvent>(result.events[0].event) == CancelEvent{identity, Quantity{7}} &&
                  std::get<TradeEvent>(result.events[1].event).taker == identity,
                  "crossing replace cancels old then trades with preserved generation");
            if (desired == 8) {
                const auto added = std::get<AddEvent>(result.events[2].event);
                check(added.remaining == Quantity{3} && added.priority == IngressSequence{3} &&
                      added.order == identity, "crossing replacement rests only remaining demand");
            } else {
                check(result.reports.back().status == ExecutionStatus::Fill,
                      "fully executed replacement does not rest");
                expect_rejection(submit(engine, replace(1, 1, 100, 1)), RejectReason::UnknownOrder);
            }
        }
    }
}

void test_acceptance() {
    // Phase 2: exact output for the README's six-command acceptance example.
    ReferenceEngine engine{configuration()};
    static_cast<void>(submit(engine, limit(1, Side::Sell, 100, 30)));
    static_cast<void>(submit(engine, limit(2, Side::Sell, 100, 40)));
    const OrderIdentity first{OrderId{1}, OrderGeneration{1}};
    const OrderIdentity second{OrderId{2}, OrderGeneration{2}};
    const OrderIdentity taker{OrderId{3}, OrderGeneration{3}};
    const auto filled = submit(engine, limit(3, Side::Buy, 100, 50, TimeInForce::IOC));
    const std::vector<ExecutionReport> reports{
        {IngressSequence{3}, 0, CorrelationId{3}, SessionId{1}, taker,
         ExecutionStatus::Accepted, Quantity{}, Quantity{50}, std::nullopt, std::nullopt},
        {IngressSequence{3}, 1, CorrelationId{3}, SessionId{1}, first,
         ExecutionStatus::Fill, Quantity{30}, Quantity{}, PriceTicks{100}, std::nullopt},
        {IngressSequence{3}, 2, CorrelationId{3}, SessionId{1}, taker,
         ExecutionStatus::PartialFill, Quantity{30}, Quantity{20}, PriceTicks{100}, std::nullopt},
        {IngressSequence{3}, 3, CorrelationId{3}, SessionId{1}, second,
         ExecutionStatus::PartialFill, Quantity{20}, Quantity{20}, PriceTicks{100}, std::nullopt},
        {IngressSequence{3}, 4, CorrelationId{3}, SessionId{1}, taker,
         ExecutionStatus::Fill, Quantity{20}, Quantity{}, PriceTicks{100}, std::nullopt}};
    check(filled == CommandResult{IngressSequence{3}, reports,
        {{MarketSequence{3}, IngressSequence{3}, InstrumentId{1}, TradeEvent{first, taker, PriceTicks{100}, Quantity{30}}},
         {MarketSequence{4}, IngressSequence{3}, InstrumentId{1}, TradeEvent{second, taker, PriceTicks{100}, Quantity{20}}}},
        MarketSequence{2}, MarketSequence{4}}, "exact IOC fill result");
    const auto reduced = submit(engine, replace(2, 2, 100, 10));
    check(reduced == CommandResult{IngressSequence{4},
        {{IngressSequence{4}, 0, CorrelationId{4}, SessionId{1}, second,
          ExecutionStatus::Replaced, Quantity{}, Quantity{10}, std::nullopt, std::nullopt}},
        {{MarketSequence{5}, IngressSequence{4}, InstrumentId{1}, ReduceEvent{second, Quantity{10}}}},
        MarketSequence{4}, MarketSequence{5}}, "exact reduction result");
    const auto repriced = submit(engine, replace(2, 2, 101, 15));
    check(repriced == CommandResult{IngressSequence{5},
        {{IngressSequence{5}, 0, CorrelationId{5}, SessionId{1}, second,
          ExecutionStatus::Replaced, Quantity{}, Quantity{15}, std::nullopt, std::nullopt}},
        {{MarketSequence{6}, IngressSequence{5}, InstrumentId{1}, CancelEvent{second, Quantity{10}}},
         {MarketSequence{7}, IngressSequence{5}, InstrumentId{1},
          AddEvent{second, Side::Sell, PriceTicks{101}, Quantity{15}, IngressSequence{5}}}},
        MarketSequence{5}, MarketSequence{7}}, "exact reprice result");
    const auto cancelled = submit(engine, CancelInput{OrderId{2}, OrderGeneration{2}});
    check(cancelled == CommandResult{IngressSequence{6},
        {{IngressSequence{6}, 0, CorrelationId{6}, SessionId{1}, second,
          ExecutionStatus::Cancelled, Quantity{}, Quantity{}, std::nullopt, std::nullopt}},
        {{MarketSequence{8}, IngressSequence{6}, InstrumentId{1}, CancelEvent{second, Quantity{15}}}},
        MarketSequence{7}, MarketSequence{8}} && engine.active_orders() == 0, "exact final cancel result");
}

void test_replace_rejections() {
    ReferenceEngine engine{configuration()};
    static_cast<void>(submit(engine, limit(1, Side::Buy, 100, 5)));
    const auto original = engine.snapshot();
    expect_rejection(submit(engine, replace(0, 0, 0, 0)), RejectReason::InvalidField);
    expect_rejection(submit(engine, replace(9, 0, 0, 0)), RejectReason::UnknownOrder);
    expect_rejection(submit(engine, replace(1, 9, 0, 0), 2), RejectReason::StaleOrder);
    expect_rejection(submit(engine, replace(1, 1, 0, 0), 2), RejectReason::NotOwner);
    expect_rejection(submit(engine, replace(1, 1, 0, 0)), RejectReason::InvalidPrice);
    expect_rejection(submit(engine, replace(1, 1, 10001, 1)), RejectReason::InvalidPrice);
    expect_rejection(submit(engine, replace(1, 1, 100, 0)), RejectReason::InvalidQuantity);
    expect_rejection(submit(engine, replace(1, 1, 100, 1000001)), RejectReason::InvalidQuantity);
    check(engine.snapshot() == original, "all replace validation preserves original order and priority");
    static_cast<void>(submit(engine, CancelInput{OrderId{1}, OrderGeneration{1}}));
    const auto reused = submit(engine, limit(1, Side::Buy, 100, 5));
    const auto generation = reused.ingress.value();
    expect_rejection(submit(engine, replace(1, 1, 101, 6)), RejectReason::StaleOrder);
    const auto valid = submit(engine, replace(1, generation, 101, 6));
    check(valid.reports[0].order.generation == OrderGeneration{generation}, "reused lifecycle remains distinct");
}

void test_capacity() {
    auto config = configuration();
    config.capacity = {1, 1};
    ReferenceEngine engine{config};
    static_cast<void>(submit(engine, limit(1, Side::Buy, 100, 5)));
    static_cast<void>(submit(engine, replace(1, 1, 101, 6)));
    static_cast<void>(submit(engine, replace(1, 1, 101, 7)));
    check(engine.active_orders() == 1, "replacement reuses both order and level capacity");
    const auto ioc = submit(engine, market(2, Side::Sell, 20));
    check(engine.active_orders() == 0 && ioc.reports.back().status == ExecutionStatus::Expired,
          "IOC with excess demand succeeds at full capacity");

    config.capacity = {3, 2};
    ReferenceEngine levels{config};
    static_cast<void>(submit(levels, limit(1, Side::Buy, 100, 5)));
    static_cast<void>(submit(levels, limit(2, Side::Buy, 100, 5)));
    static_cast<void>(submit(levels, limit(3, Side::Buy, 99, 5)));
    const auto before_levels = levels.snapshot();
    expect_rejection(submit(levels, replace(1, 1, 98, 6)), RejectReason::ResourceExhausted);
    check(levels.snapshot() == before_levels, "old nonempty level prevents new level at capacity");
    static_cast<void>(submit(levels, replace(1, 1, 99, 6)));
    check(levels.active_orders() == 3, "reprice to existing level succeeds at capacity");

    config.capacity = {10, 10};
    config.instruments.front().maximum_level_quantity = Quantity{10};
    ReferenceEngine atomic{config};
    static_cast<void>(submit(atomic, limit(1, Side::Buy, 90, 5)));
    static_cast<void>(submit(atomic, limit(2, Side::Sell, 100, 5)));
    const auto before = atomic.snapshot();
    expect_rejection(submit(atomic, replace(1, 1, 100, 20)), RejectReason::ResourceExhausted);
    check(atomic.snapshot() == before && atomic.last_market() == MarketSequence{2},
          "rejected crossing replacement changes neither old order nor planned makers");

    config.instruments.front().maximum_order_quantity = Quantity{std::numeric_limits<std::uint64_t>::max()};
    config.instruments.front().maximum_level_quantity = Quantity{std::numeric_limits<std::uint64_t>::max()};
    ReferenceEngine overflow{config};
    static_cast<void>(submit(overflow, limit(1, Side::Buy, 100, std::numeric_limits<std::uint64_t>::max() - 1)));
    static_cast<void>(submit(overflow, limit(2, Side::Buy, 100, 1)));
    const auto full = overflow.snapshot();
    expect_rejection(submit(overflow, replace(2, 2, 100, 2)), RejectReason::ResourceExhausted);
    check(overflow.snapshot() == full, "replacement aggregate overflow preserves state");
}

// Phase 2: independent vector oracle. It scans all orders for each next maker,
// rather than using the engine's maps, ID index, fill plans, or mutation helpers.
struct ModelOrder {
    std::uint64_t id;
    std::uint64_t generation;
    std::uint64_t priority;
    Side side;
    std::int64_t price;
    std::uint64_t remaining;
};

void model_match(std::vector<ModelOrder>& model, ModelOrder taker, bool market_order, bool rests) {
    while (taker.remaining != 0) {
        auto best = model.end();
        for (auto candidate = model.begin(); candidate != model.end(); ++candidate) {
            if (candidate->side == taker.side || (!market_order &&
                (taker.side == Side::Buy ? candidate->price > taker.price : candidate->price < taker.price))) {
                continue;
            }
            const bool better_price = best != model.end() &&
                (taker.side == Side::Buy ? candidate->price < best->price : candidate->price > best->price);
            if (best == model.end() || better_price ||
                (candidate->price == best->price && candidate->priority < best->priority)) {
                best = candidate;
            }
        }
        if (best == model.end()) break;
        const auto executed = std::min(best->remaining, taker.remaining);
        best->remaining -= executed;
        taker.remaining -= executed;
        if (best->remaining == 0) model.erase(best);
    }
    if (rests && taker.remaining != 0) model.push_back(taker);
}

void compare_model(const ReferenceEngine& engine, std::vector<ModelOrder> model) {
    std::sort(model.begin(), model.end(), [](const auto& left, const auto& right) {
        if (left.side != right.side) return left.side == Side::Buy;
        if (left.price != right.price) return left.side == Side::Buy ? left.price > right.price : left.price < right.price;
        return left.priority < right.priority;
    });
    const auto snapshot = engine.snapshot();
    std::size_t index = 0;
    const auto inspect = [&](const auto& levels) {
        for (const auto& level : levels) {
            std::uint64_t total = 0;
            for (const auto& actual : level.fifo) {
                check(index < model.size(), "model cardinality too small");
                const auto& expected = model[index++];
                check(actual.id.value() == expected.id && actual.generation.value() == expected.generation &&
                      actual.priority.value() == expected.priority && actual.remaining.value() == expected.remaining &&
                      level.side == expected.side && level.price.value() == expected.price,
                      "mixed command state/FIFO/lifecycle differs from independent model");
                total += actual.remaining.value();
            }
            check(total == level.total.value(), "model aggregate quantity");
        }
    };
    inspect(snapshot.books[0].bids);
    inspect(snapshot.books[0].asks);
    check(index == model.size(), "model cardinality too large");
}

void test_generated() {
    for (const auto seed : {123456ULL, 42ULL, 987654ULL}) {
        ReferenceEngine engine{configuration()};
        std::vector<ModelOrder> model;
        std::mt19937_64 random{seed};
        for (std::uint64_t step = 1; step <= 2000; ++step) {
            try {
                const auto operation = random() % 8;
                if (!model.empty() && operation < 3) {
                    const auto index = static_cast<std::size_t>(random() % model.size());
                    auto existing = model[index];
                    if (operation == 0) {
                        static_cast<void>(submit(engine, CancelInput{OrderId{existing.id}, OrderGeneration{existing.generation}}));
                        model.erase(model.begin() + static_cast<std::ptrdiff_t>(index));
                    } else {
                        const auto price = operation == 1 ? existing.price : 95 + static_cast<std::int64_t>(random() % 11);
                        const auto quantity = 1 + random() % 30;
                        const auto result = submit(engine, replace(existing.id, existing.generation, price, quantity));
                        check(result.reports[0].status == ExecutionStatus::Replaced, "valid generated replace");
                        if (price == existing.price && quantity <= existing.remaining) {
                            model[index].remaining = quantity;
                        } else {
                            model.erase(model.begin() + static_cast<std::ptrdiff_t>(index));
                            existing.price = price;
                            existing.remaining = quantity;
                            existing.priority = step;
                            model_match(model, existing, false, true);
                        }
                    }
                } else {
                    const auto side = random() % 2 == 0 ? Side::Buy : Side::Sell;
                    const auto price = 95 + static_cast<std::int64_t>(random() % 11);
                    const auto quantity = 1 + random() % 30;
                    const bool is_market = operation == 3;
                    const bool rests = operation != 3 && operation != 4;
                    const auto input = is_market ? market(step, side, quantity)
                        : limit(step, side, price, quantity, rests ? TimeInForce::GTC : TimeInForce::IOC);
                    const auto result = submit(engine, input);
                    check(result.reports[0].status == ExecutionStatus::Accepted, "valid generated new");
                    model_match(model, {step, step, step, side, price, quantity}, is_market, rests);
                }
                compare_model(engine, model);
            } catch (...) {
                std::cerr << "Reproduce seed=" << seed << " prefix=" << step << '\n';
                throw;
            }
        }
    }
}
} // namespace

int main(int argc, char* argv[]) {
    if (argc != 2) return 2;
    try {
        const std::string_view suite{argv[1]};
        if (suite == "ioc") test_ioc();
        else if (suite == "validation") test_validation();
        else if (suite == "replace_priority") test_replace_priority();
        else if (suite == "replace_crossing") test_replace_crossing();
        else if (suite == "acceptance") test_acceptance();
        else if (suite == "replace_rejections") test_replace_rejections();
        else if (suite == "capacity") test_capacity();
        else if (suite == "generated") test_generated();
        else return 2;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
