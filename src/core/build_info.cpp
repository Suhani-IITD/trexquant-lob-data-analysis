#include "exchange/core/build_info.hpp"

#include "exchange/build_config.hpp"

namespace exchange {

BuildInfo build_info() noexcept {
    // Matching semantics are complete through M2; auxiliary M3 tooling was removed.
    return {EXCHANGE_VERSION, EXCHANGE_COMPILER, EXCHANGE_BUILD_TYPE, "M2"};
}

} // namespace exchange
