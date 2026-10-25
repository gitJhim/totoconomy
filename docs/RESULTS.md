# Foundation completion evidence

Source/build: `0.1.0-20b9e645bf668c71`, model `econ-1`; GCC 16.2.1, CMake 4.4.4, x86-64 Linux. Standard dependencies are contained in the source tree. A separate source-only copy with only CMake, include, src, tests, scenarios and third_party built and passed tests from a fresh Release build directory. CMake/build/test contain no download step; network isolation was not imposed on that source-copy check.

## Executed gates

- Final Debug CTest: 5/5 passed, 44.38 seconds.
- Final Release CTest: 5/5 passed, 5.34 seconds.
- Final clean source-copy Release: 5/5 passed (raw timing in `measurements/clean-source-tests.txt`).
- Final AddressSanitizer + UndefinedBehaviorSanitizer CTest: 5/5 passed, 247.85 seconds; no sanitizer errors or environment restrictions.

The five CTest entries include an always-active C++ acceptance executable covering all 18 requested gates plus firm evidence and order explanations, help/version/validate smoke tests, and an end-to-end CLI script. Exact cases map to source tests in `VERIFICATION.md`. Raw logs are in `measurements/*-tests.txt`; no unavailable test is reported as passed.

## Observed accounting and experiment

The normal engine's 30-day accounting output is in `runs/accounting/`. Final producer cash is **3,000 cents**, retailer cash **6,000 cents**, each of six households has **zero cash and zero food**, all firm food inventories are zero, total cash is **9,000 cents**, production/consumption are **180/180 units**, and unmet need is zero. The C++ test checks each intermediate phase as well as the final totals. `measurements/accounting.json` preserves the actual independent verification output.

The 60-day synthetic baseline contains 24 households and four firms. It exports real accounts, orders, observations, household and firm decisions, metrics and daily display states. Its final canonical digest is `a67eea756e9fe35f`. Both a day-30 resume and no-op branch have that same final digest and exactly the same ordered economic event suffix. The C++ acceptance suite hashes all parent artifacts before and after both branches and proves they were unchanged. A separate manual artifact scan checked the same suffix equality and the single day-30 board event.

The board branch has an identical pre-intervention opening state and changes information access at day 30. The `[30,60)` comparison is recorded in `measurements/comparison.json`: baseline consumption is food 519 / fruit 3; board consumption is food 490 / fruit 23. It does not win every measure. Baseline payroll inactivity and shortages are documented in `PERFORMANCE.md` with the actual event reasons; no rescue transfer or forced preferred result is present.

`decision:1` in the baseline is a real producer payroll decision with its financing constraints and wage events. `decision:11` is the first real household choice. Saved explanations are in `measurements/explanation.json` and `household-explanation.json`. The C++ test finds a real delayed procurement decision and confirms its explanation includes both reservation and actual delivery.

## Measurements and reproduction

The final Release year workload (24 households / four firms, seed 42, 365 days, summary tracing) evaluated **22,842 decisions in 0.68922 seconds**, about **33,142 decisions/s**, with **5,536 KiB peak RSS** and **20,356,678 bytes** of output at the recorded sampling point. JSONL dump/write accounted for 0.17815 seconds; the remaining 0.51107 includes simulation and other record construction/serialization. Post-run verification is excluded from elapsed time. The optional 3,650-day workload evaluated 227,847 decisions in 6.54403 seconds. See `PERFORMANCE.md` and raw measurement JSON for the supplied accounting/audit workloads, environment and limits.

README contains exact commands for Debug/Release/sanitizer builds, tests, validate/run, inspect/explain, resume, both branches, compare, verify and benchmark. The executable `tests/acceptance.cmake` ran each command, read emitted IDs/results and checked negative cases. Existing outputs are preserved by default; reproduction uses fresh names or explicit `--overwrite`.

## Scope remaining

No active Stage 1 behavior is intentionally omitted. Independent policy re-execution from logs and empirical calibration are not claims of the persisted verifier; it reports those limits explicitly. There is no Clang or cross-architecture reproducibility claim. A GUI/video renderer, road routing, dynamic employment, business closure, deeper substitution, broad experiments and all later-stage mechanisms remain roadmap work. The town is a synthetic experiment, not a prosperity calibration.

The next concrete stage is a grain→flour→bread scenario with independently reconciled input-constrained production, stockout-aware procurement/demand and richer labor/inactivity behavior. Dependencies and exit criteria for Stages 2–5 are in `ROADMAP.md`.
