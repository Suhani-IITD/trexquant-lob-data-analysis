#include "exchange/engine/reference_engine.hpp"
#include "exchange/analysis/csv.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <charconv>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
using namespace exchange;

template <typename T>
T integer(const std::string& text) {
    T value{};
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
        throw std::runtime_error("Invalid integer: " + text);
    }
    return value;
}

void require(bool condition, const std::string& description) {
    if (!condition) {
        throw std::runtime_error(description);
    }
}

std::string_view reason_name(RejectReason reason) {
    switch (reason) {
    case RejectReason::InvalidField: return "INVALID_FIELD";
    case RejectReason::InvalidInstrument: return "INVALID_INSTRUMENT";
    case RejectReason::InvalidPrice: return "INVALID_PRICE";
    case RejectReason::InvalidQuantity: return "INVALID_QUANTITY";
    case RejectReason::DuplicateOrderId: return "DUPLICATE_ORDER_ID";
    case RejectReason::UnknownOrder: return "UNKNOWN_ORDER";
    case RejectReason::StaleOrder: return "STALE_ORDER";
    case RejectReason::NotOwner: return "NOT_OWNER";
    case RejectReason::ResourceExhausted: return "RESOURCE_EXHAUSTED";
    case RejectReason::UnsupportedOrderType: return "UNSUPPORTED_ORDER_TYPE";
    }
    throw std::logic_error("Unknown internal rejection code");
}

int run(const std::string& path, const std::optional<std::string>& csv_path, std::uint64_t cadence) {
    std::ifstream input{path};
    require(input.is_open(), "Cannot open scenario: " + path);
    EngineConfig config{{{InstrumentId{1}, "XYZ", PriceTicks{1}, PriceTicks{10000},
                           Quantity{1000000}, Quantity{10000000}}}, {10000, 10000}};
    AdmissionDriver driver{std::move(config)};
    const std::array instruments{InstrumentId{1}};
    std::optional<MarketAnalysis> analysis;
    std::ofstream csv;
    if (csv_path) {
        // Protect the source even when the output name is a symlink/hard link.
        std::error_code error;
        const bool same_file = std::filesystem::equivalent(path, *csv_path, error);
        require(!same_file, "Analysis output must not overwrite scenario input");
        csv.open(*csv_path);
        require(csv.is_open(), "Cannot open analysis CSV: " + *csv_path);
        analysis.emplace(instruments);
        write_analysis_csv_header(csv);
    }
    std::uint64_t last_observed = 0;
    const auto observe = [&](const auto& snapshot) {
        for (const auto& book : snapshot.books) {
            write_analysis_csv_row(csv, analysis->observe(book));
        }
        last_observed = analysis->last_ingress().value();
    };
    std::optional<CommandResult> last;
    std::uint64_t commands = 0;
    std::size_t expectations = 0;
    std::size_t line_number = 0;
    bool header = false;
    for (std::string line; std::getline(input, line);) {
        ++line_number;
        line = line.substr(0, line.find('#'));
        std::istringstream stream{line};
        std::vector<std::string> words;
        for (std::string word; stream >> word;) {
            words.push_back(std::move(word));
        }
        if (words.empty()) {
            continue;
        }
        try {
            if (!header) {
                require(words == std::vector<std::string>{"SCENARIO", "1"}, "Expected SCENARIO 1 header");
                header = true;
                continue;
            }
            const auto& operation = words.front();
            if (operation == "NEW") {
                require(words.size() == 8, "NEW requires id instrument side type tif price quantity");
                require(words[3] == "BUY" || words[3] == "SELL", "Invalid side token");
                require(words[4] == "LIMIT" || words[4] == "MARKET", "Invalid type token");
                require(words[5] == "GTC" || words[5] == "IOC", "Invalid time-in-force token");
                std::optional<PriceTicks> price;
                if (words[6] != "-") {
                    price = PriceTicks{integer<std::int64_t>(words[6])};
                }
                NewInput request{OrderId{integer<std::uint64_t>(words[1])},
                                 InstrumentId{integer<std::uint32_t>(words[2])},
                                 words[3] == "BUY" ? Side::Buy : Side::Sell,
                                 words[4] == "LIMIT" ? OrderType::Limit : OrderType::Market,
                                 words[5] == "GTC" ? TimeInForce::GTC : TimeInForce::IOC,
                                 price, Quantity{integer<std::uint64_t>(words[7])}};
                last = driver.submit(SessionId{1}, CorrelationId{++commands}, request);
            } else if (operation == "CANCEL") {
                require(words.size() == 3, "CANCEL requires id generation");
                last = driver.submit(SessionId{1}, CorrelationId{++commands},
                    CancelInput{OrderId{integer<std::uint64_t>(words[1])},
                                OrderGeneration{integer<std::uint64_t>(words[2])}});
            } else if (operation == "REPLACE") {
                // Phase 2: the fixture names the existing lifecycle, then its desired
                // price and remaining quantity; it cannot change instrument or side.
                require(words.size() == 5, "REPLACE requires id generation price remaining");
                last = driver.submit(SessionId{1}, CorrelationId{++commands},
                    ReplaceInput{OrderId{integer<std::uint64_t>(words[1])},
                                 OrderGeneration{integer<std::uint64_t>(words[2])},
                                 PriceTicks{integer<std::int64_t>(words[3])},
                                 Quantity{integer<std::uint64_t>(words[4])}});
            } else if (operation == "EXPECT_MARKET_SEQUENCE") {
                require(words.size() == 2, "EXPECT_MARKET_SEQUENCE requires sequence");
                require(driver.engine().last_market().value() == integer<std::uint64_t>(words[1]),
                        "Market sequence mismatch");
                ++expectations;
                continue;
            } else if (operation == "EXPECT_ACTIVE") {
                require(words.size() == 2, "EXPECT_ACTIVE requires count");
                require(driver.engine().active_orders() == integer<std::size_t>(words[1]),
                        "Active count mismatch");
                ++expectations;
                continue;
            } else if (operation == "EXPECT_EMPTY") {
                require(words.size() == 1 && driver.engine().active_orders() == 0, "Expected empty book");
                ++expectations;
                continue;
            } else if (operation == "EXPECT_REJECT") {
                require(words.size() == 2 && last.has_value(), "EXPECT_REJECT requires reason and prior command");
                require(last->reports.size() == 1 && last->reports.front().rejection.has_value(),
                        "Previous command was not rejected");
                require(reason_name(*last->reports.front().rejection) == words[1], "Rejection mismatch");
                ++expectations;
                continue;
            } else if (operation == "EXPECT_BEST_ASK" || operation == "EXPECT_BEST_BID") {
                require(words.size() == 4, "Quote expectation requires instrument price total");
                const auto state = driver.engine().snapshot();
                const auto instrument = InstrumentId{integer<std::uint32_t>(words[1])};
                const auto found = std::find_if(state.books.begin(), state.books.end(), [&](const auto& book) {
                    return book.instrument == instrument;
                });
                require(found != state.books.end(), "Expected quote on unknown instrument");
                const auto& levels = operation == "EXPECT_BEST_ASK" ? found->asks : found->bids;
                require(!levels.empty() && levels.front().price.value() == integer<std::int64_t>(words[2]) &&
                        levels.front().total.value() == integer<std::uint64_t>(words[3]), "Best quote mismatch");
                ++expectations;
                continue;
            } else {
                throw std::runtime_error("Unknown operation: " + operation);
            }
            driver.engine().validate();
            if (analysis) {
                analysis->consume(*last);
                if (commands % cadence == 0) { observe(driver.engine().depth_snapshot()); }
            }
            std::cout << "ingress=" << last->ingress.value() << " reports=" << last->reports.size()
                      << " events=" << last->events.size() << '\n';
            for (const auto& event : last->events) {
                if (const auto* trade = std::get_if<TradeEvent>(&event.event)) {
                    std::cout << "trade maker=" << trade->maker.id.value() << " taker=" << trade->taker.id.value()
                              << " price_ticks=" << trade->price.value() << " quantity=" << trade->executed.value() << '\n';
                }
            }
        } catch (const std::exception& error) {
            throw std::runtime_error("line " + std::to_string(line_number) + ": " + error.what());
        }
    }
    require(!input.bad(), "I/O failure while reading scenario");
    require(header && commands > 0, "Scenario needs a header and commands");
    // Simulation output is useful without EXPECT directives or a verification pipeline.
    const auto final_state = driver.engine().snapshot();
    if (analysis) {
        if (last_observed != commands) { observe(final_state); }
        csv.close();
        require(!csv.fail(), "Cannot finish analysis CSV: " + *csv_path);
        const auto& trades = analysis->trades(InstrumentId{1});
        std::cout << "ANALYSIS instrument=1 trades=" << trades.count << " volume=" << trades.volume;
        if (const auto vwap = trades.vwap_ticks()) { std::cout << " vwap_ticks=" << *vwap; }
        else { std::cout << " vwap_ticks=NA"; }
        std::cout << " observation_interval=" << cadence << " csv=" << *csv_path << '\n';
    }
    for (const auto& book : final_state.books) {
        std::cout << "FINAL BOOK instrument=" << book.instrument.value() << '\n';
        for (const auto* levels : {&book.bids, &book.asks}) {
            std::cout << (levels == &book.bids ? "BIDS" : "ASKS") << '\n';
            if (levels->empty()) {
                std::cout << "  empty\n";
            }
            for (const auto& level : *levels) {
                std::cout << "  price_ticks=" << level.price.value() << " total=" << level.total.value()
                          << " FIFO:";
                for (const auto& order : level.fifo) {
                    std::cout << ' ' << order.id.value() << ':' << order.remaining.value();
                }
                std::cout << '\n';
            }
        }
    }
    std::cout << "expectations_checked=" << expectations << '\n';
    std::cout << "SCENARIO PASS commands=" << commands << " market_sequence=" << driver.engine().last_market().value() << '\n';
    return 0;
}
} // namespace

int main(int argc, char* argv[]) {
    constexpr std::string_view usage =
        "Usage: scenario_runner --input <scenario.txt> [--analysis-csv <output.csv>] "
        "[--observe-every <positive command count>]\n";
    if (argc == 2 && std::string_view{argv[1]} == "--help") {
        std::cout << usage;
        return 0;
    }
    std::optional<std::string> input, csv;
    std::uint64_t cadence = 1;
    bool cadence_set = false;
    try {
        for (int i = 1; i < argc; i += 2) {
            require(i + 1 < argc, "Missing option value");
            const std::string_view flag{argv[i]};
            if (flag == "--input" && !input) { input = argv[i + 1]; }
            else if (flag == "--analysis-csv" && !csv) { csv = argv[i + 1]; }
            else if (flag == "--observe-every" && !cadence_set) {
                cadence = integer<std::uint64_t>(argv[i + 1]);
                require(cadence > 0, "Observation interval must be positive");
                cadence_set = true;
            } else { throw std::runtime_error("Unknown or duplicate option"); }
        }
        require(input.has_value(), "Input scenario is required");
        require(!cadence_set || csv.has_value(), "--observe-every requires --analysis-csv");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n' << usage;
        return 2;
    }
    try {
        return run(*input, csv, cadence);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
