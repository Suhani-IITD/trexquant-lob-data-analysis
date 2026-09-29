// Phase 2: fail each ordinary allocation in a command, one at a time. This
// executable owns the allocator override; production code has no test hooks.
#include "phase2_test_support.hpp"
#include "exchange/analysis/market_analysis.hpp"
#include <array>

#include <cstdlib>
#include <iostream>
#include <new>

namespace {
bool inject_failure = false;
std::size_t allocations_before_failure = 0;

void* allocate(std::size_t bytes) {
    if (inject_failure) {
        if (allocations_before_failure == 0) throw std::bad_alloc{};
        --allocations_before_failure;
    }
    if (void* memory = std::malloc(bytes == 0 ? 1 : bytes)) return memory;
    throw std::bad_alloc{};
}
} // namespace

void* operator new(std::size_t bytes) { return allocate(bytes); }
void* operator new[](std::size_t bytes) { return allocate(bytes); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

namespace {
using namespace phase2_tests;

void exercise(const CommandInput& input, bool reuse = false, bool warm = false) {
    bool completed = false;
    std::size_t observed_failures = 0;
    for (std::size_t fail_at = 0; fail_at < 100; ++fail_at) {
        ReferenceEngine engine{configuration()};
        static_cast<void>(submit(engine, limit(1, Side::Buy, 90, 5)));
        static_cast<void>(submit(engine, limit(2, Side::Sell, 100, 3)));
        auto previous = submit(engine, limit(3, Side::Sell, 101, 4));
        CommandResult output;
        if (warm) {
            if (reuse) { output = std::move(previous); }
            output.reports.reserve(3);
            output.events.reserve(1);
        }
        const auto before = engine.snapshot();
        const auto ingress_before = engine.last_ingress();
        const auto market_before = engine.last_market();
        const CommandEnvelope command{engine.epoch(), IngressSequence{4}, SessionId{1},
                                       CorrelationId{4}, input};
        allocations_before_failure = fail_at;
        inject_failure = true;
        bool failed = false;
        try {
            if (reuse) { engine.process_into(command, output); }
            else { output = engine.process(command); }
            const auto& result = output;
            inject_failure = false;
            check(!result.reports.empty(), "fault test command produced a report");
        } catch (const std::bad_alloc&) {
            inject_failure = false;
            failed = true;
        } catch (...) {
            inject_failure = false;
            throw;
        }
        engine.validate();
        if (!failed) {
            completed = true;
            break;
        }
        ++observed_failures;
        check(output.reports.empty() && output.events.empty(), "failed preparation exposed partial output");
        if (reuse) {
            check(output.ingress == command.ingress && output.market_before == market_before &&
                  output.market_after == market_before, "failed output metadata");
        }
        check(engine.snapshot() == before && engine.last_ingress() == ingress_before &&
              engine.last_market() == market_before, "allocation failure changed committed state");
        // Failed preparation emitted no output and did not consume admission. The
        // exact same envelope can be retried here, inside this controlled test.
        ReferenceEngine expected{configuration()};
        static_cast<void>(submit(expected, limit(1, Side::Buy, 90, 5)));
        static_cast<void>(submit(expected, limit(2, Side::Sell, 100, 3)));
        static_cast<void>(submit(expected, limit(3, Side::Sell, 101, 4)));
        const auto expected_result = expected.process(command);
        if (reuse) { engine.process_into(command, output); }
        else { output = engine.process(command); }
        const auto& retried = output;
        check(retried == expected_result && engine.snapshot() == expected.snapshot(), "retry semantic equality");
        check(!retried.reports.empty(), "retry after failed preparation succeeds");
        engine.validate();
    }
    check(completed && (observed_failures > 0 || warm), "all allocation boundaries were exercised");
    std::cout << "checked " << observed_failures << " allocation failure positions\n";
}
void analysis_without_allocations() {
    const std::array ids{InstrumentId{1}};
    MarketAnalysis analysis{ids};
    ReferenceEngine engine{configuration()};
    const auto add = submit(engine, limit(1, Side::Sell, 100, 5));
    const auto fill = submit(engine, market(2, Side::Buy, 3));
    allocations_before_failure = 0;
    inject_failure = true;
    try {
        analysis.consume(add);
        analysis.consume(fill);
    } catch (...) { inject_failure = false; throw; }
    inject_failure = false;
    check(analysis.trades(ids[0]) == TradeStatistics{1, 3, 300}, "allocation-free consumption preserves totals");
}
} // namespace

int main() {
    try {
        analysis_without_allocations();
        for (const bool reuse : {false, true}) {
            for (const bool warm : {false, true}) {
                exercise(limit(4, Side::Buy, 101, 10), reuse, warm); // Fills, new price level, list/index nodes.
                exercise(limit(4, Side::Buy, 90, 2), reuse, warm);  // Existing price level insertion.
                exercise(market(4, Side::Buy, 10), reuse, warm);   // Fills followed by private expiry.
                exercise(replace(1, 1, 101, 10), reuse, warm);     // Cancel, multiple trades, remainder Add.
                exercise(replace(2, 2, 90, 10), reuse, warm);      // Sell-side replacement and ask insertion.
                exercise(replace(1, 1, 90, 6), reuse, warm);       // Recreate the sole order's level.
                exercise(replace(1, 1, 90, 2), reuse, warm);       // Reduce output storage before mutation.
                exercise(replace(1, 1, 90, 5), reuse, warm);       // No-op acknowledgement storage.
                exercise(CancelInput{OrderId{1}, OrderGeneration{1}}, reuse, warm);
                exercise(limit(0, Side::Buy, 90, 1), reuse, warm);
                exercise(CancelInput{OrderId{99}, OrderGeneration{1}}, reuse, warm);
                exercise(replace(99, 1, 90, 1), reuse, warm);
            }
        }
    } catch (const std::exception& error) {
        inject_failure = false;
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
