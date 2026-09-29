#include "exchange/core/build_info.hpp"

#include <iostream>
#include <string_view>

namespace {
constexpr std::string_view usage = "Usage: exchange_info [--version | --help]\n";
}

int main(int argc, char* argv[]) {
    const auto info = exchange::build_info();
    if (argc == 2 && std::string_view{argv[1]} == "--help") {
        std::cout << usage;
        return 0;
    }
    if (argc == 2 && std::string_view{argv[1]} == "--version") {
        std::cout << "deterministic_exchange " << info.version << '\n';
        return 0;
    }
    if (argc != 1) {
        std::cerr << usage;
        return 2;
    }
    std::cout << "deterministic_exchange " << info.version << '\n'
              << "Milestone: " << info.milestone << " reference matcher\n"
              << "Compiler: " << info.compiler << '\n'
              << "Configuration: " << info.configuration << '\n'
              << "Language: C++20\n"
              // Phase 2: expose the expanded contract in the diagnostic CLI.
              << "Matching engine: limit GTC/IOC, market IOC, cancel, replace\n"
              << "Analysis: trade totals, VWAP, snapshot depth and top-of-book metrics\n";
    return 0;
}
