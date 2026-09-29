#pragma once

#include "exchange/core/types.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace exchange {

struct InstrumentConfig {
    InstrumentId id;
    std::string symbol;
    PriceTicks minimum;
    PriceTicks maximum;
    Quantity maximum_order_quantity;
    Quantity maximum_level_quantity;
};

struct CapacityConfig {
    std::size_t maximum_active_orders{100000};
    std::size_t maximum_active_levels{100000};
};

struct EngineConfig {
    std::vector<InstrumentConfig> instruments;
    CapacityConfig capacity;
};

struct ConfigError {
    std::string field;
    std::string description;
};

[[nodiscard]] std::vector<ConfigError> validate_config(const EngineConfig& config);

} // namespace exchange
