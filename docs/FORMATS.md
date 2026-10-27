# Versioned data contracts

All files are UTF-8 JSON. JSONL is one complete compact JSON object per line, newline terminated. All economic numeric fields use **decimal JSON integer tokens in signed 64-bit range**, never exponent/fractional tokens. IDs are strings. The C++ parser preserves exact integer values, including above 2^53. Browser consumers must use a lossless integer parser/BigInt before coercion; ordinary JavaScript `JSON.parse` numbers are not a supported exact-money parser. Only the nonauthoritative `performance` fields may contain floating point. Nonnegative amounts are validated; zero posted prices are invalid. Unknown scenario fields and CLI flags are errors.

## Scenario (`scenario-1`)

Required top-level fields: `schema`, `products` (unique product names), `households` and `firms` (nonempty arrays). Product IDs match `[a-z][a-z0-9_]*`; account IDs match `household:[1-9][0-9]*` / `firm:[1-9][0-9]*` and must be unique. References are validated. Defaults: `name="town"`, `refresh_days=3`, `explore_days=7`, `public_board=false`. Both observation periods must be positive. All resolved defaults appear in `scenario.normalized.json`. Firm/household arrays and products sort by ID/name.

Households require `id`, `employer`, `budget`, `needs` (product→units), `known_shops` (unique retailer IDs). Defaults: `cash=0`, `stock={}`, `time_minutes=60`, `time_value=0`. The employer must be a firm ID, or empty/null for initial unemployment. A zero need/budget is allowed. Known-shop observations are initialized from scenario offers, not input omniscience.

Firms require `id`, `kind` (`producer` or `retailer`). Defaults: `cash=0`, `stock={}`, `wage=0`, `capacity=0`, `labor_required=0`, `output=""`, `inputs={}`, `prices={}`, `targets={}`, `suppliers={}`, `lead_days=0`, `travel_minutes=0`, `price_policy="fixed"`, `price_min=1`, `price_max=100000`, `price_step=0`. Maps except suppliers are product→integer; suppliers is product→firm ID. Procurement targets require suppliers that actually quote that product. Producers require a valid output, positive minimum labor and positive wage; retailers cannot declare production capacity/inputs. Price bounds apply to `inventory_band` initial/evolving prices. Opening stocks are endowments. Product maps cannot contain unknown products.

The supplied scenario files are complete worked input examples. Cash, payroll, recipes, capacity, inventories, leads, budgets, information intervals and pricing are data, not fixture-name branches in code.

## Run manifest (`manifest-1`)

`model="econ-2"`, `status` (`running`, `complete`, `failed`), `scenario_digest`, `seed`, `origin`, `run_id`, `build`, exclusive interval `[from_day,until_day)`, `trace`, `employment` (`fixed` or `dynamic`), `parent`, `intervention`, `artifacts`, final `state_digest`, and measured `performance`. `build` identifies version, source hash, compiler and configuration. Origin hashes normalized scenario/seed/model. `parent` is null or identifies run path, complete checkpoint digest and pre-intervention state digest. `intervention` is null or the actual validated input. A no-op still appears in manifest metadata without adding an economic event.

`performance.elapsed_seconds` covers the engine loop, streaming exports, checkpoint writing and final-state serialization. `jsonl_serialization_write_seconds` measures JSONL dump/write calls; `other_execution_seconds` is the remainder, including policy/state work, record construction, checkpoint JSON and final serialization. Decision counts include both household choices and firm policy evaluations. These are not independent pure-engine and pure-I/O microbenchmarks. Verification/normalization and directory-size scanning are outside elapsed time. RSS is Linux `getrusage(RUSAGE_SELF).ru_maxrss` in KiB. Output bytes are sampled before the final size field is written, so exclude that small metadata update. Tests may share a process high-water mark; standalone benchmarks start fresh.

## State and checkpoints

`initial_state.json` / `final_state.json`: `schema="state-1"`, model, `state`, `digest`. Initial is before any branch intervention. State contains `next_day`, `seed`, `next_event`, `next_decision`, `next_order`, `public_board`, `accounts`, `households`, `firms`, `orders`.

- Account: `id`, `cash`, `reserved_cash`, complete product→unit `stock` and `reserved_stock` maps, and `labor` (zero at day boundaries).
- Household: ID/employer/budget/time/needs, `known_shops`, `knowledge` observations. Observation: `shop`, `product`, `price`, `stock` (available observed units), `day`, `source`.
- Firm: resolved configuration plus current prices.
- Pending order: `id`, `buyer`, `seller`, `product`, `quantity`, agreed unit `price`, integer `arrival` day. Every pending order has matching buyer-cash and seller-stock reservations. It cannot settle early or twice.

`checkpoints/day-NNNN.json`: `schema="checkpoint-1"`, model, normalized `scenario`, complete `state`, `state_digest`, `origin`, `parent_run`, `checkpoint_digest`. The envelope checksum excludes only itself. Complete-day validation checks structural compatibility, fixed config, prices, references, reservations, observations, numeric types and aggregate cash. Unknown schemas/models, missing fields, truncated JSON and checksum mismatches fail. Files are written to a sibling `.tmp` and renamed only after a successful close. Parent checkpoints are never edited. No migration is implemented; `econ-1` artifacts require the earlier executable.

Interventions require exactly `schema="intervention-1"`, `day` equal to loaded `next_day`, and `type="noop"` or `"public_price_board"`. Enabling an already enabled board fails. No generic scripting, monetary issuance or external shocks are accepted.

## Economic events (`event-1`)

`id`, integer `day`, `phase`, `type`, `data`, `effects`. Event IDs are monotonically allocated and continue through checkpoints. Facts within each day follow documented phase order and stable engine traversal order. Each effect is `{actor,field,product,delta}` with `field` one of `cash`, `reserved_cash`, `stock`, `reserved_stock`, `labor`. Product is empty for cash and labor. Effects describe changes; the verifier applies them independently.

Event types and relevant data:

- `wage`: employer, household, amount, labor_units; equal cash debit/credit and one worker-day delivered to the employer.
- `production`: firm, product, quantity, capacity, output_per_worker, paid_workers, inputs_per_unit, labor_used, labor_idle; input and labor debits/output credit.
- `labor_released`: firm, units, use (`idle` or `retail_service`); removes unused worker-days.
- Employment events, staffing/search decisions, and their state and metric fields are specified in [employment.md](employment.md).
- `order_reserved`: complete order, stage; reserve cash and seller goods.
- `trade`: buyer/seller/product/quantity/price/amount/stage/order; cash and goods settle atomically; `order` null for immediate settlement or a linked reserved order ID.
- `trade_rejected`: requested buyer/seller/product/quantity/price/stage/reason; no effects.
- `consumption`: household/product/quantity; stock debit. `unmet`: same identity/quantity, no effects.
- `price_changed`: firm/product/old_price/new_price/policy; no account effects.
- `work_inactive`, `procurement_blocked`: firm and recorded financing/labor/stock constraint; no effects.
- `intervention`: type `public_price_board`, at its declared boundary; no resource effects.

## Decisions (`decision-1`)

`id`, `kind` (`household` or `firm`), actor/day/product, policy/version, constraints, knowledge, feasible `alternatives`, `exclusions`, chosen action/shop or null, selection reason, actual checkout evidence if attempted, outcome, linked `event_ids`, `order_ids`, and `omitted` fields. Household shopping creates no delayed orders, so its `order_ids` is empty. Firm procurement links actual reserved order IDs; `explain` scans both explicitly linked events and the matching reservation/delivery events for those orders, including later days in the same run. A pending order has no invented settlement event. Audit retains all evaluated memories/candidates; summary removes knowledge/alternatives/exclusions and explicitly lists them in `omitted`. Constraints, choices, checkout/outcome and links remain. Both levels retain the same complete economic ledger and result.

Reason/outcome codes include `UNKNOWN_SHOP`, `INSUFFICIENT_CASH`, `OUT_OF_STOCK`, `BUDGET_LIMIT`, `TIME_LIMIT`, `STALE_PRICE`, `NO_FEASIBLE_OPTION`, `SELECTED_LOWEST_PERCEIVED_COST`, `PURCHASED`, `NO_PURCHASE`. Firm policies are `financed_payroll-1`, `paid_capacity_recipe-1`, `inventory_target-1`, `fixed-1`, and `inventory_band-1`. Their constraints contain actual own financing, worker/input capacity, stock/target/incoming orders, supplier quotes, or price bounds as appropriate. Outcomes include `PAID`, `INACTIVE`, `PRODUCED`, `NO_OUTPUT`, `RESERVED`, `SETTLED`, `NO_ORDER`, `UNCHANGED`, `PRICE_CHANGED`; reasons identify the executed policy condition. Firm knowledge is permitted supplier quotes, not household omniscience. Perceived household exclusions are separate from feasible alternatives; checkout has actual price/stock and stale status. `explain` scans recorded decisions and retrieves linked actual events, without retrospective invented intentions. Summary cannot restore omitted candidate details.

## Metrics (`metric-1`)

One record per completed day: product maps `production`, `consumption`, `unmet`; `wages`, `paid_workers`; `sales_cents`/`sales_units` split into wholesale/retail; individual quantity-weightable `realized_prices` from **successful sales only**; actual rejected `failed_attempts` (including stale-price checkout budget failures) / `stockouts`; `firm_net_cash_flow`; closing `household_cash`, `firm_cash`, per-owner `inventories`, and current `employment`. Missing aggregate product entries mean zero units, never a zero price. Per-household consumption/unmet/work and per-firm sales are available from actor-tagged economic events. Net cash flow is all cash credits minus debits for the day, not accrual profit. No growth or survival expectation is built into invariants.

## Display replay (`replay-1`)

One completed-day snapshot with `next_day`, full account balances/inventories/reservations, firm parameters/current prices, and household IDs/employment/needs. It is a minimal display contract, not a complete execution replay: knowledge details and decision reasoning live in audit decisions/checkpoints. Inventory/price charts and entity inspectors can read it without rerunning the engine. The initial display state is available in `initial_state.json`; a branch's earlier snapshots remain in the parent. Historical inspection is a linear scan of these periodic snapshots and event/decision files; the CLI `inspect` currently returns the final boundary only. No database or history index is required.

## Verification (`verification-1`)

Actual `passed` status, individual check statuses, explicit unavailable checks, event/period counts, total cash, and product totals. Missing/malformed required artifacts fail the command instead of becoming passes. Records are independently reconciled for effects/semantics, reservations/arrival timing, phase/ID order, paid production, daily household needs, metrics, final accounts/prices/board, decision links and display boundaries. Full independent policy re-execution and empirical validity are explicitly unavailable. Prior verification reports are never trusted as proof.

Employment configuration, records, vacancy-board state, daily labor metrics and comparison rules are specified in [employment.md](employment.md).
