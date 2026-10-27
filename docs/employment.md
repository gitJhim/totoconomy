# Employment: fixed and dynamic modes

Every run has an employment mode, recorded in `manifest.json` (`"employment"`) and in every
state file and checkpoint (`state.employment_mode`).

| Mode | Meaning |
|---|---|
| `fixed` (default) | Each household keeps the scenario's `employer` for the whole run. Original scenarios retain their economic behavior. The additional labor ledger and evidence fields change event IDs, serialized records and state digests. |
| `dynamic` | Firms set staffing targets from demand, stock-outs, capacity and payroll, advertise positions with wage offers and eligibility rules on a vacancy board, and households take or change jobs. Every change is scheduled first and takes effect on a later day, and every move records its reason. |

## Choosing the mode

- Command line: `econ run --scenario FILE --seed N --until-day D --out DIR --employment dynamic`
  (also `benchmark`). The default is `fixed`.
- Inspector: press `n` in the run list, move to the **Employment** field and toggle it with
  Space / Left / Right, then choose **Start run**. The run is written as a new directory under
  the runs directory; existing runs are never modified.
- `resume` and `branch` do not take the flag: they inherit the mode stored in the checkpoint.
- `./scripts/dev.sh start` runs the labor-market scenario in both modes next to the town demo.

Compare modes using two unmodified runs started at day zero with the same scenario and seed:

```sh
econ run --scenario scenarios/labor_market.json --seed 7 --until-day 40 --employment fixed --out runs/labor-fixed
econ run --scenario scenarios/labor_market.json --seed 7 --until-day 40 --employment dynamic --out runs/labor-dynamic
econ compare --baseline runs/labor-fixed --variant runs/labor-dynamic --from-day 0 --until-day 40 --json
```

`compare` proves that the initial states differ only in `employment_mode` and verifies both runs.
Its output labels both modes and reports wages, paid worker-days, employed/unemployed person-days,
hires, separations, job-search totals and labor use alongside production, consumption, unmet needs
and sales. A person-day is one household on one day; holding a job does not imply being paid.
A shorter shared comparison interval is allowed. Different-mode continuations and interventions
are rejected because they cannot establish a common day-zero starting point on their own.

This changes the persisted model to `econ-2`. Existing `econ-1` artifacts remain on disk, but the
new executable cannot inspect, verify or resume them; use the earlier executable for those runs.
No checkpoint migration is implemented.

## Holding a job, working, and being paid are three different facts

| Fact | Where it is recorded | Rule |
|---|---|---|
| Holds a job | `household.employer`, the open `EmploymentRecord` | Set only by the scenario (fixed) or by an `employment_changed` event on its effective day (dynamic). |
| Worked today | `metrics.labor[id].worked` | True only if the employer ran payroll today: it had at least `labor_required` workers and could finance the whole payroll. |
| Paid today | `wage` event, `metrics.labor[id].wage_paid` | Exactly one wage event per worker of a firm that ran payroll. A firm pays all of its workers or none. |

An employed household whose firm is inactive is **employed, idle and unpaid**; the record carries
`idle_reason` (`INSUFFICIENT_PAYROLL` or `INSUFFICIENT_LABOR`) and its `unpaid_days` streak grows.
A household hired with effective day *d* is unemployed, idle and unpaid on day *d - 1* even though
the hire is already scheduled.

## The day, per person

Phases run in this order; the verifier rejects ledgers that violate it.

1. `intervention` (branches only)
2. `arrivals` — wholesale deliveries settle.
3. `employment` — dynamic mode only:
   1. **Changes take effect.** Every pending change with `effective == today` is applied in
      schedule order: the open spell closes (`end = today`, `end_reason`), `employer` is updated,
      `unpaid_days` resets, and a new spell opens at the agreed wage if the change has a
      destination. Event: `employment_changed`.
   2. **Firms plan staffing** (decision `staffing_plan-1`, one per firm per day). Each firm
      computes the headcount it wants from what it can observe (see *Staffing targets*), then:
      - withdraws pending layoffs the new target no longer justifies
        (`employment_change_cancelled`, reason `STAFFING_TARGET_RECOVERED`);
      - schedules layoffs of any excess, newest spell first, effective `today + notice_days`
        (`employment_scheduled`, reason `LAYOFF_<driver>`);
      - keeps its vacancy-board posting in line with the gap between the target and who is
        staying or arriving (`posting_opened` with reason `HIRE_<driver>`, `posting_adjusted`
        on `openings` with `STAFFING_TARGET_RAISED` / `STAFFING_TARGET_LOWERED`, `posting_closed`);
      - manages its offer: if cash cannot cover `payroll_runway_days` of the current payroll plus
        the openings at the offered wage, the offer drops by `wage_step`, clamped at `wage_min` (`PAYROLL_LOW`).
        Any openings still unaffordable at that offer are removed, or the posting is withdrawn
        if none can be funded (`PAYROLL_LOW`); if the posting has gone
        `raise_after_unfilled_days` without an applicant the offer rises by `wage_step` up to
        `wage_max` (`UNFILLED`), or the decision records why not (`OFFER_AT_MAXIMUM`, `PAYROLL_LOW`).
      Every action appears in the decision's `alternatives` with its reason and linked event.
   3. **Households search** (decision `job_search-2`), in the same daily rotation as shopping.
      Who reads the board: every unemployed household; every paid worker whose
      `search_while_employed` is true (when the board is not empty); and every worker unpaid for
      at least its `unpaid_tolerance_days`. Households with a pending change do not search.
      Each posting is judged in two stages, and the decision records which stage said no:
      - *eligibility* (the firm's rules): `CURRENT_EMPLOYER`, `NO_OPENINGS`,
        `NOT_ELIGIBLE_EXPERIENCE`, `NOT_ELIGIBLE_EMPLOYED`, `REHIRE_COOLDOWN`;
      - *preference* (the household's rules, see *Job preferences*): `BELOW_RESERVATION_WAGE`,
        `TENURE_TOO_SHORT`, `INSUFFICIENT_WAGE_GAIN`.
      The household applies to the eligible, preferable posting with the highest **net wage**
      (ties: board order). The application consumes one opening at the advertised wage, which
      becomes the wage of the new spell, and is counted on the posting (`applications`).
      Outcomes: `HIRE_SCHEDULED` (from unemployment, change reason `HIRED`), `SWITCH_SCHEDULED`
      (from a paying job, `SWITCHED`), `QUIT_SCHEDULED` (from an unpaid job, `QUIT_UNPAID`),
      `DECLINED` (eligible offers, none preferable), `INELIGIBLE`, `NO_POSTINGS`.
      **Hires, switches and quits always take effect the next day**; only layoffs wait
      `notice_days`.
   4. Postings that received nobody today age by one `unfilled_days`; postings whose openings
      reached zero leave the board (`posting_closed`, `FILLED`).
4. `work` — each firm checks `labor_required` and payroll affordability against the jobs held
   *now* (including changes that took effect in step 3). Payroll is the sum of the wages agreed
   in the open spells. If it can be financed, the firm pays every worker, and each `wage`
   transaction delivers one worker-day of labor to the firm (see *Labor in the ledger*);
   otherwise it emits `work_inactive` and everyone stays idle and unpaid. Production then
   consumes labor: `min(capacity, output_per_worker x paid workers)` subject to inputs, using
   `ceil(quantity / output_per_worker)` worker-days. Whatever labor is left is released
   (`labor_released`). Streak counters update here.
5. `procurement` — retailers restock; when a supplier cannot fill an order, the shortfall is
   recorded (`procurement_blocked` / `supply_shortfall`, both naming the supplier).
6. `information`, `shopping`, `consumption`, `pricing` — unchanged.
7. End of day: each firm appends what it observed of demand today (units it sold plus units
   buyers wanted from it but could not get) to `demand_history`, a rolling window of
   `demand_window_days`.

Because employment precedes work, a change scheduled today is never in force today, and a
change effective today is in force for today's payroll.

## Labor in the ledger

Employment feeds the rest of the economy through recorded transactions, not implicitly:

| Transaction | Effects | Meaning |
|---|---|---|
| `wage` | firm cash −w, household cash +w, firm labor +1 | payroll buys one worker-day (`labor_units: 1`) |
| `production` | firm inputs −, output +, firm labor −`labor_used` | output consumes the worker-days it needs; `labor_idle` is the rest of the paid crew |
| `labor_released` | firm labor −units | unused worker-days leave the ledger the same day: `idle` at a producer, `retail_service` at a retailer |

`account.labor` is therefore positive only inside the `work` phase; every state at a day boundary
has `labor == 0`, and the verifier rejects ledgers where labor goes negative, where production
uses more than payroll delivered, or where labor survives the day. Production without a financed
payroll is impossible by construction: a firm that cannot pay has no labor to consume. Households
close the circuit by spending wages at retail (`trade`) and consuming (`consumption`).

Per day (`metrics.jsonl`): `labor_units: {delivered, used, idle, retail_service}` for the town and
`labor_flow` per firm, both reconciled against the events.

## Reading it in the inspector

The inspector presents the same data at three levels:

1. **2 Overview** — the day's circuit in three lines: labor (worker-days bought, used, idle,
   service), goods (made → wholesale → retail → consumed / unmet) and cash (wages → households,
   retail → shops, wholesale → producers), next to the employment and job-search summaries.
2. **7 Ledger** — one line per entity for the selected day: closing cash and the day's net,
   payroll or wages, sales or spending, labor bought and used, goods made, sold, bought, eaten or
   missed. The right pane breaks the selected entity down.
3. **Enter on an entity** — its transactions for the day in ledger order, each with the cash,
   labor and goods it moved for that entity and the running cash balance; **Enter** again opens
   the full recorded event. **Esc** climbs back one level at a time.

**3 Entities**, **4 Decisions**, **5 Events** and **6 Labor** remain the flat views; `d` / `e` on
an entity or ledger line jumps to its decisions or events for the day.

## Staffing targets

`staffing_plan-1` derives the headcount a firm wants from observable factors only; the decision's
`constraints` record the observed factors and target. Agreed contract wages and seniority come
from the employment spells; together these records explain the payroll limit and layoff order.

| Factor | Producer | Retailer |
|---|---|---|
| Demand | average of `demand_history` (sold + stock-out shortfalls) once the window is full; until then the firm staffs for capacity (`INSUFFICIENT_HISTORY`) | not used: retail staff do not limit sales in this model (`OVERHEAD_STAFFING`) |
| Inventory | `wanted_output = demand + (demand x inventory_cover_days - on_hand) / inventory_cover_days`, clamped to `[0, capacity]` | — |
| Capacity | crew = `ceil(desired_output / output_per_worker)`, at least `labor_required` while demand is observed, at most `min(positions, crew for capacity)` | `positions` |
| Payroll left | retain oldest contracts at their agreed wages within `available_cash / payroll_runway_days`, then budget any additional workers at the base wage. If the target exceeds this affordable headcount, reduce it, or use 0 for a producer that cannot carry its minimum crew (`PAYROLL_RUNWAY`) | same |

Reasons: `INSUFFICIENT_HISTORY`, `NO_DEMAND`, `INVENTORY_SURPLUS`, `RESTOCKING`, `DEMAND_MATCHED`,
`CAPACITY_LIMIT`, `PAYROLL_RUNWAY`, `OVERHEAD_STAFFING`. Layoffs cite `LAYOFF_NO_DEMAND`,
`LAYOFF_INVENTORY_SURPLUS`, `LAYOFF_PAYROLL_RUNWAY`, `LAYOFF_DEMAND_BELOW_STAFFING`,
`LAYOFF_ABOVE_CAPACITY` (more crew than capacity can use) or `LAYOFF_ABOVE_POSITIONS` (a retailer
staffed beyond its positions); postings cite `HIRE_<reason>`.

`output_per_worker` defaults to `max(1, ceil(capacity / labor_required))`, so with the minimum crew a
producer makes exactly `capacity` — the original behaviour. Set it lower to let output grow with
the crew up to `capacity`.

## The vacancy board

`state.board[]` holds at most one `Posting` per firm:
`{ id: "posting:N", firm, openings, wage, min_experience_days, accept_employed,
rehire_cooldown_days, posted, unfilled_days, reason }`. The eligibility fields are copied from the
firm's `hiring` policy when the posting opens. Experience is the total number of days a household
has held any job (from its employment records); the rehire cooldown applies to someone the same
firm laid off (`end_reason` starting with `LAYOFF_`) fewer than `rehire_cooldown_days` ago.

Per firm (`hiring` in the scenario, all optional):

```json
"hiring": {
  "wage_step": 100,                 // default wage / 10
  "wage_min": 900,                  // default wage; must not exceed wage
  "wage_max": 1300,                 // default wage + 3 x wage_step; must not be below wage
  "raise_after_unfilled_days": 2,
  "min_experience_days": 2,
  "accept_employed": true,
  "rehire_cooldown_days": 2
}
```

## Job preferences

How a household values an offer. The *net wage* of a job is its wage minus the commute,
`firm.travel_minutes x household.time_value`; the current job is valued the same way, or at 0
when it is not paying (unpaid beyond tolerance).

| Rule | Meaning |
|---|---|
| `reservation_wage` | lowest net wage the household takes at all (default 0) |
| `min_wage_gain` | absolute net gain required to leave a paying job (default 0 c) |
| `min_wage_gain_percent` | relative net gain required to leave a paying job (default 10) |
| `min_tenure_days` | days in the current job before it considers switching (default 5) |
| `search_while_employed` | whether a paid worker reads the board at all (default true) |
| `unpaid_tolerance_days` | unpaid days after which any offer at or above the reservation wage is preferable (default 3) |

Set scenario-wide defaults under `employment.job_preferences` and override any field per
household under `households[].job_preferences`; the normalized scenario carries the merged
rules for every household, and the decision's `constraints.preferences` repeats them.

## Search metrics

Per day (`metrics.jsonl`, `job_search`; the inspector's Overview shows them with an acceptance
rate = applied / (applied + declined)):

```json
"job_search": {
  "searching": 12, "applied": 4, "hires": 3, "switches": 0, "quits": 1,
  "declined": 1, "ineligible": 7, "no_postings": 0,
  "declined_reasons": {"BELOW_RESERVATION_WAGE": 1},
  "ineligible_reasons": {"CURRENT_EMPLOYER": 4, "NOT_ELIGIBLE_EXPERIENCE": 1, "NO_OPENINGS": 11}
}
```

`searching`, `applied`, `declined`, `ineligible` and `no_postings` count households by outcome;
`hires`/`switches`/`quits` split the applications; the reason maps count postings judged.
Each person's daily `labor` record carries `search: {outcome, reason, chosen, declined, ineligible}`
(or `null` when the household did not read the board), and every posting reports
`applications` received so far.

## Records

State (`state.json`, checkpoints, `inspect --entity`):

- `employment[]` — `EmploymentRecord { id: "employment:N", household, employer, start, end, wage,
  reason, end_reason }`. `end` is `-1` and `end_reason` empty while the spell is open. Initial jobs
  have reason `INITIAL`, `start` equal to the first simulated day and the firm's wage. Each employed
  household has exactly one open spell naming its employer.
- `pending_changes[]` — `EmploymentChange { id: "change:N", household, from_employer, to_employer,
  requested, effective, wage, posting, reason }`. Empty `from_employer` is a hire from unemployment;
  empty `to_employer` is a separation (wage 0, no posting). At most one pending change per household;
  household-initiated changes are effective `requested + 1`, layoffs `requested + notice_days`.
- `household.job_preferences` — see *Job preferences*.
- `board[]` — the vacancy board, see above.
- `household.unpaid_days` — consecutive days holding a job without a wage.
- `firm.positions` — maximum headcount (scenario field; defaults to the scenario's staffing).
- `firm.output_per_worker`, `firm.hiring` — see above.
- `firm.payroll_missed_days` — consecutive days with workers present but no payroll run.
- `firm.demand_history` — the rolling demand observations, newest last.

Per day (`metrics.jsonl`, shown in the inspector's **6 Labor** view together with the board):

```json
"labor": {
  "household:4": {
    "employer": "firm:1", "status": "employed",
    "worked": false, "wage_paid": 0, "idle_reason": "INSUFFICIENT_PAYROLL", "unpaid_days": 3,
    "scheduled": {"id": "change:1", "from_employer": "firm:1", "to_employer": "",
                  "effective": 12, "wage": 0, "posting": "", "reason": "LAYOFF_PAYROLL_RUNWAY"},
    "change": null, "cancelled": null,
    "search": {"outcome": "DECLINED", "reason": "NO_PREFERABLE_OFFER", "chosen": null,
               "declined": 1, "ineligible": 0}
  }
},
"employed": 11, "unemployed": 1, "hires": 0, "separations": 0, "pending_changes": 1,
"job_search": {"searching": 11, "applied": 0, "...": "see Search metrics"},
"inactive_firms": {"firm:1": "INSUFFICIENT_PAYROLL"},
"board": [{"id": "posting:3", "firm": "firm:2", "openings": 2, "wage": 1100, "unfilled_days": 1, ...}],
"board_activity": [{"event": "event:88", "type": "posting_adjusted", "firm": "firm:2",
                    "posting": "posting:3", "reason": "UNFILLED"}],
"demand": {"firm:1": 16, "firm:2": 8}
```

`change` describes a change that took effect that day, `scheduled` one that was promised that day,
`cancelled` a layoff withdrawn that day.

## Verification

`econ verify` reconstructs jobs, spells, pending changes, the vacancy board, demand observations
and streak counters from the event log alone and checks them against every day's `labor`,
`board`, `board_activity` and `demand` records and the final state
(`per_person_labor_records_reconcile`, `employment_records_and_pending_changes_reconcile`).
It also requires that every `wage` event pays the wage of the household's open spell, that every
hire answers a live posting at its advertised wage and starts the next day, that offers stay
within the firm's bounds, that layoff timing matches the configured notice, that the application/hire/switch/quit counts match the scheduled
changes, and (`job_search_metrics_reconcile`) that the search totals and each person's search
record agree with the recorded `job_search-2` decisions;
that production never exceeds `output_per_worker x paid workers`, that changes only take effect
on their effective day, and that fixed-mode runs contain no employment-phase activity. The
staffing policy itself is not re-executed by the verifier; its inputs and outputs are recorded in
each `staffing_plan-1` decision instead.

## Scenario parameters

```json
"employment": {
  "notice_days": 1,
  "demand_window_days": 5,
  "inventory_cover_days": 3,
  "payroll_runway_days": 3,
  "job_preferences": { "min_wage_gain_percent": 10, "min_tenure_days": 5, "...": "see Job preferences" }
}
```

All are optional, must be positive, and are inert under fixed employment. A household may start
unemployed with `"employer": ""`; under fixed employment it then never works. A firm's `positions`
may be set explicitly to let it grow beyond its initial staffing. `scenarios/labor_market.json`
exercises hiring, eligibility rules, rising offers, layoffs and quitting within 20 days.

## Integration review

The imported implementation included the vacancy board, job preferences, staffing plans,
next-day hires and switches, layoffs with notice, employment histories, checkpoint support,
and the inspector's launch, Labor and Ledger views. Integration corrected these issues:

- Fixed/dynamic runs were rejected by `compare`; mode experiments now validate common initial
  conditions and include employment and labor totals.
- Staffing affordability used the base wage despite more expensive existing contracts. It now
  budgets agreed wages in the same seniority order used for layoffs.
- Lowering an offer once could leave unaffordable openings available for immediate application.
  The board now reduces or withdraws the remaining unfunded openings before households search.
- Ceiling division and wage-step expressions could overflow signed integers. Safe division,
  capped output multiplication and bounded wage steps handle large values; other overflowing
  aggregate arithmetic raises an explicit error.
- A zero-capacity producer normalized to a zero per-worker output that failed on a second
  normalization. Its default per-worker output is now positive, with actual output still zero.
- The verifier did not check manifest employment labels, layoff notice timing, full payroll
  coverage, posting eligibility or agreement between successful search decisions and hires.
  These are now checked, including summary traces where applicable.
- At small supported terminal sizes, later new-run form fields were hidden. The form now scrolls
  to keep the selected field and Start action visible.
- Payroll-miss streaks remained nonzero after the last worker left an inactive firm. They now
  reset when no workers remain.

The model still has deliberate limits: retail employees do not constrain sales throughput;
offer changes affect future contracts, while incumbent contract wages stay fixed; a quit is a
move to another accepted job; and there is no permanent bankruptcy, bargaining or empirical
calibration. No particular mode is expected to produce a better economic outcome.

Validation on 2026-10-26, Release build `0.1.0-731cb536a3119686`: all six CTest
entries passed in Debug, Release and ASan/UBSan configurations. The 30-day accounting and
60-day town runs matched every original daily metric and closing account in fixed mode.
The full `scripts/dev.sh start` workflow, including the 40-day mode comparison, completed in
`runs/labor-integration-20261026`; its `reports/integration-validation.json` records the checks.
The earlier local executable is preserved at `build/legacy-econ-1/econ` for existing runs.
