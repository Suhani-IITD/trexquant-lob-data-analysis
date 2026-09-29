#include "exchange/core/build_info.hpp"
#include "exchange/core/checked_arithmetic.hpp"
#include "exchange/core/types.hpp"

#include <cstdint>
#include <iostream>
#include <limits>
#include <string_view>
#include <type_traits>

namespace {
using namespace exchange;

// Deliberately uses runtime checks, not assert(), so Release/NDEBUG tests still execute.
class Checks {
public:
    void expect(bool condition, std::string_view description) {
        if (!condition) {
            ++failures_;
            std::cerr << "FAIL: " << description << '\n';
        }
    }
    [[nodiscard]] int result() const noexcept { return failures_ == 0 ? 0 : 1; }

private:
    int failures_{};
};

static_assert(!std::is_convertible_v<OrderId, Quantity>);
static_assert(!std::is_constructible_v<OrderId, Quantity>);
static_assert(!std::is_convertible_v<std::uint64_t, OrderId>);
static_assert(!std::is_convertible_v<IngressSequence, MarketSequence>);
static_assert(std::is_trivially_copyable_v<OrderId>);
static_assert(sizeof(OrderId) == sizeof(std::uint64_t));
static_assert(OrderId{1} < OrderId{2});

void test_types(Checks& checks) {
    checks.expect(OrderId{}.value() == 0, "default value preserves the zero sentinel");
    checks.expect(OrderId{42}.value() == 42, "explicit ID construction");
    checks.expect(Quantity{17} == Quantity{17}, "same-type equality");
    checks.expect(PriceTicks{-1}.value() == -1, "invalid raw price retained for validation");
    checks.expect(static_cast<std::uint8_t>(Side::Sell) == 2, "fixed side representation");
}

void test_arithmetic(Checks& checks) {
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    checks.expect(checked_add(0, 0) == 0, "zero addition");
    checks.expect(checked_add(maximum - 1, 1) == maximum, "exact addition boundary");
    checks.expect(!checked_add(maximum, 1), "reject one-past-boundary addition");
    checks.expect(!checked_add(maximum, maximum), "reject large addition overflow");
    checks.expect(checked_subtract(maximum, maximum) == 0, "exact subtraction boundary");
    checks.expect(checked_subtract(20, 7) == 13, "remaining quantity subtraction");
    checks.expect(!checked_subtract(0, 1), "reject quantity underflow");
    checks.expect(checked_multiply(maximum, 0) == 0, "zero product with maximum lhs");
    checks.expect(checked_multiply(0, maximum) == 0, "zero product with maximum rhs");
    checks.expect(checked_multiply(maximum, 1) == maximum, "exact multiplication boundary");
    checks.expect(checked_multiply(8, 32) == 256, "storage-size product");
    checks.expect(!checked_multiply(maximum, 2), "reject storage-size overflow");
    checks.expect(tick_index(PriceTicks{100}, PriceTicks{100}, PriceTicks{110}) == 0,
                  "inclusive minimum index");
    checks.expect(tick_index(PriceTicks{110}, PriceTicks{100}, PriceTicks{110}) == 10,
                  "inclusive maximum index");
    checks.expect(tick_index(PriceTicks{105}, PriceTicks{100}, PriceTicks{110}) == 5,
                  "already-in-ticks indexing does not divide again");
    checks.expect(!tick_index(PriceTicks{99}, PriceTicks{100}, PriceTicks{110}),
                  "reject below-range price");
    checks.expect(!tick_index(PriceTicks{111}, PriceTicks{100}, PriceTicks{110}),
                  "reject above-range price");
    checks.expect(!tick_index(PriceTicks{1}, PriceTicks{2}, PriceTicks{1}),
                  "reject reversed range");
    checks.expect(!tick_index(PriceTicks{0}, PriceTicks{0}, PriceTicks{10}),
                  "reject nonpositive minimum");
    checks.expect(!tick_index(PriceTicks{std::numeric_limits<std::int64_t>::min()},
                             PriceTicks{1}, PriceTicks{10}),
                  "reject extreme negative before subtraction");
    checks.expect(tick_index(PriceTicks{std::numeric_limits<std::int64_t>::max()},
                            PriceTicks{std::numeric_limits<std::int64_t>::max()},
                            PriceTicks{std::numeric_limits<std::int64_t>::max()}) == 0,
                  "largest valid one-tick range");
}

void test_metadata(Checks& checks) {
    const auto info = build_info();
    checks.expect(info.version == EXCHANGE_EXPECTED_VERSION, "version comes from CMake project");
    checks.expect(!info.compiler.empty(), "compiler recorded");
    // Phase 2: keep diagnostic metadata aligned with the implemented milestone.
    checks.expect(info.milestone == "M2", "implemented milestone is honest");
    checks.expect(info.version == build_info().version, "stable metadata across calls");
}
} // namespace

int main(int argc, char* argv[]) {
    if (argc != 2) {
        return 2;
    }
    Checks checks;
    const std::string_view suite{argv[1]};
    if (suite == "types") {
        test_types(checks);
    } else if (suite == "arithmetic") {
        test_arithmetic(checks);
    } else if (suite == "metadata") {
        test_metadata(checks);
    } else {
        std::cerr << "Unknown suite: " << suite << '\n';
        return 2;
    }
    return checks.result();
}
