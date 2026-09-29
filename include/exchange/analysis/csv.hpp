#pragma once

#include "exchange/analysis/market_analysis.hpp"
#include <iosfwd>

namespace exchange {
// Stable column order, classic locale, six fractional digits, blank absent values.
// These writers do not change the caller's stream formatting. I/O failure throws.
void write_analysis_csv_header(std::ostream& output);
void write_analysis_csv_row(std::ostream& output, const BookObservation& observation);
} // namespace exchange
