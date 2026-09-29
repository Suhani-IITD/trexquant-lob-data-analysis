# C++ Limit Order Book and Market-Data Analysis

A dependency-free C++20 matching simulator with price-time priority and a market-data
analysis consumer. Supports limit GTC/IOC, market IOC, cancellation, partial fills,
and atomic replacement. Analytics include trade count, volume, VWAP, best quotes,
spread, midpoint, depth and top-of-book imbalance.

## Build and run

Requires CMake 3.24+, Ninja and a C++20 compiler. Run from the project root:

```bash
cmake --preset release -DEXCHANGE_WARNINGS_AS_ERRORS=ON
cmake --build --preset release --parallel 2
ctest --preset release
./build/release/bin/scenario_runner --input fixtures/manual_simulation.txt
./build/release/bin/analysis_demo
./build/release/bin/analysis_demo --csv > build/analysis.csv
```

Windows uses the same Debug/Release presets and `.exe` executables. Create separate
build directories for Windows and Linux; generated caches are platform-specific.
For debugging, use the `debug` preset. Linux/WSL sanitizer checks:

```bash
cmake --preset asan-ubsan -DEXCHANGE_WARNINGS_AS_ERRORS=ON
cmake --build --preset asan-ubsan --parallel 2
ctest --preset asan-ubsan
```

LeakSanitizer must run outside debugger/ptrace supervision. The 48 CTest cases cover
matching models, order lifecycles, allocation failures, output reuse, analytics,
and CLI/CSV behavior. `BUILD_TESTING=OFF` omits test executables.

## Simulation and CSV analysis

Scenario files begin with `SCENARIO 1`. For example:

```text
SCENARIO 1
NEW 1 1 SELL LIMIT GTC 100 5
NEW 2 1 BUY LIMIT IOC 100 3
CANCEL 1 1
EXPECT_EMPTY
```

Commands use `NEW id instrument BUY|SELL LIMIT|MARKET GTC|IOC price quantity`,
`CANCEL id generation`, or `REPLACE id generation price remaining_quantity`.
Market orders require IOC and `-` for price. Generation is the accepted New's
command sequence; replacement preserves it. Blank lines and `#` comments are
allowed. Optional `EXPECT_*` assertions are illustrated in `fixtures/`.

The scenario runner configures instrument 1, tick prices 1–10000, and capacity for
10000 active orders/levels. It prints trades and the final FIFO book. Export metrics
from your own scenario with:

```bash
./build/release/bin/scenario_runner --input fixtures/analysis_scenario.txt \
  --analysis-csv build/scenario-analysis.csv --observe-every 4
```

Every result contributes to trade totals. Observations occur every N commands and
at the final command; the default interval is 1. EXPECT lines do not advance the
interval. CSV output overwrites the selected output path, protects the input file,
and can be partial if the command exits with an error.

The demo executes 6 units at 101 and 2 at 103: volume 8, notional 812 tick-units,
VWAP 101.5 ticks. [The expected CSV](fixtures/analysis_expected.csv) is a regression
fixture. Metric definitions:

- VWAP = sum(price ticks × executed units) / traded volume; count each trade once.
- Spread = best ask − best bid; midpoint = their average. Both require two sides.
- Depth sums all price-level quantities on each side.
- Top imbalance = (best bid quantity − best ask quantity) / their sum. A one-sided
  book yields +1 or −1; a zero denominator yields no value.

CSV blanks represent absent values. Totals use checked integer arithmetic;
overflow leaves prior analytics intact. Ratios use floating-point arithmetic and
CSV rounds to six decimals. Observations use command sequences, not timestamps.
The data is synthetic; no historical execution or profitability claims are made.

## Design and source layout

| Directory | Purpose |
| --- | --- |
| `include/exchange`, `src` | Domain types, book, matching engine and analytics |
| `apps/scenario_runner`, `apps/analysis_demo` | Interactive examples and CSV analysis |
| `apps/benchmark_driver` | Timing workloads and separate allocation diagnostics |
| `apps/exchange_info` | Version/compiler information |
| `tests`, `fixtures` | Regression checks, scenarios and expected metrics |
| `cmake`, `.github` | Build controls and CI |

One thread owns book mutation. Ordered price maps own FIFO lists; an unordered ID
index locates orders for cancellation/replacement. Trades execute at maker prices.
Same-price reductions retain priority; quantity increases and price changes lose
priority and match again. Preparation reserves output and stages allocations before
book mutation, so rejected replacement or preparation failure preserves the book.

`ReferenceEngine::process()` returns owned output; `process_into()` reuses caller
vectors. Consume/copy output before reusing it. Fill scratch is reused by default
(`EXCHANGE_REUSE_FILL_BUFFER=OFF` selects the baseline); buffers retain their largest
capacity. Full `snapshot()` includes FIFO orders; `depth_snapshot()` copies only
sorted prices/totals. `MarketAnalysis` consumes every result in sequence and derives
metrics from a current snapshot. It uses reusable transaction storage and maintains
no second order book. There is no networking or external historical-feed adapter.

## Performance tests

Use unsanitized Release builds. Workloads are `add`, `cancel`, `match`, `sweep`,
`reduce`, `reprice`, `mixed`, `sustained`, and `diverse`:

```bash
./build/release/bin/benchmark_driver --workload all --operations 10000 \
  --trials 5 --output reuse > build/benchmark.csv
./build/release/bin/allocation_driver --workload all --operations 10000 \
  --trials 1 --output reuse > build/allocations.csv
./build/release/bin/benchmark_driver --workload diverse --operations 1000000 \
  --book-size 100000 --seed 20260928 --trials 5 --output reuse \
  --analysis depth --observe-every 10000 > build/large-benchmark.csv
```

`--analysis off` measures matching; `trades` adds cumulative analysis; `snapshots`
adds full book copies; `depth` uses compact level snapshots. `--output value|reuse`
selects output ownership. Timing includes processing, result disposal, counters and
selected analytics. Input generation, prefill, final validation and CSV I/O are
excluded. Each invocation includes an excluded warm-up. Batch means are not latency
percentiles. Allocation counts cover ordinary new/delete; requested bytes are
cumulative, not peak memory.

The large `diverse` dataset has four instruments, both sides and up to 100 prices
per side/instrument. A fixed xorshift64 seed selects resting orders with replacement.
Each five-command block reduces, reprices, partially fills, cancels and replenishes
one order, producing one trade of three units. Expected totals and final depths are
checked after each trial. `sustained` uses rotating slots in a mostly one-price book.
Block workloads require command counts divisible by five; diverse supports up to
5M commands, other workloads 200K, and live-book size up to 100K. A longer example
uses `--workload diverse --operations 5000000 --book-size 100000 --seed 42`.

Optional Linux function profiling (Valgrind headers required):

```bash
cmake -S . -B build/profile -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DEXCHANGE_ENABLE_CALLGRIND=ON
cmake --build build/profile --parallel 2
valgrind --tool=callgrind --instr-atstart=no \
  --callgrind-out-file=build/profile/match.callgrind \
  ./build/profile/bin/benchmark_driver --workload match --operations 10000 --trials 1
```

See [PERFORMANCE_REPORT.md](PERFORMANCE_REPORT.md) for measured results and decisions.
Generated builds, CSV exports, profiles and local tooling files are excluded from Git.
