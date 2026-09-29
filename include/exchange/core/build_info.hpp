#pragma once

#include <string_view>

namespace exchange {

struct BuildInfo {
    std::string_view version;
    std::string_view compiler;
    std::string_view configuration;
    std::string_view milestone;
    bool reuses_fill_buffer; // EXCHANGE_REUSE_FILL_BUFFER, private to the library build
};

// All returned views refer to static storage, valid for the lifetime of the process.
[[nodiscard]] BuildInfo build_info() noexcept;

} // namespace exchange
