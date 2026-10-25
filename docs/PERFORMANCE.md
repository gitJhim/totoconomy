# Measured performance

Measured 2026-10-25 on x86-64 Linux 7.2.8-9-omarchy, AMD Ryzen 7 PRO 6850U (16 logical CPUs), GCC 16.2.1, CMake 4.4.4. Release uses `-O3 -DNDEBUG -std=c++20`, no sanitizers, single-threaded authoritative updates. Exact flags/build identity and machine details are in `measurements/environment.json` and `measurements/version.json`. Each row is one actual run, seed 42, not a throughput guarantee. This was a shared workstation with other tests/builds active, not an isolated benchmark host.

| Workload | Households / firms | Days | Trace | Decisions | Elapsed s | Decisions/s | JSONL dump/write s | Other execution s | Output bytes | Peak RSS KiB |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Accounting fixture | 6 / 2 | 30 | audit | 360 | 0.01456 | 24,723 | 0.00403 | 0.01053 | 520,986 | 5,120 |
| Town baseline, checkpoints every 15 days | 24 / 4 | 60 | audit | 3,799 | 0.15174 | 25,037 | 0.04390 | 0.10784 | 5,959,441 | 5,656 |
| Town year | 24 / 4 | 365 | summary | 22,842 | 0.68922 | 33,142 | 0.17815 | 0.51107 | 20,356,678 | 5,536 |
| Optional town decade | 24 / 4 | 3,650 | summary | 227,847 | 6.54403 | 34,818 | 1.73531 | 4.80873 | 191,415,899 | 5,464 |

Decision counts include firm payroll/production/procurement/pricing evaluations and household shopping. Every measured output passed the independent persisted-record verifier. These runs have different durations and trace/checkpoint costs; the table is not an audit-versus-summary controlled speedup claim. The decade workload increases duration/history size, not population. No population-scaling result has been claimed. Stable low standalone RSS is consistent with streamed histories; output size still grows with simulated duration.

Elapsed covers the engine loop, JSONL export, requested checkpoints and final-state serialization. It excludes scenario normalization, opening the run, post-run verification and size scanning. JSONL serialization/write time is measured separately; the remainder includes simulation **and** JSON object construction, checkpoint serialization and final output. No pure engine-only timing is claimed. Buffered writes are measured without fsync. Output bytes are sampled before the final manifest's size-field update. RSS is Linux per-process high-water KiB; the table uses direct shell CLI launches to avoid a larger wrapper process contributing inherited high-water memory. Raw results are in `measurements/accounting.json`, `baseline.json`, `benchmark.json`, and `benchmark-large.json`.

Small benchmark smoke (also run by CTest):

```sh
./build/release/econ benchmark --scenario scenarios/demo_town.json --seed 42 --until-day 3 --trace summary --out runs/benchmark-smoke --json
```

Measured year and optional larger history workload:

```sh
./build/release/econ benchmark --scenario scenarios/demo_town.json --seed 42 --until-day 365 --trace summary --out runs/benchmark --json
./build/release/econ benchmark --scenario scenarios/demo_town.json --seed 42 --until-day 3650 --trace summary --out runs/benchmark-large --json
```

Preserve prior baselines; choose a fresh output name, or explicitly add `--overwrite`. Compare the same compiler, flags, hardware, seed, model, scenario, duration and trace before attributing a performance difference to a code change. No absolute timing gate is imposed on tests. Profiling has not justified multithreaded world updates, specialized allocation systems or a database.

## Economic behavior is separate from speed

The 60-day baseline records food production/consumption 1,320/1,160 units and fruit 576/412, with unmet needs 280 food and 1,028 fruit. It completes every day; this is not a stable calibration. `work_inactive` facts first report insufficient payroll for fruit producer `firm:2` on day 16, retailer `firm:4` on day 37, and food producer `firm:1` on day 43. Their inactive-day counts are 36, 22 and 5 respectively. Fixed employment persists; the model has no permanent closure or hidden rescue. Working capital is exhausted when procurement/sales fail to finance committed payroll. The audit records actual cash, price/stock observations, exclusions and checkout results needed to investigate each household's contribution; no single macroeconomic cause is inferred from final balances.

For the controlled interval `[30,60)`, baseline consumption is food 519 / fruit 3; board consumption is food 490 / fruit 23. Baseline unmet needs are food 201 / fruit 717; board unmet needs are food 230 / fruit 697. The intervention changes information, not starting resources, and does not improve every measure. See raw paired results in `measurements/comparison.json`. A richer initialization/behavior calibration belongs to Stage 2; the exact stable fixture provides the separate accounting reference.
