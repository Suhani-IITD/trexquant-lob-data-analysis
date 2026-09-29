#include "exchange/core/build_info.hpp"
#include "exchange/analysis/market_analysis.hpp"
#include "exchange/engine/reference_engine.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#ifdef EXCHANGE_ALLOCATION_DIAGNOSTICS
#include "allocation_tracking.hpp"
#endif
#ifdef EXCHANGE_ENABLE_CALLGRIND
#include <valgrind/callgrind.h>
#endif

namespace {
using namespace exchange;
using Clock = std::chrono::steady_clock;
constexpr std::string_view names[]{"add", "cancel", "match", "sweep", "reduce", "reprice", "mixed", "sustained", "diverse"};

struct Options {
    std::string workload{"all"};
    std::size_t operations{10000};
    std::size_t trials{5};
    std::size_t book_size{1000};
    bool reuse_output{false};
    std::string analysis{"off"};
    std::size_t observe_every{10000};
    std::uint64_t seed{20260928};
};

// Commands and their admission metadata are prepared before any timing begins.
struct Workload {
    std::vector<CommandEnvelope> prefill;
    std::vector<CommandEnvelope> commands;
    std::size_t expected_active{};
    std::size_t expected_trades{};
    std::uint64_t expected_volume{};
    std::vector<TradeStatistics> expected_statistics;
    std::vector<bool> selected_slots;
    std::vector<std::uint64_t> expected_bid_depth, expected_ask_depth;
};

struct Measurements {
    double seconds{};
    std::size_t trades{};
    std::uint64_t volume{};
    std::size_t rejections{};
    std::size_t observations{};
#ifdef EXCHANGE_ALLOCATION_DIAGNOSTICS
    allocation_tracking::Counts allocation_counts;
#endif
};

std::size_t number(std::string_view text, std::size_t maximum) {
    std::size_t value{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || value == 0 || value > maximum) {
        throw std::invalid_argument("Expected a positive integer no greater than " + std::to_string(maximum));
    }
    return value;
}

Options parse(int argc, char* argv[]) {
    Options options;
    for (int i = 1; i < argc; i += 2) {
        const std::string_view flag{argv[i]};
        if (i + 1 >= argc) { throw std::invalid_argument("Missing option value"); }
        const std::string_view value{argv[i + 1]};
        if (flag == "--workload") { options.workload = value; }
        else if (flag == "--operations") { options.operations = number(value, 5000000); }
        else if (flag == "--trials") { options.trials = number(value, 50); }
        else if (flag == "--book-size") { options.book_size = number(value, 100000); }
        else if (flag == "--analysis" && (value == "off" || value == "trades" || value == "snapshots" || value == "depth")) { options.analysis = value; }
        else if (flag == "--observe-every") { options.observe_every = number(value, 5000000); }
        else if (flag == "--seed") { options.seed = static_cast<std::uint64_t>(number(value, 1000000000)); }
        else if (flag == "--output" && (value == "value" || value == "reuse")) { options.reuse_output = value == "reuse"; }
        else { throw std::invalid_argument("Unknown option: " + std::string{flag}); }
    }
    if (options.workload != "all" && std::find(std::begin(names), std::end(names), options.workload) == std::end(names)) {
        throw std::invalid_argument("Unknown workload");
    }
    return options;
}

NewInput limit(std::uint64_t id, Side side, std::int64_t price, std::uint64_t quantity,
               TimeInForce tif = TimeInForce::GTC) {
    return {OrderId{id}, InstrumentId{1}, side, OrderType::Limit, tif, PriceTicks{price}, Quantity{quantity}};
}

Workload generate(std::string_view name, std::size_t count, std::size_t book_size, std::uint64_t seed) {
    Workload workload;
    std::uint64_t sequence = 0;
    const auto append = [&](std::vector<CommandEnvelope>& destination, CommandInput input) {
        ++sequence;
        destination.push_back({EpochId{1}, IngressSequence{sequence}, SessionId{1}, CorrelationId{sequence}, std::move(input)});
    };
    if (name == "diverse") {
        constexpr std::size_t instruments = 4;
        workload.expected_statistics.resize(instruments);
        workload.expected_bid_depth.resize(instruments);
        workload.expected_ask_depth.resize(instruments);
        workload.selected_slots.resize(book_size);
        std::vector<OrderGeneration> generations(book_size);
        const auto instrument = [](std::size_t slot) { return InstrumentId{static_cast<std::uint32_t>(slot % instruments + 1)}; };
        const auto side = [](std::size_t slot) { return (slot / instruments) % 2 == 0 ? Side::Buy : Side::Sell; };
        const auto price = [&](std::size_t slot) {
            const auto level = static_cast<std::int64_t>((slot / (2 * instruments)) % 100);
            return side(slot) == Side::Buy ? 900 - level : 1100 + level;
        };
        const auto order = [&](std::size_t slot) {
            auto input = limit(static_cast<std::uint64_t>(slot + 1), side(slot), price(slot), 10);
            input.instrument = instrument(slot);
            return input;
        };
        workload.prefill.reserve(book_size);
        workload.commands.reserve(count);
        for (std::size_t i = 0; i < book_size; ++i) {
            generations[i] = OrderGeneration{sequence + 1};
            append(workload.prefill, order(i));
            auto& depth = side(i) == Side::Buy ? workload.expected_bid_depth : workload.expected_ask_depth;
            depth[i % instruments] += 10;
        }
        for (std::size_t block = 0; block < count / 5; ++block) {
            // Fixed xorshift64 recipe; random selection with replacement, not a market model.
            seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
            const auto slot = static_cast<std::size_t>(seed % book_size);
            workload.selected_slots[slot] = true;
            const auto id = OrderId{static_cast<std::uint64_t>(slot + 1)};
            const auto improved = side(slot) == Side::Buy ? 1099 : 901;
            append(workload.commands, ReplaceInput{id, generations[slot], PriceTicks{price(slot)}, Quantity{8}});
            append(workload.commands, ReplaceInput{id, generations[slot], PriceTicks{improved}, Quantity{8}});
            auto taker = limit(static_cast<std::uint64_t>(book_size + block + 1),
                               side(slot) == Side::Buy ? Side::Sell : Side::Buy, improved, 3, TimeInForce::IOC);
            taker.instrument = instrument(slot);
            append(workload.commands, taker);
            append(workload.commands, CancelInput{id, generations[slot]});
            generations[slot] = OrderGeneration{sequence + 1};
            append(workload.commands, order(slot));
            auto& expected = workload.expected_statistics[slot % instruments];
            ++expected.count;
            expected.volume += 3;
            expected.notional_ticks += static_cast<std::uint64_t>(improved) * 3;
        }
        workload.expected_active = book_size;
        workload.expected_trades = count / 5;
        workload.expected_volume = static_cast<std::uint64_t>(count / 5) * 3;
        return workload;
    }
    if (name == "sustained") {
        // Rotate through existing orders. Reprice the selected ask to 99 so it
        // becomes the best maker regardless of its previous FIFO position.
        std::vector<OrderGeneration> generations(book_size);
        workload.prefill.reserve(book_size);
        workload.commands.reserve(count);
        for (std::size_t i = 0; i < book_size; ++i) {
            generations[i] = OrderGeneration{sequence + 1};
            append(workload.prefill, limit(static_cast<std::uint64_t>(i + 1), Side::Sell, 100, 10));
        }
        for (std::size_t block = 0; block < count / 5; ++block) {
            const auto slot = block % book_size;
            const auto id = OrderId{static_cast<std::uint64_t>(slot + 1)};
            append(workload.commands, ReplaceInput{id, generations[slot], PriceTicks{100}, Quantity{8}});
            append(workload.commands, ReplaceInput{id, generations[slot], PriceTicks{99}, Quantity{8}});
            append(workload.commands, limit(static_cast<std::uint64_t>(book_size + block + 1), Side::Buy, 99, 3, TimeInForce::IOC));
            append(workload.commands, CancelInput{id, generations[slot]});
            generations[slot] = OrderGeneration{sequence + 1};
            append(workload.commands, limit(id.value(), Side::Sell, 100, 10));
        }
        workload.expected_active = book_size;
        workload.expected_trades = count / 5;
        workload.expected_volume = static_cast<std::uint64_t>(count / 5) * 3;
        return workload;
    }
    if (name == "mixed") {
        // A complete five-command lifecycle ends empty, so blocks are independent.
        if (count % 5 != 0) { throw std::invalid_argument("mixed/all requires --operations divisible by 5"); }
        for (std::size_t i = 0; i < count / 5; ++i) {
            const auto id = static_cast<std::uint64_t>(2 * i + 1);
            const auto generation = OrderGeneration{sequence + 1};
            append(workload.commands, limit(id, Side::Sell, 100, 10));
            append(workload.commands, ReplaceInput{OrderId{id}, generation, PriceTicks{100}, Quantity{8}});
            append(workload.commands, limit(id + 1, Side::Buy, 100, 3, TimeInForce::IOC));
            append(workload.commands, ReplaceInput{OrderId{id}, generation, PriceTicks{101}, Quantity{5}});
            append(workload.commands, CancelInput{OrderId{id}, generation});
        }
        workload.expected_trades = count / 5;
        workload.expected_volume = static_cast<std::uint64_t>(count / 5) * 3;
        return workload;
    }
    const std::size_t makers = name == "add" ? 0 : (name == "sweep" ? 4 * count : count);
    workload.prefill.reserve(makers);
    workload.commands.reserve(count);
    for (std::size_t i = 0; i < makers; ++i) {
        // Sweep: one order per level; each market order consumes four prices.
        const auto price = name == "sweep" ? 100 + static_cast<std::int64_t>(i) : 100;
        append(workload.prefill, limit(static_cast<std::uint64_t>(i + 1), Side::Sell, price, 10));
    }
    for (std::size_t i = 0; i < count; ++i) {
        const auto id = static_cast<std::uint64_t>(i + 1);
        if (name == "add") {
            append(workload.commands, limit(id, Side::Buy, 90 + static_cast<std::int64_t>(i % 10), 10));
        } else if (name == "cancel") {
            append(workload.commands, CancelInput{OrderId{id}, OrderGeneration{id}});
        } else if (name == "reduce" || name == "reprice") {
            append(workload.commands, ReplaceInput{OrderId{id}, OrderGeneration{id},
                PriceTicks{name == "reduce" ? 100 : 101}, Quantity{name == "reduce" ? 5U : 10U}});
        } else if (name == "match") {
            append(workload.commands, limit(static_cast<std::uint64_t>(makers) + id, Side::Buy, 100, 10, TimeInForce::IOC));
        } else {
            append(workload.commands, NewInput{OrderId{static_cast<std::uint64_t>(makers) + id}, InstrumentId{1},
                Side::Buy, OrderType::Market, TimeInForce::IOC, std::nullopt, Quantity{40}});
        }
    }
    workload.expected_active = name == "add" || name == "reduce" || name == "reprice" ? count : 0;
    workload.expected_trades = name == "match" ? count : (name == "sweep" ? 4 * count : 0);
    workload.expected_volume = static_cast<std::uint64_t>(workload.expected_trades) * 10;
    return workload;
}

// A named boundary makes the measured loop easy to locate in CPU call graphs.
void execute_commands(ReferenceEngine& engine, const Workload& workload, Measurements& measurement, const Options& options, MarketAnalysis* analysis) {
    CommandResult storage;
    std::size_t processed = 0;
    for (const auto& command : workload.commands) {
        CommandResult owned;
        if (options.reuse_output) { engine.process_into(command, storage); }
        else { owned = engine.process(command); }
        const auto& result = options.reuse_output ? storage : owned;
        ++processed;
        if (analysis) {
            analysis->consume(result);
            if ((options.analysis == "snapshots" || options.analysis == "depth") &&
                (processed % options.observe_every == 0 || processed == workload.commands.size())) {
                const auto observe = [&](const auto& snapshot) {
                    for (const auto& book : snapshot.books) {
                        const auto observed = analysis->observe(book);
                        if (observed.command_sequence != command.ingress) { throw std::runtime_error("Wrong observation sequence"); }
                        ++measurement.observations;
                    }
                };
                if (options.analysis == "depth") { observe(engine.depth_snapshot()); }
                else { observe(engine.snapshot()); }
            }
        }
        for (const auto& report : result.reports) { measurement.rejections += report.rejection.has_value() ? 1U : 0U; }
        for (const auto& event : result.events) {
            if (const auto* trade = std::get_if<TradeEvent>(&event.event)) {
                ++measurement.trades;
                measurement.volume += trade->executed.value();
            }
        }
    }
}

Measurements run_trial(const Workload& workload, const Options& options, [[maybe_unused]] bool profile = true) {
    const auto capacity = workload.prefill.size() + workload.commands.size() + 1;
    EngineConfig config{{{InstrumentId{1}, "XYZ", PriceTicks{1}, PriceTicks{1000000},
                           Quantity{100}, Quantity{10000000}}}, {capacity, capacity}};
    std::vector<InstrumentId> instruments{InstrumentId{1}};
    if (!workload.expected_statistics.empty()) {
        for (std::uint32_t id = 2; id <= 4; ++id) {
            auto instrument = config.instruments.front();
            instrument.id = InstrumentId{id};
            instrument.symbol = "SYN" + std::to_string(id);
            config.instruments.push_back(instrument);
            instruments.push_back(instrument.id);
        }
    }
    ReferenceEngine engine{std::move(config)};
    std::optional<MarketAnalysis> analysis;
    if (options.analysis != "off") { analysis.emplace(instruments); }
    for (const auto& command : workload.prefill) {
        const auto result = engine.process(command);
        if (analysis) { analysis->consume(result); }
        for (const auto& report : result.reports) {
            if (report.rejection) { throw std::runtime_error("Prefill was rejected"); }
        }
    }
    engine.validate();
    Measurements measurement;
#ifdef EXCHANGE_ENABLE_CALLGRIND
    // Run Callgrind with --instr-atstart=no; excludes setup calls as well as costs.
    if (profile) { CALLGRIND_START_INSTRUMENTATION; }
#endif
    const auto start = Clock::now();
    {
#ifdef EXCHANGE_ALLOCATION_DIAGNOSTICS
        allocation_tracking::Scope tracking;
#endif
        execute_commands(engine, workload, measurement, options, analysis ? &*analysis : nullptr);
#ifdef EXCHANGE_ALLOCATION_DIAGNOSTICS
        measurement.allocation_counts = allocation_tracking::counts;
#endif
    }
    measurement.seconds = std::chrono::duration<double>(Clock::now() - start).count();
#ifdef EXCHANGE_ENABLE_CALLGRIND
    if (profile) { CALLGRIND_STOP_INSTRUMENTATION; }
#endif
    // Small analytical workload checks, outside timing; no replay or second book.
    engine.validate();
    if (measurement.rejections != 0 || measurement.trades != workload.expected_trades ||
        measurement.volume != workload.expected_volume || engine.active_orders() != workload.expected_active) {
        throw std::runtime_error("Workload outcome differs from its analytical expectation");
    }
    if (analysis) {
        const auto final = engine.snapshot();
        const auto final_depth = engine.depth_snapshot();
        std::uint64_t count = 0, volume = 0;
        for (std::size_t i = 0; i < instruments.size(); ++i) {
            const auto& stats = analysis->trades(instruments[i]);
            count += stats.count; volume += stats.volume;
            if (analysis->observe(final.books[i]) != analysis->observe(final_depth.books[i])) {
                throw std::runtime_error("Full/depth observations differ");
            }
            if (!workload.expected_statistics.empty()) {
                const auto observed = analysis->observe(final.books[i]);
                if (stats != workload.expected_statistics[i] ||
                    observed.bid_depth != workload.expected_bid_depth[i] ||
                    observed.ask_depth != workload.expected_ask_depth[i]) {
                    throw std::runtime_error("Per-instrument analysis differs from expected totals/depth");
                }
            }
        }
        if (count != workload.expected_trades || volume != workload.expected_volume) {
            throw std::runtime_error("Analysis trade totals differ");
        }
        const auto expected_observations = (options.analysis == "snapshots" || options.analysis == "depth")
            ? ((workload.commands.size() - 1) / options.observe_every + 1) * instruments.size() : 0;
        if (measurement.observations != expected_observations) { throw std::runtime_error("Observation count differs"); }
    }
    if (measurement.seconds <= 0) { throw std::runtime_error("Trial too short for clock resolution"); }
    return measurement;
}

void benchmark(std::string_view name, const Options& options) {
    const auto workload = generate(name, options.operations, options.book_size, options.seed);
    if (!workload.selected_slots.empty()) {
        std::cout << "# diverse selected_slots=" << std::count(workload.selected_slots.begin(), workload.selected_slots.end(), true)
                  << " instruments=4 maximum_levels_per_side_per_instrument=100\n";
        for (std::size_t i = 0; i < workload.expected_statistics.size(); ++i) {
            const auto& stats = workload.expected_statistics[i];
            std::cout << "# expected instrument=" << i + 1 << " trades=" << stats.count << " volume=" << stats.volume
                      << " notional_ticks=" << stats.notional_ticks << '\n';
        }
    }
    (void)run_trial(workload, options, false); // Unreported fresh-engine warm-up; not profiled.
    std::vector<double> durations;
    durations.reserve(options.trials);
    for (std::size_t trial = 1; trial <= options.trials; ++trial) {
        const auto measurement = run_trial(workload, options);
        durations.push_back(measurement.seconds);
        const auto commands = static_cast<double>(workload.commands.size());
        std::cout << name << ',' << trial << ',' << workload.commands.size() << ',' << workload.prefill.size()
                  << ',' << workload.expected_active << ',' << measurement.trades << ',' << measurement.volume
                  << ',' << measurement.rejections << ',' << measurement.observations;
#ifdef EXCHANGE_ALLOCATION_DIAGNOSTICS
        std::cout << ',' << measurement.allocation_counts.allocations << ',' << measurement.allocation_counts.requested_bytes
                  << ',' << measurement.allocation_counts.deallocations
                  << ',' << static_cast<double>(measurement.allocation_counts.allocations) / commands
                  << ',' << static_cast<double>(measurement.allocation_counts.requested_bytes) / commands << '\n';
#else
        std::cout << ',' << measurement.seconds * 1000.0
                  << ',' << commands / measurement.seconds << ',' << measurement.seconds * 1e9 / commands << '\n';
#endif
    }
#ifndef EXCHANGE_ALLOCATION_DIAGNOSTICS
    std::sort(durations.begin(), durations.end());
    const auto middle = durations.size() / 2;
    const auto median = durations.size() % 2 == 0 ? (durations[middle - 1] + durations[middle]) / 2 : durations[middle];
    std::cout << "# " << name << " batch_ms min=" << durations.front() * 1000.0
              << " median=" << median * 1000.0 << " max=" << durations.back() * 1000.0 << '\n';
#endif
}
} // namespace

int main(int argc, char* argv[]) {
    if (argc == 2 && std::string_view{argv[1]} == "--help") {
        std::cout << "Usage: benchmark_driver (or allocation_driver) [--workload all|add|cancel|match|sweep|reduce|reprice|mixed|sustained|diverse]\n"
                     "                        [--operations 10000] [--trials 5] [--book-size 1000] [--output value|reuse]\n"
                     "Operations: up to 5000000 for diverse (200000 for other workloads); multiple of 5 for block workloads; trials: 1..50; book-size: 1..100000.\n"
                     "                        [--analysis off|trades|snapshots|depth] [--observe-every 10000] [--seed 20260928]\n"
                     "Use unsanitized Release for measurements; small counts for Debug checks.\n";
        return 0;
    }
    try {
        const auto options = parse(argc, argv);
        // Validate before printing results or running a partial 'all' suite.
        if ((options.workload == "all" || options.workload == "mixed" || options.workload == "sustained" || options.workload == "diverse") && options.operations % 5 != 0) {
            throw std::invalid_argument("mixed/sustained/diverse/all requires --operations divisible by 5");
        }
        if (options.operations > 200000 && options.workload != "diverse") {
            throw std::invalid_argument("More than 200000 commands requires diverse workload");
        }
        std::cout << "# analysis=" << options.analysis << " observe_every=" << options.observe_every
                  << " seed=" << options.seed << '\n';
        std::cout << "# output=" << (options.reuse_output ? "reuse" : "value") << '\n';
        const auto info = build_info();
#ifdef EXCHANGE_REUSE_FILL_BUFFER
        std::cout << "# fill_buffer=reused (retains per-engine high-water capacity)\n";
#else
        std::cout << "# fill_buffer=per_command (baseline)\n";
#endif
        std::cout << "# version=" << info.version << " compiler=" << info.compiler << " build=" << info.configuration
                  << " workload_version=3 clock=steady_clock warmup_trials=1 book_size=" << options.book_size << '\n';
#ifndef NDEBUG
        std::cout << "# Debug invariants enabled: these timings are diagnostic, not a Release baseline.\n";
#endif
        std::cout << "# Excludes generation/prefill/final validation; includes process, result disposal, counters and selected analysis (no CSV I/O).\n"
#ifdef EXCHANGE_ALLOCATION_DIAGNOSTICS
                     "# Ordinary C++ new/delete only; requested bytes are cumulative, not peak memory. Timings intentionally omitted.\n"
                     "workload,trial,commands,initial_orders,final_orders,trades,volume,rejections,observations,allocations,requested_bytes,deallocations,allocations_per_command,bytes_per_command\n"
#else
                     "workload,trial,commands,initial_orders,final_orders,trades,volume,rejections,observations,elapsed_ms,commands_per_second,mean_ns_per_command\n"
#endif
                  << std::fixed << std::setprecision(3);
        if (options.workload == "all") {
            for (const auto name : names) { benchmark(name, options); }
        } else { benchmark(options.workload, options); }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "benchmark_driver: " << error.what() << '\n';
        return 1;
    }
}
