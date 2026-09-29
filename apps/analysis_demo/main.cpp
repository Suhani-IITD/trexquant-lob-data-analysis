#include "exchange/analysis/csv.hpp"
#include "exchange/engine/reference_engine.hpp"
#include <array>
#include <iostream>
#include <string_view>

using namespace exchange;
int main(int argc, char** argv) {
    const bool csv = argc == 2 && std::string_view{argv[1]} == "--csv";
    if (argc > 1 && !csv) { std::cerr << "Usage: analysis_demo [--csv]\n"; return 2; }
    try {
        const std::array ids{InstrumentId{1}};
        MarketAnalysis analysis{ids};
        ReferenceEngine engine{EngineConfig{{{ids[0], "XYZ", PriceTicks{1}, PriceTicks{1000},
            Quantity{1000}, Quantity{10000}}}, {100, 100}}};
        const auto limit = [&](std::uint64_t id, Side side, std::int64_t price, std::uint64_t qty) {
            return NewInput{OrderId{id}, ids[0], side, OrderType::Limit, TimeInForce::GTC, PriceTicks{price}, Quantity{qty}};
        };
        const std::vector<CommandInput> commands{
            limit(1, Side::Buy, 99, 10), limit(2, Side::Sell, 101, 6),
            limit(3, Side::Sell, 103, 4),
            NewInput{OrderId{4}, ids[0], Side::Buy, OrderType::Market, TimeInForce::IOC, std::nullopt, Quantity{8}},
            ReplaceInput{OrderId{1}, OrderGeneration{1}, PriceTicks{99}, Quantity{5}},
            CancelInput{OrderId{3}, OrderGeneration{3}}};
        const std::array<std::string_view, 6> descriptions{
            "Rest bid 10 @ 99", "Rest ask 6 @ 101", "Rest ask 4 @ 103",
            "Buy market IOC 8: trades 6 @ 101 and 2 @ 103",
            "Reduce bid to 5 @ 99", "Cancel remaining ask 2 @ 103"};
        if (!csv) { std::cout << "Synthetic XYZ; observations after each command, no wall-clock timestamps.\n"; }
        write_analysis_csv_header(std::cout);
        CommandResult output;
        for (std::size_t i = 0; i < commands.size(); ++i) {
            const auto sequence = static_cast<std::uint64_t>(i + 1);
            engine.process_into({EpochId{1}, IngressSequence{sequence}, SessionId{1}, CorrelationId{sequence}, commands[i]}, output);
            analysis.consume(output);
            const auto observation = analysis.observe(engine.depth_snapshot().books.front());
            if (!csv) { std::cout << "Command " << sequence << ": " << descriptions[i] << '\n'; }
            write_analysis_csv_row(std::cout, observation);
        }
        if (!csv) { std::cout << "Final: 2 trades, volume 8, VWAP 812/8 = 101.5 ticks; bid depth 5, no ask.\n"; }
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
