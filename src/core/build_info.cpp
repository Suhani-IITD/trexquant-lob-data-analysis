#include "exchange/core/build_info.hpp"

#include "exchange/build_config.hpp"

namespace exchange {

BuildInfo build_info() noexcept {
    // Matching semantics are complete through M2; auxiliary M3 tooling was removed.
#ifdef EXCHANGE_REUSE_FILL_BUFFER
    constexpr bool reuses_fill_buffer = true;
#else
    constexpr bool reuses_fill_buffer = false;
#endif
    return {EXCHANGE_VERSION, EXCHANGE_COMPILER, EXCHANGE_BUILD_TYPE, "M2", reuses_fill_buffer};
}

} // namespace exchange
