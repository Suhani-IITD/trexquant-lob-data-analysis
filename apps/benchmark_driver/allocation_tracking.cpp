#include "allocation_tracking.hpp"

#include <cstdlib>
#include <new>

namespace {
void* allocate(std::size_t bytes) {
    // This diagnostic counts requested ordinary C++ allocations, not allocator
    // metadata/RSS or direct malloc/aligned allocation. Engine nodes use ordinary new.
    for (;;) {
        if (void* memory = std::malloc(bytes == 0 ? 1 : bytes)) {
            if (allocation_tracking::enabled) {
                ++allocation_tracking::counts.allocations;
                allocation_tracking::counts.requested_bytes += bytes;
            }
            return memory;
        }
        const auto handler = std::get_new_handler();
        if (!handler) { throw std::bad_alloc{}; }
        handler();
    }
}
void release(void* memory) noexcept {
    if (memory != nullptr && allocation_tracking::enabled) {
        ++allocation_tracking::counts.deallocations;
    }
    std::free(memory);
}
} // namespace
void* operator new(std::size_t bytes) { return allocate(bytes); }
void* operator new[](std::size_t bytes) { return allocate(bytes); }
void operator delete(void* memory) noexcept { release(memory); }
void operator delete[](void* memory) noexcept { release(memory); }
void operator delete(void* memory, std::size_t) noexcept { release(memory); }
void operator delete[](void* memory, std::size_t) noexcept { release(memory); }
