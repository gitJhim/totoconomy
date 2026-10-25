# Status and staged exit criteria

The eight long-term areas are runtime/persistence; monetary/inventory accounting; households; firms/employment; logistics; information/memory; evidence/experiments; investigation/export. Each stage below expands these observable contracts rather than adding empty interfaces.

## Stage 1: executable foundation

Implemented: C++20 library/CLI, offline dependency, checked cash and unit accounting, fixed employment/paid renewable or input recipes, immediate/full-fill trades, reservations/delays, real accounting/town scenarios, constrained observed-price policy, refresh/discovery/public board, bounded inventory price policy, streamed facts/evidence/metrics/display states, complete-boundary checkpoints, resume/no-op/board branches, strict paired comparison, independent ledger verifier, test gates, and benchmark measurement.

Limited by design: employment stays fixed; inactivity has no permanent bankruptcy; one supplier per product; no order cancellation; notice discovery is free rather than a geographic search; no same-day checkout retry or substitution across products; display exports have daily resolution and no player. Verification independently reconciles records but does not independently regenerate all beliefs. These are explicit Stage 1 rules, not hidden placeholders.

Exit: clean offline source builds; exact accounting and synthetic town run; every documented acceptance gate executes in Debug/Release/sanitizer where supported; persisted evidence reconciles; continuous/resume/no-op histories match; the board experiment executes without a guaranteed preferred outcome; measured results and limitations are documented.

## Stage 2: richer economic behavior (next)

Depends on Stage 1 accounting and explanation contracts. Add deeper recipes and interdependent procurement, stockout-aware demand estimation, substitution and better bounded pricing, employment changes, explicit business inactivity/closure, and initialization diagnostics. First concrete increment: add a grain→flour→bread scenario with input-constrained production, then prove paid labor, upstream shortages and unmet household needs through actual linked facts. Exit: every new action has independently reconciled cash/goods flows, inactivity and closure are distinct, households cannot exceed resources, observed-demand policies distinguish stockouts from zero demand, and scenario expectations are tested separately from invariants.

## Stage 3: geography and information

Depends on sound delayed logistics and knowledge boundaries. Add roads, route choice, vehicle/shipment capacity, disruptions, explicit physical search/time budgets, advertisements, observation aging and information sharing. Exit: a blocked route changes actual arrivals; conserved goods have exactly one owner/location; route/search decisions use permitted information and time; stale/false/shared observations retain provenance and explain their effects. No visual engine may bypass economic settlement.

## Stage 4: experiment laboratory

Depends on richer behavior plus matched schedules. Add controlled interventions, independent external shock keys, batches/sweeps, distributions and assumption robustness, evidence navigation and optional automation-ownership/resilience experiments after their financing mechanisms exist. Exit: paired inputs and permitted differences are validated, outside shocks stay aligned despite differing action counts, statistical summaries preserve individual evidence, and conclusions report variation/tradeoffs rather than requiring a policy to win.

## Stage 5: investigation and presentation

Depends on stable export versions and experiment semantics. Build a separate viewer/renderer, daily/subday navigation where recorded, linked household/firm explanations, synchronized branch comparisons, later vertical-video export, profiling and documentation hardening. Exit: displayed values and explanations match persisted facts, presentation replay never runs policy logic, exported media is checked against the recorded run, and representative performance is measured. A renderer remains a consumer of the C++ core's outputs.

Deferred dependencies: 3D game engine, vehicle physics, multiplayer, backend/cloud, banks/credit, national calibration, resident LLMs and reinforcement learning. Add none without a concrete later requirement and an explicit model contract.

## Interactive inspector increment

A terminal investigation viewer is now implemented as a separate recorded-data consumer: per-day browsing, entities, decisions/events and linked evidence. This adds an initial part of Stage 5 investigation, without replacing the still-planned graphical presentation, synchronized visual branch comparisons, subday rendering or video export. See `TUI.md`.
