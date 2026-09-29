#pragma once

#include <cstddef>

namespace allocation_tracking {
struct Counts {
    std::size_t allocations{};
    std::size_t requested_bytes{};
    std::size_t deallocations{};
};
// Only the separate allocation_driver links replacements for ordinary new/delete.
inline thread_local bool enabled = false;
inline thread_local Counts counts;
class Scope {
public:
    Scope() { counts = {}; enabled = true; }
    ~Scope() { enabled = false; }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
};
} // namespace allocation_tracking
