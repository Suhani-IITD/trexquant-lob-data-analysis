# Performance summary

Recorded 2026-09-28 on Ubuntu 24.04/WSL2, Intel i5-8365U (4 cores/8 threads),
GCC 13.3, CMake 3.28.3 and Ninja 1.11.1. Native timings use Release `-O3 -DNDEBUG`.
Inputs/prefill, validation and CSV I/O are excluded; processing, output disposal and
selected analytics are included. Synthetic workloads, unpinned CPU and WSL
scheduling limit generalization. Values are batch medians, not latency percentiles.

## Matching buffers

Hypothesis: repeated fill-plan and result-vector allocations waste work. Retain
engine scratch capacity and offer caller-owned `process_into()` output. Three
rotating rounds × three trials, 100K commands per workload:

| Workload | Original buffers | Fill reuse | Fill + output reuse | Combined reduction |
| --- | ---: | ---: | ---: | ---: |
| Match | 20.935 ms | 19.461 ms | 16.026 ms | 23.4% |
| Four-level sweep | 84.269 ms | 76.284 ms | 69.834 ms | 17.1% |
| Sustained, 10K live | 19.362 ms | 18.893 ms | 14.708 ms | 24.0% |

At 10K matching commands, ordinary allocations fell from 30000 to 3. **Keep both**:
fill reuse defaults ON; output reuse is optional. Buffers retain their largest
capacity. Fill-only gains were workload-dependent, with overlapping ranges in
several cases; an earlier Windows sequential trial was slower with reuse.

## Large dataset and analyser

One million commands, 100K live orders, four instruments, 800 occupied price levels,
seed 20260928: 86570 distinct slots exercised, 200K trades, 600K units, zero rejections.
Five rounds rotated analysis modes and alternated original/candidate execution.

| Experiment | Before median [range] | After median [range] | Decision |
| --- | --- | --- | --- |
| Replace per-result statistics-map copies with reusable aggregate vectors | 407.714 [365.937–435.116] ms | 298.920 [277.183–341.960] ms | Keep: 26.7% faster |
| Replace full FIFO snapshots with aggregate depth snapshots, every 10K commands | 1663.636 [1615.556–1770.073] ms | 308.124 [280.828–321.107] ms | Keep: 81.5% faster |

The snapshot comparison was a separate five-round experiment after aggregate reuse.
All quote/depth/trade metrics agree across snapshot types. Reusable aggregates
eliminate 4M allocations per 1M commands; ordinary allocation calls drop from
5400005 to 1400005. Compact snapshots reduce cumulative requested bytes from
423063048 to 100503048. They omit order-level detail; full snapshots remain available.

LOB-only control medians were 281.084/282.477 ms, with overlapping ranges. Screening
at 1K/10K/100K live orders gave 195.425/219.153/282.477 ms; larger books cost more in
this workload. Snapshot cadence matters: at 100K live orders, observing every 1K
commands cost 11942 ms with full copies versus 320 ms with compact depth.

## Long run and correctness

Five million commands, 100K live orders, seed 42, three trials with depth observations
every 10K commands: **1M trades, 3M units, zero rejections**, 99995 distinct slots.
Median **1478.518 ms** (range 1478.120–1499.403), or **3.382M commands/second**.
Peak process RSS was approximately **466 MiB**, including prebuilt commands and
reserved engine storage. Lower cumulative allocation did not reduce that peak.

The implementation passed **48/48 tests** in Release, Debug and optimized
ASan/UBSan with leak detection. Checks cover independent matching models, full
output equivalence, allocation failures/retries, overflow rollback, instrument
isolation, snapshot equivalence and exact scenario CSVs. No pool or container
rewrite was justified. Fresh Windows validation and hosted CI results are not claimed.

Callgrind's former inflated call counts came from collection toggles retaining
unmeasured shared-edge calls. START/STOP instrumentation with `--instr-atstart=no`
correctly reports 10000 calls for 10000 measured commands. Instruction counts are
not runtime or cache measurements.

[README](README.md#performance-tests) contains commands for the current workloads
and comparison modes. Before-values above summarize recorded development
experiments; transient builds, raw captures and debug logs were removed for submission.
