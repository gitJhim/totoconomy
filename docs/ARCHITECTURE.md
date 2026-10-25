# Architecture and decision records

`econ_core` owns reusable engine, policy, persistence and analysis code. `econ` only parses flags, loads inputs, selects library operations and formats results. Tests link the same library and execute the actual CLI. There are no mutable globals or downloads at configure time.

| File | Responsibility |
| --- | --- |
| `include/econ/econ.hpp` | Domain records, explicit state ownership, narrow interfaces |
| `src/core/model.cpp` | Checked integers, normalized scenarios, IDs, initial state, boundary invariants, stable digest/key functions |
| `src/core/policies.cpp` | Household action proposal using `ShoppingView` only |
| `src/core/simulation.cpp` | Scheduling, observation publication, transaction validation, reservations, wages, recipes, checkout, consumption, pricing |
| `src/core/storage.cpp` | Atomic JSON files, streaming logs, runs/checkpoints, independent ledger reconciliation, inspect/explain/compare |
| `src/cli/main.cpp` | Strict command dispatch, interactive inspector launch and exit codes |

An `Engine` owns `State` and normalized scenario. Accounts own cash and product inventories; households own needs, employment and memories; firms own recipe/procurement/price configuration and current prices. `State.orders` owns all outstanding commitments. The next day and economic record counters are explicit state. A completed run does not retain history in memory: sinks stream events/decisions and each day's metrics/display data to files. Verification keeps reconstructed current state, outstanding orders, and bounded per-day aggregates.

Policy proposals are separated from authoritative settlement. `choose()` receives only observations, known directory identities, household budget/cash/time constraints and configured travel minutes. It cannot read a live shop account or price. Checkout refreshes observations and validates actual affordability/availability. Every monetary/stock mutation is an effect list staged on account copies, validated before committing. A rejected purchase changes evidence/observations but no balances, inventories or reservations.

Verification reads persisted facts through separate reconciliation logic. It does not call `Engine.step()` or trust the previous verification report. Analytics do not feed results back into the economy. A renderer can consume `replay.jsonl` without invoking a policy. Verification is in the persistence translation unit for now, with a separate function and no simulation callbacks; splitting it is appropriate when formats grow.

## Decisions

1. **Integer, cash-only boundary.** Money is cents and quantities are whole product units. No loans, debt, taxes, rent, fees, imports, spoilage or issuance exist in Stage 1. This makes exact closed-ledger checks possible. Adding a new boundary flow will require an explicit counterparty/record.
2. **Completed-day checkpoints.** No mid-phase restore contract. This reduces pending scheduler state to `next_day`, reservations/orders and policy/knowledge state. Complete-file rename prevents partial JSON from becoming a valid checkpoint; no claim of power-loss durability/fsync is made.
3. **Seller ownership during delivery.** Buyer cash and seller goods remain reserved until arrival. In-transit goods are a subset of seller inventory, never an additional inventory location. No cancellation is exposed; no money has transferred before arrival, so no refund flow exists. Arrival price is the agreed order price even if the seller later changes its posted price.
4. **Explicit simple policies.** Fixed employment, inventory-target procurement, whole-request checkout and deterministic remembered-cost comparison precede learning or dynamic labor markets. There is no branch-specific economic script.
5. **Canonical noncryptographic digest.** FNV-1a-64 over compact sorted-key JSON provides stable reproducibility comparisons, not adversarial authentication. Complete checkpoint integrity and economic state digests are separate.
6. **Direct JSON domain serialization.** A small pinned JSON dependency is preferable to a custom parser. Source-format fields are documented; schema/model changes must be explicit migrations or clear rejection. Public structs favor inspectable foundations over ABI stability.

7. **Inventory liquidation can continue during paid-work inactivity.** No new production runs without financed eligible labor. Existing stock transfers and procurement do not require a modeled shop-service throughput resource. This explicit simplification lets cash-constrained firms recover without debt; later storefront capacity/closure work must change the contract and tests.

## Terminal inspector

The `econ_tui` library (`src/tui/`) consumes recorded artifacts through per-day byte-offset indexes. Its controller/rendering can be tested without a terminal; a POSIX RAII terminal adapter supplies key input, resizing, signal cleanup and ANSI output. It never advances `Engine` or changes run artifacts. Exact initial/final/checkpoint state and partial replay snapshots remain distinct. See `TUI.md` for controls and evidence scope.
