#include "exchange/analysis/csv.hpp"
#include <iomanip>
#include <locale>
#include <ostream>
#include <sstream>

namespace exchange {
namespace {
void write(std::ostream& output, const std::string& text) {
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!output) { throw std::ios_base::failure("Cannot write analysis CSV"); }
}
}
void write_analysis_csv_header(std::ostream& output) {
    write(output, "command_sequence,instrument,trade_count,volume,vwap_ticks,best_bid_ticks,"
                  "best_ask_ticks,spread_ticks,midpoint_ticks,bid_depth,ask_depth,top_imbalance\n");
}
void write_analysis_csv_row(std::ostream& output, const BookObservation& o) {
    std::ostringstream row;
    row.imbue(std::locale::classic());
    row << std::fixed << std::setprecision(6);
    const auto optional = [&](const std::optional<long double>& value) {
        if (value) { row << *value; }
    };
    row << o.command_sequence.value() << ',' << o.instrument.value() << ',' << o.trades.count
        << ',' << o.trades.volume << ',';
    optional(o.trades.vwap_ticks());
    row << ',';
    if (o.best_bid) { row << o.best_bid->value(); }
    row << ',';
    if (o.best_ask) { row << o.best_ask->value(); }
    row << ','; optional(o.spread_ticks);
    row << ','; optional(o.midpoint_ticks);
    row << ',' << o.bid_depth << ',' << o.ask_depth << ',';
    optional(o.top_imbalance);
    row << '\n';
    if (!row) { throw std::ios_base::failure("Cannot format analysis CSV"); }
    write(output, row.str());
}
} // namespace exchange
