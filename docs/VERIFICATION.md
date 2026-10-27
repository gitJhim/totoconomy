# Verification contracts and acceptance evidence

Use the exact Debug, Release and sanitizer commands in README. `econ_tests` uses throwing `REQUIRE` checks, not `assert`, so Release keeps all checks active. `tests/acceptance.cmake` exercises the compiled CLI and checks real emitted results. There are no absolute timing gates.

The accounting fixture has independent arithmetic: payroll = 6×1,000 = 6,000; producer payroll = 3,000; wholesale = 6×500 = 3,000; retail = 6×1,000 = 6,000; output/consumption = 30×6 = 180. Opening/closing cash is 3,000 producer + 6,000 retailer = 9,000, with zero household cash. Tests check after payroll/production, after wholesale, after shopping, after consumption, and after 30 completed days. They assert all six wage records precede production. No expected result is calculated by calling the implementation's pricing or transaction helper.

| Gate | Authoritative executable evidence |
| --- | --- |
| 1. Exact accounting cycle | `accounting()` intermediate checks and 30-day independent constants; CLI reads verified 180/180/9,000 |
| 2–3. Atomic wages/trades and rejection | `transactions()` checks successful resource movements and byte-equivalent account state after cash/stock rejection; no unpaid output |
| 4. Checked arithmetic | `arithmetic()` tests INT64 max/min, multiplication overflow/zero, fractional/out-of-range JSON and destination overflow without partial account mutation |
| 5. Capacity and recipes | `recipe()` independently expects 7 inputs at 2/unit → 3 output, 1 input remainder; ample inputs yield exactly capacity 6 |
| 6. Competing reservations | `reservations()` first reserves 3,000 cents/6 units, then rejects cash and stock overcommitments with unchanged accounts |
| 7–8. Delays and persistence | `reservations()` attempts settlement on days 0 and 1, saves pending day-2 state, restores and settles exactly once; 60-day town checkpoints also contain real pending orders |
| 9. Restricted information | `information()` verifies hidden cheap store price absent from exclusions, discovery changing choice, actual board publication, stale checkout rejecting unaffordable purchase, and stockout refresh |
| 10. Re-simulation | `integration()` two runs match final digest, all ordered economic facts, and audit decisions |
| 11–12. Resume/no-op | continuous day 60 equals resumed/no-op day-30 continuation in canonical state **and every suffix economic event** |
| 13. Parent immutability | file-content digests of every parent artifact match before/after both branches |
| 14. Public-board boundary | pre-intervention resources/state equal checkpoint; exactly one board event at day 30, parent history unchanged |
| 15. Trace invariance | summary/audit final digests and full economic facts match |
| 16. Metrics | separate persisted-record verifier recalculates product units, stage sales/prices, payroll, household needs, cash flows and closing balances; edited wages fail verification |
| 17. Invalid inputs | malformed JSON, incompatible schemas, duplicate/bad IDs, capacities, unknown keys/interventions, unsigned overflow, incomplete/truncated checkpoints, earlier resume, unknown/duplicate CLI flags |
| 18. Paired compatibility | shared interval/seed/origin/checkpoint proof; rejects unrelated seed or pre-continuation interval |

Additional tests check all four firm policy families, payroll constraints, no-change price decisions and explain an order through reservation and actual delivery. Additional integration checks remove an event and require verification failure; verify recorded decision/event links and per-day display accounts; exercise `inspect`, `explain`, output preservation, and the benchmark command. Test artifacts are temporary and removed on success, retained with their path on failure. CLI CTest artifacts remain in `build/<configuration>/acceptance/`.

Hard invariants are checked separately from expectations. Negative cash/physical quantities, over-reservation, unbalanced settlement, overflow, duplicated/early order fulfillment, or lost persisted facts are software errors. A household not purchasing, unmet need, firm inactivity, accumulating stock or unequal firm outcomes can be valid model behavior.

The independent verifier starts from recorded opening state and applies validated operation effects in sequence. It validates each operation's expected effect shape, per-day labor/recipe/consumption accounting, exact arrival and invoice matching, final accounts, evolving prices, metrics, decision links and periodic display exports. It does not regenerate households' memories/policy choices or assert empirical realism. Those unavailable checks are identified in its output. The test suite separately proves policy information restrictions and deterministic re-execution. A self-consistent rewrite of all artifacts is not cryptographically authenticated by this system.

Executed build/test and measured run results are recorded in `RESULTS.md` and `PERFORMANCE.md`. These records describe actual invocations, not unrun proposed commands.

The labor-market increment adds tests for vacancy eligibility, wage offers, reservation wages,
commute valuation, tenure, employer switching, notice and cancellation, negotiated payroll,
staffing demand/inventory rules, perishable worker-days and per-person labor records. Regression
cases cover zero and maximum capacity, large wage steps, zero-wage offer adjustments, unfunded
openings, manifest mode labels, and consistent-but-false search evidence. Dynamic audit/summary,
resume and no-op runs must match canonical state and economic facts; public-board branches must
verify. Mode comparisons require a common day-zero endowment. TUI form focus is checked at
60×18, 80×24 and 120×36. See [integration review](employment.md).
