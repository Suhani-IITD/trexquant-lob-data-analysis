#include "exchange/core/config.hpp"

#include <set>

namespace exchange {

std::vector<ConfigError> validate_config(const EngineConfig& config) {
    std::vector<ConfigError> errors;
    if (config.instruments.empty()) {
        errors.push_back({"instruments", "At least one instrument is required"});
    }
    if (config.capacity.maximum_active_orders == 0) {
        errors.push_back({"maximum_active_orders", "Must be positive"});
    }
    if (config.capacity.maximum_active_levels == 0) {
        errors.push_back({"maximum_active_levels", "Must be positive"});
    }
    std::set<InstrumentId> ids;
    std::set<std::string> symbols;
    for (const auto& instrument : config.instruments) {
        if (instrument.id.value() == 0 || !ids.insert(instrument.id).second) {
            errors.push_back({"instrument.id", "IDs must be nonzero and unique"});
        }
        if (instrument.symbol.empty() || !symbols.insert(instrument.symbol).second) {
            errors.push_back({"instrument.symbol", "Symbols must be nonempty and unique"});
        }
        if (instrument.minimum.value() <= 0 || instrument.maximum < instrument.minimum) {
            errors.push_back({"instrument.price_range", "Require 0 < minimum <= maximum"});
        }
        if (instrument.maximum_order_quantity.value() == 0 ||
            instrument.maximum_level_quantity.value() == 0) {
            errors.push_back({"instrument.quantity_limits", "Must be positive"});
        }
    }
    return errors;
}

} // namespace exchange
