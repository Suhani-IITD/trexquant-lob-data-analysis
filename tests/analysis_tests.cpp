#include "exchange/analysis/csv.hpp"
#include "phase2_test_support.hpp"
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <locale>
#include <sstream>
using namespace phase2_tests;
namespace {
void near(std::optional<long double> actual, long double expected) {
    check(actual && std::abs(*actual - expected) < 1e-12L, "derived metric differs");
}
template<class Exception, class F> void throws(F action) {
    bool caught = false;
    try { action(); } catch (const Exception&) { caught = true; }
    check(caught, "expected analysis error");
}
CommandResult trade_result(std::uint64_t sequence, std::uint64_t qty, std::int64_t price = 1) {
    CommandResult result{};
    result.ingress = IngressSequence{sequence};
    result.events.push_back({MarketSequence{sequence}, result.ingress, InstrumentId{1},
        TradeEvent{{OrderId{1}, OrderGeneration{1}}, {OrderId{2}, OrderGeneration{2}}, PriceTicks{price}, Quantity{qty}}});
    return result;
}
void metrics() {
    const std::array ids{InstrumentId{1}, InstrumentId{2}};
    MarketAnalysis analysis{ids};
    auto config = configuration();
    config.instruments.push_back({ids[1], "ABC", PriceTicks{1}, PriceTicks{10000}, Quantity{1000}, Quantity{10000}});
    ReferenceEngine engine{config};
    auto empty = analysis.observe(engine.snapshot().books[0]);
    check(!empty.best_bid && !empty.best_ask && !empty.spread_ticks && !empty.midpoint_ticks &&
          !empty.top_imbalance && !empty.trades.vwap_ticks(), "empty metrics absent");
    const auto consume = [&](CommandInput input) {
        analysis.consume(submit(engine, std::move(input)));
        const auto full = engine.snapshot();
        const auto depth = engine.depth_snapshot();
        check(full.books.size() == depth.books.size(), "snapshot instrument count");
        for (std::size_t i = 0; i < full.books.size(); ++i) {
            check(analysis.observe(full.books[i]) == analysis.observe(depth.books[i]), "full/depth observation equality");
            for (bool bids : {false, true}) {
                const auto& full_levels = bids ? full.books[i].bids : full.books[i].asks;
                const auto& depth_levels = bids ? depth.books[i].bids : depth.books[i].asks;
                check(full_levels.size() == depth_levels.size(), "depth snapshot level count");
                for (std::size_t j = 0; j < full_levels.size(); ++j) {
                    check(full_levels[j].price == depth_levels[j].price && full_levels[j].total == depth_levels[j].total,
                          "depth snapshot preserves every price and quantity in order");
                }
            }
        }
    };
    consume(limit(1, Side::Buy, 99, 10));
    auto one = analysis.observe(engine.snapshot().books[0]);
    near(one.top_imbalance, 1);
    check(!one.spread_ticks && !one.midpoint_ticks && one.bid_depth == 10, "one-sided book");
    consume(limit(2, Side::Sell, 101, 6));
    consume(limit(3, Side::Sell, 103, 4));
    auto before = analysis.observe(engine.snapshot().books[0]);
    near(before.spread_ticks, 2); near(before.midpoint_ticks, 100); near(before.top_imbalance, 0.25L);
    check(before.bid_depth == 10 && before.ask_depth == 10, "all-level depth versus top imbalance");
    consume(market(4, Side::Buy, 8));
    auto filled = analysis.observe(engine.snapshot().books[0]);
    check(filled.trades == TradeStatistics{2, 8, 812} && filled.command_sequence == IngressSequence{4}, "trade totals count once per event");
    near(filled.trades.vwap_ticks(), 101.5L); near(filled.spread_ticks, 4);
    near(filled.midpoint_ticks, 101); near(filled.top_imbalance, 2.0L/3);
    check(filled.ask_depth == 2 && filled.best_ask == PriceTicks{103}, "post-sweep observation");
    consume(replace(1, 1, 99, 5));
    consume(CancelInput{OrderId{3}, OrderGeneration{3}});
    consume(limit(0, Side::Buy, 1, 1)); // rejection still consumes sequence
    consume(market(8, Side::Buy, 10)); // expiry emits no trades
    check(analysis.trades(ids[0]) == filled.trades, "non-trades leave trade totals unchanged");
    auto other = limit(9, Side::Sell, 80, 3); other.instrument = ids[1]; consume(other);
    auto taker = market(10, Side::Buy, 2); taker.instrument = ids[1]; consume(taker);
    check(analysis.trades(ids[1]) == TradeStatistics{1, 2, 160} && analysis.trades(ids[0]) == filled.trades, "instrument isolation");
    near(analysis.observe(engine.snapshot().books[1]).top_imbalance, -1);
}
void boundaries() {
    const std::array ids{InstrumentId{1}};
    MarketAnalysis analysis{ids};
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    throws<std::overflow_error>([&] { analysis.consume(trade_result(1, maximum, 2)); });
    check(analysis.last_ingress() == IngressSequence{} && analysis.trades(ids[0]) == TradeStatistics{}, "multiply overflow atomic");
    auto bad = trade_result(1, 1); bad.events.push_back(trade_result(1, maximum, 2).events.front());
    throws<std::overflow_error>([&] { analysis.consume(bad); });
    check(analysis.trades(ids[0]) == TradeStatistics{}, "late event overflow atomic");
    analysis.consume(trade_result(1, maximum));
    throws<std::overflow_error>([&] { analysis.consume(trade_result(2, 1)); });
    check(analysis.trades(ids[0]) == TradeStatistics{1, maximum, maximum} && analysis.last_ingress() == IngressSequence{1}, "cumulative overflow atomic");
    throws<std::invalid_argument>([&] { analysis.consume(trade_result(1, 1)); });
    throws<std::invalid_argument>([&] { analysis.consume(trade_result(3, 1)); });
    bad = trade_result(2, 1); bad.events.front().instrument = InstrumentId{9};
    throws<std::invalid_argument>([&] { analysis.consume(bad); });
    throws<std::invalid_argument>([&] { analysis.consume(trade_result(2, 0)); });
    throws<std::invalid_argument>([&] { analysis.consume(trade_result(2, 1, -1)); });
    BookSnapshot book{ids[0], {{Side::Buy, PriceTicks{1}, Quantity{maximum}, {}}},
        {{Side::Sell, PriceTicks{std::numeric_limits<std::int64_t>::max()}, Quantity{maximum}, {}}}};
    near(analysis.observe(book).top_imbalance, 0); // sum of top quantities would overflow uint64
    book.bids.push_back({Side::Buy, PriceTicks{1}, Quantity{1}, {}});
    throws<std::overflow_error>([&] { (void)analysis.observe(book); });
    const std::array duplicates{ids[0], ids[0]};
    throws<std::invalid_argument>([&] { MarketAnalysis invalid{duplicates}; });
    throws<std::invalid_argument>([&] { MarketAnalysis invalid{std::span<const InstrumentId>{}}; });
}
void multiple_instrument_rollback() {
    // Deliberately unsorted IDs exercise lookup-to-storage indexing.
    const std::array ids{InstrumentId{2}, InstrumentId{1}};
    MarketAnalysis analysis{ids};
    auto result = trade_result(1, 3, 100);
    result.events.front().instrument = ids[0];
    result.events.push_back(trade_result(1, std::numeric_limits<std::uint64_t>::max(), 2).events.front());
    throws<std::overflow_error>([&] { analysis.consume(result); });
    check(analysis.trades(ids[0]) == TradeStatistics{} && analysis.trades(ids[1]) == TradeStatistics{},
          "late overflow must roll back every instrument");
    result.events.back() = trade_result(1, 4, 100).events.front();
    analysis.consume(result);
    check(analysis.trades(ids[0]) == TradeStatistics{1, 3, 300} &&
          analysis.trades(ids[1]) == TradeStatistics{1, 4, 400}, "retry must reset dirty transaction scratch");
}
struct CommaDecimal : std::numpunct<char> {
    char do_decimal_point() const override { return ','; }
    char do_thousands_sep() const override { return '.'; }
    std::string do_grouping() const override { return "\3"; }
};
void csv_format() {
    BookObservation observation{};
    observation.command_sequence = IngressSequence{1234};
    observation.instrument = InstrumentId{1};
    observation.trades = {2, 8, 812};
    observation.best_bid = PriceTicks{99};
    observation.bid_depth = 10;
    observation.top_imbalance = 1;
    std::ostringstream plain, localized;
    localized.imbue(std::locale{std::locale::classic(), new CommaDecimal});
    localized.precision(2);
    localized.width(20);
    localized.setf(std::ios_base::scientific, std::ios_base::floatfield);
    const auto flags = localized.flags();
    write_analysis_csv_header(plain);
    write_analysis_csv_row(plain, observation);
    write_analysis_csv_header(localized);
    write_analysis_csv_row(localized, observation);
    check(plain.str() == localized.str(), "CSV must ignore caller locale/formatting");
    check(localized.precision() == 2 && localized.width() == 20 && localized.flags() == flags,
          "CSV must preserve caller formatting");
    check(plain.str().ends_with("1234,1,2,8,101.500000,99,,,,10,0,1.000000\n"),
          "CSV exact values and absent fields");
    std::ostream failed{nullptr};
    throws<std::ios_base::failure>([&] { write_analysis_csv_header(failed); });
    throws<std::ios_base::failure>([&] { write_analysis_csv_row(failed, observation); });
}

}
int main() {
    try { metrics(); boundaries(); csv_format(); multiple_instrument_rollback(); }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
