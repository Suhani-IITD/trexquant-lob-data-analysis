# Performance summary

Measurements use Ubuntu 24.04/WSL2, Intel i5-8365U, GCC 13.3 and Release
`-O3 -DNDEBUG`. Timings exclude generation, prefill, final validation and CSV I/O;
processing, output disposal and selected analytics are included. Each invocation
has an excluded warmup. Synthetic workloads and WSL scheduling limit generalization;
values are batch medians, not latency percentiles. Compare paired experiments,
not absolute times from different runs.

## Engine storage and matcher improvements — 2026-09-29

Hypothesis: repeated allocation of staging buckets and replacement nodes wastes work.
Reuse staging-index buckets and resting replacements' FIFO/index nodes, remove
redundant matcher work and pass the already-found book into matching. Preparation
still completes allocations before mutation; replacements preserve generation and
receive new FIFO priority when required.

GCC Release, LTO OFF, reusable output, CPU 2 pinned; 100K commands, 10K book-size
parameter, five alternating before/after rounds × three trials:

| Workload | Before median [IQR], ms | After median [IQR], ms | Reduction |
| --- | ---: | ---: | ---: |
| Add | 28.439 [26.558–32.431] | 21.979 [21.100–23.128] | 22.7% |
| Reprice | 27.161 [25.645–28.605] | 13.201 [12.679–13.660] | 51.4% |
| Diverse | 22.511 [20.217–24.788] | 16.072 [15.433–16.758] | 28.6% |

Separate 10K-command diagnostics reduced ordinary allocation calls: add
**30012 → 20012**, reprice **30003 → 3**, diverse **14005 → 6005**. The timings
compare the combined changes, not individual commits. **Keep**: measured savings
support the storage changes; retained scratch/bucket capacity uses some memory.
The imported implementation passed all 48 existing tests in Release and optimized
ASan/UBSan with leak detection, including allocation-failure/retry checks.

## Header arithmetic, stable layout and build controls

Checked arithmetic is now header-only `constexpr`, enabling inlining without LTO.
The fill-reuse macro is private to the library; its vector member always exists,
so callers see the same engine layout. Benchmark metadata reads the library's
setting through `BuildInfo`. OFF mode retains an unused vector object.

Current source passed **48/48 tests each** in Release fill ON, Release fill OFF,
and optimized ASan/UBSan with leak detection. All nine workloads passed 10K-command
smoke runs in both modes; metadata matched the setting. Separate compile checks
confirmed constexpr overflow handling and equal engine size (256 bytes on this
platform) with either consumer macro setting. These smoke runs are not timing
experiments. **Keep for inlining opportunity and layout consistency; no isolated
speedup is claimed for this change.**

LTO remains opt-in (`EXCHANGE_ENABLE_LTO=ON`) with compiler support checked at
configuration. The scenario runner validates once at completion in Release;
`--validate-every N` enables periodic scans. Debug retains per-command engine scans.
These controls avoid unnecessary scans and permit build comparisons; no new local
isolated timing claim is made for them. The later book/output refactor is not yet
part of this source.

## Earlier buffer and analysis evidence — 2026-09-28

These recorded development comparisons precede the staging/node changes above:

| Hypothesis/change | Before → after median | Decision/trade-off |
| --- | --- | --- |
| Reuse fill scratch and caller output, 100K match commands | 20.935 → 16.026 ms | Keep; buffers retain peak capacity |
| Reuse analysis aggregate vectors, 1M commands/100K live | 407.714 → 298.920 ms | Keep; eliminates 4M allocation calls |
| Use aggregate depth instead of full FIFO snapshots every 10K commands | 1663.636 → 308.124 ms | Keep; omits order-level detail |

The analysis comparisons used five rotated rounds; exact trade and snapshot metrics
agreed. The 1M-command diverse dataset produced 200K trades/600K units with zero
rejections. A 5M-command depth run produced 1M trades/3M units, median 1478.518 ms,
peak RSS about 466 MiB including prebuilt commands. A September 29 clean-source
rerun also passed, but measured 3074.153 ms and 465.7 MiB. Neither run measures the
latest arithmetic/layout change; cross-day timing differences do not establish a
regression. Original raw development captures were removed during cleanup.

Callgrind's inflated counts were resolved with START/STOP instrumentation and
`--instr-atstart=no`: 10000 measured commands report 10000 calls. Instruction counts
do not establish runtime gains or cache behavior.

[README](README.md#performance-tests) gives benchmark commands. Retained local
logs/CSVs are under `build/results/`: `pr-1-review`, `implementation-commit`,
`pr2-implementation-01`, and `2026-09-29-clean`. These are Git-ignored artifacts;
this report preserves the concise findings. Current source retains 48 tests;
additional upstream test suites and the book/output refactor remain deferred.
