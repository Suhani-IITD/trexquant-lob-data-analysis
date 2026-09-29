#include "phase2_test_support.hpp"
#include <iostream>

using namespace phase2_tests;
int main() {
    try {
        ReferenceEngine value{configuration()}, reused{configuration()};
        CommandResult output;
        const std::vector<CommandInput> inputs{
            limit(1, Side::Buy, 90, 5), limit(2, Side::Sell, 100, 3),
            limit(3, Side::Sell, 101, 4), replace(1, 1, 90, 5),
            replace(1, 1, 90, 2), replace(1, 1, 101, 10),
            limit(1, Side::Buy, 80, 1), CancelInput{OrderId{1}, OrderGeneration{99}},
            CancelInput{OrderId{1}, OrderGeneration{1}}, market(4, Side::Buy, 10),
            limit(5, Side::Sell, 102, 8), limit(6, Side::Buy, 102, 3, TimeInForce::IOC),
            replace(5, 11, 102, 9), replace(5, 11, 99, 9),
            CancelInput{OrderId{5}, OrderGeneration{11}}, limit(0, Side::Buy, 1, 1)};
        for (const auto& input : inputs) {
            const auto next = value.last_ingress().value() + 1;
            const CommandEnvelope command{EpochId{1}, IngressSequence{next}, SessionId{1}, CorrelationId{next}, input};
            const auto expected = value.process(command);
            reused.process_into(command, output);
            check(output == expected, "full reports/events/watermarks differ across APIs");
            check(value.snapshot() == reused.snapshot(), "books differ across APIs");
            reused.validate();
        }
        // Deterministic mixed requests include invalid fields, stale identities,
        // wrong owners, IOC expiry and different queue shapes.
        std::uint64_t random = 17;
        for (std::size_t i = 0; i < 1000; ++i) {
            random = random * 6364136223846793005ULL + 1;
            const auto id = random % 20 + 1;
            const auto next = value.last_ingress().value() + 1;
            CommandInput input = limit(id, (random & 32U) ? Side::Buy : Side::Sell,
                                       98 + static_cast<std::int64_t>(random % 5), random % 10 + 1,
                                       (random & 64U) ? TimeInForce::IOC : TimeInForce::GTC);
            const auto snapshot = value.snapshot();
            std::uint64_t generation = 1;
            for (const auto& book : snapshot.books) {
                for (const auto* levels : {&book.bids, &book.asks}) {
                    for (const auto& level : *levels) {
                        for (const auto& order : level.fifo) {
                            if (order.id == OrderId{id}) { generation = order.generation.value(); }
                        }
                    }
                }
            }
            if (random % 3 == 0) { input = replace(id, generation, 99, random % 12 + 1); }
            if (random % 3 == 1) { input = CancelInput{OrderId{id}, OrderGeneration{generation}}; }
            const CommandEnvelope command{EpochId{1}, IngressSequence{next},
                SessionId{random % 7 == 0 ? 2U : 1U}, CorrelationId{next}, input};
            const auto expected = value.process(command);
            reused.process_into(command, output);
            check(output == expected && value.snapshot() == reused.snapshot(), "mixed API equivalence");
        }
        const auto saved = output;
        bool invalid = false;
        try { reused.process_into({EpochId{1}, IngressSequence{1}, SessionId{1}, CorrelationId{1}, inputs.front()}, output); }
        catch (const std::invalid_argument&) { invalid = true; }
        check(invalid && output == saved, "invalid envelope must preserve caller output");
        check(output.reports.capacity() >= 5 && output.events.capacity() >= 3, "reuse retains high-water capacity");
        // Alternate APIs on the same engine; scratch and output ownership are independent.
        const auto expected = submit(value, market(99, Side::Sell, 1));
        check(submit(reused, market(99, Side::Sell, 1)) == expected, "alternating APIs");
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
