#include <algorithm>
#include <econ/econ.hpp>
#include <limits>
#include <set>
namespace econ {
Engine::Engine(State s, Json config, Sink ev, Sink dec, std::string level)
    : state(std::move(s)), scenario(std::move(config)), events(std::move(ev)), decisions(std::move(dec)),
      trace(std::move(level)) {}
static void increment(Json &j, const std::string &key, Amount n) {
    if (j.is_null())
        j = Json::object();
    j[key] = add(j.value(key, Amount{0}), n);
}
Json Engine::emit(const std::string &type, Json data, Json effects) {
    if (state.next_event >= static_cast<std::uint64_t>(std::numeric_limits<Amount>::max()))
        throw Error(5, "event ID exhausted");
    // Stage all account changes, including overflow checks, before any economic mutation.
    std::map<std::string, Account> staged;
    for (const auto &e : effects) {
        auto id = e.at("actor").get<std::string>();
        if (!staged.contains(id))
            staged[id] = account(state, id);
        auto &a = staged[id];
        auto f = e.at("field").get<std::string>();
        auto p = e.at("product").get<std::string>();
        auto d = integer(e.at("delta"));
        if (f == "cash")
            a.cash = add(a.cash, d);
        else if (f == "reserved_cash")
            a.reserved_cash = add(a.reserved_cash, d);
        else if (f == "stock")
            a.stock[p] = add(stock(a.stock, p), d);
        else if (f == "reserved_stock")
            a.reserved_stock[p] = add(stock(a.reserved_stock, p), d);
        else if (f == "labor")
            a.labor = add(a.labor, d);
        else
            throw Error(6, "unknown effect field");
    }
    for (const auto &[id, a] : staged) {
        (void)id;
        if (a.cash < 0 || a.reserved_cash < 0 || a.reserved_cash > a.cash || a.labor < 0)
            throw Error(5, "atomic effect invalid cash");
        for (auto &[p, q] : a.stock)
            if (q < 0 || stock(a.reserved_stock, p) < 0 || stock(a.reserved_stock, p) > q)
                throw Error(5, "atomic effect invalid stock");
    }
    for (auto &[id, a] : staged)
        account(state, id) = std::move(a);
    Json event = {{"schema", "event-1"},   {"id", "event:" + std::to_string(state.next_event++)},
                  {"day", state.next_day}, {"phase", phase},
                  {"type", type},          {"data", data},
                  {"effects", effects}};
    if (!daily.is_null()) {
        if (type == "wage") {
            increment(daily, "paid_workers", 1);
            increment(daily, "wages", integer(data.at("amount")));
            auto &person = daily["labor"][data.at("household").get<std::string>()];
            person["worked"] = true;
            person["wage_paid"] = integer(data.at("amount"));
            increment(daily["labor_units"], "delivered", integer(data.at("labor_units")));
            increment(daily["labor_flow"][data.at("employer").get<std::string>()], "delivered",
                      integer(data.at("labor_units")));
        }
        if (type == "production") {
            increment(daily["labor_units"], "used", integer(data.at("labor_used")));
            increment(daily["labor_flow"][data.at("firm").get<std::string>()], "used",
                      integer(data.at("labor_used")));
        }
        if (type == "labor_released") {
            auto use = data.at("use").get<std::string>();
            increment(daily["labor_units"], use, integer(data.at("units")));
            increment(daily["labor_flow"][data.at("firm").get<std::string>()], use,
                      integer(data.at("units")));
        }
        if (type == "work_inactive")
            daily["inactive_firms"][data.at("firm").get<std::string>()] = data.at("reason");
        if (type == "employment_scheduled")
            daily["labor"][data.at("household").get<std::string>()]["scheduled"] = {
                {"id", data.at("change").at("id")},
                {"from_employer", data.at("from_employer")},
                {"to_employer", data.at("to_employer")},
                {"effective", data.at("effective")},
                {"wage", data.at("wage")},
                {"posting", data.at("posting")},
                {"reason", data.at("reason")}};
        if (type == "employment_changed") {
            daily["labor"][data.at("household").get<std::string>()]["change"] = {
                {"id", data.at("change").at("id")},
                {"from_employer", data.at("from_employer")},
                {"to_employer", data.at("to_employer")},
                {"wage", data.at("change").at("wage")},
                {"reason", data.at("reason")}};
            if (!data.at("from_employer").get<std::string>().empty())
                increment(daily, "separations", 1);
            if (!data.at("to_employer").get<std::string>().empty())
                increment(daily, "hires", 1);
        }
        if (type == "production")
            increment(daily["production"], data.at("product"), integer(data.at("quantity")));
        if (type == "consumption")
            increment(daily["consumption"], data.at("product"), integer(data.at("quantity")));
        if (type == "unmet")
            increment(daily["unmet"], data.at("product"), integer(data.at("quantity")));
        if (type == "trade") {
            auto stage = data.at("stage").get<std::string>();
            increment(daily["sales_cents"], stage, integer(data.at("amount")));
            increment(daily["sales_units"][stage], data.at("product"), integer(data.at("quantity")));
            daily["realized_prices"].push_back({{"stage", stage},
                                                {"product", data.at("product")},
                                                {"price", data.at("price")},
                                                {"quantity", data.at("quantity")}});
        }
        if (type == "trade_rejected") {
            increment(daily, "failed_attempts", 1);
            if (data.at("reason") == "OUT_OF_STOCK")
                increment(daily, "stockouts", 1);
        }
        if (type == "employment_change_cancelled")
            daily["labor"][data.at("household").get<std::string>()]["cancelled"] = {
                {"id", data.at("change").at("id")},
                {"from_employer", data.at("from_employer")},
                {"to_employer", data.at("to_employer")},
                {"reason", data.at("reason")}};
        if (type == "posting_opened" || type == "posting_adjusted" || type == "posting_closed")
            daily["board_activity"].push_back(
                {{"event", event.at("id")},
                 {"type", type},
                 {"firm", data.at("firm")},
                 {"posting", type == "posting_opened" ? data.at("posting").at("id") : data.at("posting")},
                 {"reason", data.at("reason")}});
        for (const auto &e : effects)
            if (e["field"] == "cash" && e.at("actor").get<std::string>().rfind("firm:", 0) == 0)
                increment(daily["firm_net_cash_flow"], e.at("actor"), integer(e.at("delta")));
    }
    // What sellers observe of demand: units they sold, plus units buyers wanted but could not get.
    if (type == "trade")
        sold_today[data.at("seller").get<std::string>()] =
            add(sold_today[data.at("seller").get<std::string>()], integer(data.at("quantity")));
    if (type == "trade_rejected" && data.at("reason") == "OUT_OF_STOCK")
        shortfall_today[data.at("seller").get<std::string>()] =
            add(shortfall_today[data.at("seller").get<std::string>()], integer(data.at("quantity")));
    if (type == "procurement_blocked" && data.at("reason") == "OUT_OF_STOCK")
        shortfall_today[data.at("supplier").get<std::string>()] =
            add(shortfall_today[data.at("supplier").get<std::string>()], integer(data.at("desired")));
    if (type == "supply_shortfall")
        shortfall_today[data.at("supplier").get<std::string>()] =
            add(shortfall_today[data.at("supplier").get<std::string>()], integer(data.at("shortfall")));
    if (events)
        events(event);
    return event;
}
void Engine::settle(const Order &o, bool reserved, const std::string &stage) {
    auto amount = mul(o.quantity, o.price);
    Json effects = {effect(o.buyer, "cash", -amount), effect(o.seller, "cash", amount),
                    effect(o.seller, "stock", -o.quantity, o.product),
                    effect(o.buyer, "stock", o.quantity, o.product)};
    if (reserved) {
        effects.push_back(effect(o.buyer, "reserved_cash", -amount));
        effects.push_back(effect(o.seller, "reserved_stock", -o.quantity, o.product));
    }
    emit("trade",
         {{"buyer", o.buyer},
          {"seller", o.seller},
          {"product", o.product},
          {"quantity", o.quantity},
          {"price", o.price},
          {"amount", amount},
          {"stage", stage},
          {"order", reserved ? Json(o.id) : Json(nullptr)}},
         effects);
}
std::string Engine::trade(const std::string &buyer, const std::string &seller, const std::string &product,
                          Amount q, Amount price, const std::string &stage, Day delay) {
    if (buyer == seller || q <= 0 || price <= 0 || delay < 0 ||
        !account(state, buyer).stock.contains(product))
        throw Error(2, "invalid trade");
    auto &b = account(state, buyer);
    auto &s = account(state, seller);
    auto amount = mul(q, price);
    std::string reason;
    if (b.cash - b.reserved_cash < amount)
        reason = "INSUFFICIENT_CASH";
    else if (stock(s.stock, product) - stock(s.reserved_stock, product) < q)
        reason = "OUT_OF_STOCK";
    if (!reason.empty()) {
        emit("trade_rejected", {{"buyer", buyer},
                                {"seller", seller},
                                {"product", product},
                                {"quantity", q},
                                {"price", price},
                                {"stage", stage},
                                {"reason", reason}});
        return "";
    }
    // Preflight destination capacity before reservation or settlement.
    add(s.cash, amount);
    add(stock(b.stock, product), q);
    auto event = "event:" + std::to_string(state.next_event);
    Order o{"", buyer, seller, product, q, price, add(state.next_day, delay)};
    if (delay) {
        if (state.next_order >= static_cast<std::uint64_t>(std::numeric_limits<Amount>::max()))
            throw Error(5, "order ID exhausted");
        o.id = "order:" + std::to_string(state.next_order);
        emit("order_reserved", {{"order", o}, {"stage", stage}},
             {effect(buyer, "reserved_cash", amount), effect(seller, "reserved_stock", q, product)});
        state.orders.push_back(o);
        ++state.next_order;
    } else
        settle(o, false, stage);
    return event;
}
void Engine::arrivals() {
    for (std::size_t i = 0; i < state.orders.size();) {
        auto o = state.orders[i];
        if (o.arrival > state.next_day) {
            ++i;
            continue;
        }
        if (o.arrival < state.next_day)
            throw Error(5, "overdue order");
        settle(o, true, "wholesale");
        state.orders.erase(state.orders.begin() + static_cast<std::ptrdiff_t>(i));
    }
}
static Json firm_evidence(const std::string &actor, const std::string &policy, Json constraints,
                          const std::string &product = "") {
    return {{"kind", "firm"},
            {"actor", actor},
            {"policy", policy},
            {"product", product},
            {"constraints", constraints},
            {"knowledge", Json::array()},
            {"alternatives", Json::array()},
            {"exclusions", Json::array()},
            {"chosen", nullptr},
            {"event_ids", Json::array()},
            {"order_ids", Json::array()}};
}
void Engine::record_decision(Json d) {
    if (state.next_decision >= static_cast<std::uint64_t>(std::numeric_limits<Amount>::max()))
        throw Error(5, "decision ID exhausted");
    d["schema"] = "decision-1";
    d["id"] = "decision:" + std::to_string(state.next_decision++);
    d["day"] = state.next_day;
    if (!d.contains("kind"))
        d["kind"] = "household";
    if (trace == "summary") {
        d.erase("knowledge");
        d.erase("alternatives");
        d.erase("exclusions");
        d["omitted"] = {"knowledge", "alternatives", "exclusions"};
    } else
        d["omitted"] = Json::array();
    if (decisions)
        decisions(d);
}
static Money wage_of(const State &s, const std::string &household) {
    for (auto &r : s.employment)
        if (r.household == household && r.end == -1)
            return r.wage;
    throw Error(5, "no open employment spell for " + household);
}
// Staffing a firm wants today, from what it can observe: recent demand (units sold plus stock-out
// shortfalls), inventory cover, production capacity per worker, and the payroll its cash can carry.
// Returns the target plus every factor, so the decision record explains the number.
Json Engine::staffing_target(const Firm &f, Amount staying, Amount leaving) {
    const auto &rules = scenario.at("employment");
    const auto window = integer(rules.at("demand_window_days")),
               cover = integer(rules.at("inventory_cover_days")),
               runway = integer(rules.at("payroll_runway_days"));
    const auto &a = account(state, f.id);
    const auto available = a.cash - a.reserved_cash, payroll_now = daily_payroll(state, f.id);
    Json factors = {{"positions", f.positions},
                    {"labor_required", f.labor_required},
                    {"staying", staying},
                    {"leaving", leaving},
                    {"available_cash", available},
                    {"payroll_per_day", payroll_now},
                    {"payroll_runway_days", runway},
                    {"demand_window_days", window},
                    {"observed_days", f.demand_history.size()},
                    {"demand_history", f.demand_history}};
    Amount target = f.positions;
    std::string reason = "OVERHEAD_STAFFING"; // retail staff do not limit sales in this model
    if (f.kind == "producer") {
        auto crew_for = [&](Amount output) {
            if (output <= 0)
                return Amount{0};
            return std::max(f.labor_required, ceil_div(output, f.output_per_worker));
        };
        auto max_workers = std::min(f.positions, crew_for(f.capacity));
        factors["capacity"] = f.capacity;
        factors["output_per_worker"] = f.output_per_worker;
        factors["max_workers"] = max_workers;
        if (static_cast<Amount>(f.demand_history.size()) < window) {
            // Too little evidence to second-guess the scenario: run at capacity.
            target = max_workers;
            reason = "INSUFFICIENT_HISTORY";
            factors["desired_output"] = f.capacity;
        } else {
            Amount sum = 0;
            for (auto q : f.demand_history)
                sum = add(sum, q);
            auto demand = ceil_div(sum, window);
            auto on_hand = stock(a.stock, f.output) - stock(a.reserved_stock, f.output);
            auto wanted = add(demand, (mul(demand, cover) - on_hand) / cover); // demand plus cover correction
            auto desired = std::clamp(wanted, Amount{0}, f.capacity);
            factors["demand_per_day"] = demand;
            factors["on_hand"] = on_hand;
            factors["inventory_cover_target"] = mul(demand, cover);
            factors["wanted_output"] = wanted;
            factors["desired_output"] = desired;
            // While any demand is observed the minimum crew keeps running; surplus stock only trims
            // the crew above that floor. Only a demand-free window idles the plant entirely.
            auto crew = demand == 0 ? Amount{0} : std::max(f.labor_required, crew_for(desired));
            target = std::min(crew, max_workers);
            reason = demand == 0          ? "NO_DEMAND"
                     : crew > max_workers ? "CAPACITY_LIMIT"
                     : wanted < demand    ? "INVENTORY_SURPLUS"
                     : wanted > demand    ? "RESTOCKING"
                                          : "DEMAND_MATCHED";
        }
    }
    // Retain the oldest contracts first, matching the newest-first layoff order. Base wages alone
    // understate the cost of workers hired at raised offers. Voluntary departures do not need funding.
    std::vector<const EmploymentRecord *> contracts;
    for (const auto &r : state.employment)
        if (r.employer == f.id && r.end == -1) {
            bool departing = false;
            for (const auto &c : state.pending_changes)
                if (c.household == r.household && !c.to_employer.empty())
                    departing = true;
            if (!departing)
                contracts.push_back(&r);
        }
    std::sort(contracts.begin(), contracts.end(), [](const auto *left, const auto *right) {
        return std::pair(left->start, std::stoull(left->id.substr(11))) <
               std::pair(right->start, std::stoull(right->id.substr(11)));
    });
    auto remaining = available / runway;
    Amount affordable = 0;
    for (auto *r : contracts) {
        if (r->wage > remaining)
            break;
        remaining -= r->wage;
        ++affordable;
    }
    if (affordable == static_cast<Amount>(contracts.size()))
        affordable = f.wage > 0 ? affordable + std::min(remaining / f.wage,
                                                        std::numeric_limits<Amount>::max() - affordable)
                                : std::numeric_limits<Amount>::max();
    factors["affordable_workers"] = affordable;
    factors["unconstrained_target"] = target;
    if (target > affordable) {
        // A producer that cannot carry its minimum crew for the runway is mothballed rather than
        // left with an idle crew that can never produce.
        target = f.kind == "producer" && affordable < f.labor_required ? 0 : affordable;
        reason = "PAYROLL_RUNWAY";
    }
    return {{"target", target}, {"reason", reason}, {"factors", factors}};
}
// Dynamic employment runs before work so a change effective today is in force for today's payroll.
// 1. Changes whose effective day is today take effect (records close/open, employer updates).
// 2. Each firm computes its staffing target (staffing_target), then cancels pending layoffs the
//    target no longer justifies, schedules layoffs of the excess (newest spell first), and keeps
//    the vacancy board in line: opening or resizing its posting, raising the offer when it stays
//    unfilled, lowering or withdrawing it when payroll is tight. Every action records its reason.
// 3. Households read the board: the unemployed, paid workers whose preferences allow a search,
//    and workers unpaid beyond their tolerance. They apply to the eligible, preferable posting with
//    the highest net wage (ties: board order). Hires, switches and quits take effect tomorrow;
//    layoffs after notice_days. Nothing is applied on the spot.
// Under fixed employment this phase is a no-op, so fixed runs are unaffected.
void Engine::employment() {
    if (state.employment_mode != employment_dynamic)
        return;
    const auto &rules = scenario.at("employment");
    const auto notice = integer(rules.at("notice_days")), runway = integer(rules.at("payroll_runway_days"));
    const Day today = state.next_day;
    auto household = [&](const std::string &id) -> Household & {
        for (auto &h : state.households)
            if (h.id == id)
                return h;
        throw Error(2, "unknown household: " + id);
    };
    auto pending_for = [&](const std::string &id) -> const EmploymentChange * {
        for (auto &c : state.pending_changes)
            if (c.household == id)
                return &c;
        return nullptr;
    };
    auto posting_of = [&](const std::string &firm_id) -> Posting * {
        for (auto &p : state.board)
            if (p.firm == firm_id)
                return &p;
        return nullptr;
    };
    for (std::size_t i = 0; i < state.pending_changes.size();) {
        auto c = state.pending_changes[i];
        if (c.effective > today) {
            ++i;
            continue;
        }
        if (c.effective < today)
            throw Error(5, "overdue employment change");
        auto &h = household(c.household);
        if (h.employer != c.from_employer)
            throw Error(5, "employment change no longer matches current job");
        Json closed = nullptr, opened = nullptr;
        for (auto &r : state.employment)
            if (r.household == h.id && r.end == -1) {
                r.end = today;
                r.end_reason = c.reason;
                closed = r.id;
            }
        h.employer = c.to_employer;
        h.unpaid_days = 0;
        if (!c.to_employer.empty()) {
            if (state.next_employment >= static_cast<std::uint64_t>(std::numeric_limits<Amount>::max()))
                throw Error(5, "employment ID exhausted");
            EmploymentRecord r{"employment:" + std::to_string(state.next_employment++),
                               h.id,
                               c.to_employer,
                               today,
                               -1,
                               c.wage,
                               c.reason,
                               ""};
            state.employment.push_back(r);
            opened = r.id;
        }
        state.pending_changes.erase(state.pending_changes.begin() + static_cast<std::ptrdiff_t>(i));
        emit("employment_changed", {{"household", h.id},
                                    {"from_employer", c.from_employer},
                                    {"to_employer", c.to_employer},
                                    {"reason", c.reason},
                                    {"change", c},
                                    {"closed_record", closed},
                                    {"opened_record", opened}});
    }
    auto schedule = [&](const std::string &who, const std::string &from, const std::string &to, Day effective,
                        Money wage, const std::string &posting, const std::string &reason) {
        if (state.next_change >= static_cast<std::uint64_t>(std::numeric_limits<Amount>::max()))
            throw Error(5, "employment change ID exhausted");
        EmploymentChange c{"change:" + std::to_string(state.next_change++),
                           who,
                           from,
                           to,
                           today,
                           effective,
                           wage,
                           posting,
                           reason};
        state.pending_changes.push_back(c);
        return emit("employment_scheduled", {{"household", who},
                                             {"from_employer", from},
                                             {"to_employer", to},
                                             {"effective", effective},
                                             {"wage", wage},
                                             {"posting", posting},
                                             {"reason", reason},
                                             {"change", c}});
    };
    auto close_posting = [&](const std::string &id, const std::string &reason) {
        for (std::size_t i = 0; i < state.board.size(); ++i)
            if (state.board[i].id == id) {
                auto p = state.board[i];
                state.board.erase(state.board.begin() + static_cast<std::ptrdiff_t>(i));
                return emit("posting_closed", {{"firm", p.firm}, {"posting", p.id}, {"reason", reason}});
            }
        throw Error(5, "unknown posting " + id);
    };
    auto adjust_posting = [&](Posting &p, const std::string &field, Amount value, const std::string &reason) {
        auto old = field == "wage" ? p.wage : p.openings;
        (field == "wage" ? p.wage : p.openings) = value;
        if (field == "wage")
            p.unfilled_days = 0;
        return emit("posting_adjusted", {{"firm", p.firm},
                                         {"posting", p.id},
                                         {"field", field},
                                         {"old", old},
                                         {"new", value},
                                         {"reason", reason}});
    };
    for (auto &f : state.firms) {
        std::vector<std::string> workers, retained;
        std::vector<EmploymentChange> layoffs;
        Amount leaving = 0, arriving = 0;
        for (auto &h : state.households)
            if (h.employer == f.id) {
                workers.push_back(h.id);
                if (!pending_for(h.id))
                    retained.push_back(h.id);
            }
        for (auto &c : state.pending_changes) {
            if (c.from_employer == f.id) {
                ++leaving;
                if (c.to_employer.empty())
                    layoffs.push_back(c);
            }
            if (c.to_employer == f.id)
                ++arriving;
        }
        auto staying = static_cast<Amount>(workers.size()) - leaving;
        auto plan = staffing_target(f, staying, leaving);
        auto target = integer(plan.at("target"));
        auto reason = plan.at("reason").get<std::string>();
        // Why staffing moves, named from the worker's side: cuts cite the binding factor.
        auto layoff_reason =
            "LAYOFF_" + (reason == "NO_DEMAND" || reason == "INVENTORY_SURPLUS" || reason == "PAYROLL_RUNWAY"
                             ? reason
                         : reason == "OVERHEAD_STAFFING" ? std::string("ABOVE_POSITIONS")
                         : reason == "INSUFFICIENT_HISTORY" || reason == "CAPACITY_LIMIT"
                             ? std::string("ABOVE_CAPACITY")
                             : std::string("DEMAND_BELOW_STAFFING"));
        auto hire_reason = "HIRE_" + reason;
        auto d = firm_evidence(f.id, "staffing_plan-1", plan.at("factors"));
        d["constraints"]["target"] = target;
        d["constraints"]["workers"] = workers;
        d["constraints"]["arriving"] = arriving;
        Json actions = Json::array();
        // Pending layoffs the new target no longer justifies are withdrawn, latest scheduled first.
        for (auto it = layoffs.rbegin(); it != layoffs.rend() && staying + arriving < target; ++it) {
            for (std::size_t i = 0; i < state.pending_changes.size(); ++i)
                if (state.pending_changes[i].id == it->id)
                    state.pending_changes.erase(state.pending_changes.begin() +
                                                static_cast<std::ptrdiff_t>(i));
            auto ev = emit("employment_change_cancelled", {{"household", it->household},
                                                           {"from_employer", it->from_employer},
                                                           {"to_employer", it->to_employer},
                                                           {"reason", "STAFFING_TARGET_RECOVERED"},
                                                           {"change", *it}});
            d["event_ids"].push_back(ev.at("id"));
            actions.push_back({{"action", "cancel_layoff"},
                               {"household", it->household},
                               {"reason", "STAFFING_TARGET_RECOVERED"}});
            retained.push_back(it->household);
            ++staying;
            --leaving;
        }
        if (staying + arriving > target) {
            // Newest spells end first: order by start day, then record sequence, descending.
            std::vector<std::pair<std::pair<Day, std::uint64_t>, std::string>> order;
            for (auto &r : state.employment)
                if (r.employer == f.id && r.end == -1 &&
                    std::find(retained.begin(), retained.end(), r.household) != retained.end())
                    order.push_back({{r.start, std::stoull(r.id.substr(11))}, r.household});
            std::sort(order.rbegin(), order.rend());
            auto excess = std::min(staying + arriving - target, static_cast<Amount>(order.size()));
            for (Amount n = 0; n < excess; ++n) {
                auto who = order[static_cast<std::size_t>(n)].second;
                auto ev = schedule(who, f.id, "", add(today, notice), 0, "", layoff_reason);
                d["event_ids"].push_back(ev.at("id"));
                actions.push_back({{"action", "lay_off"},
                                   {"household", who},
                                   {"effective", add(today, notice)},
                                   {"reason", layoff_reason}});
                --staying;
                ++leaving;
            }
        }
        // Keep the vacancy board in line with the gap between the target and who is staying/arriving.
        auto *posting = posting_of(f.id);
        auto open = posting ? posting->openings : 0;
        auto desired_openings = std::max(Amount{0}, target - (staying + arriving));
        auto gap = desired_openings - open;
        if (gap > 0) {
            if (posting) {
                auto ev = adjust_posting(*posting, "openings", add(open, gap), "STAFFING_TARGET_RAISED");
                d["event_ids"].push_back(ev.at("id"));
                actions.push_back(
                    {{"action", "add_openings"}, {"openings", gap}, {"reason", "STAFFING_TARGET_RAISED"}});
            } else {
                if (state.next_posting >= static_cast<std::uint64_t>(std::numeric_limits<Amount>::max()))
                    throw Error(5, "posting ID exhausted");
                Posting p{"posting:" + std::to_string(state.next_posting++),
                          f.id,
                          gap,
                          std::clamp(f.wage, f.hiring.wage_min, f.hiring.wage_max),
                          f.hiring.min_experience_days,
                          f.hiring.accept_employed,
                          f.hiring.rehire_cooldown_days,
                          today,
                          0,
                          0,
                          hire_reason};
                state.board.push_back(p);
                posting = posting_of(f.id);
                auto ev = emit("posting_opened", {{"firm", f.id}, {"posting", p}, {"reason", hire_reason}});
                d["event_ids"].push_back(ev.at("id"));
                actions.push_back({{"action", "post_openings"},
                                   {"openings", gap},
                                   {"wage", p.wage},
                                   {"reason", hire_reason}});
            }
        } else if (gap < 0 && posting) {
            auto remaining = std::max(Amount{0}, open + gap);
            Json ev;
            if (remaining == 0) {
                auto id = posting->id;
                posting = nullptr;
                ev = close_posting(id, "STAFFING_TARGET_LOWERED");
            } else
                ev = adjust_posting(*posting, "openings", remaining, "STAFFING_TARGET_LOWERED");
            d["event_ids"].push_back(ev.at("id"));
            actions.push_back({{"action", remaining ? "remove_openings" : "withdraw_posting"},
                               {"openings", std::min(open, -gap)},
                               {"reason", "STAFFING_TARGET_LOWERED"}});
        }
        // Offer management: unfilled postings bid up within bounds; tight payroll bids down or withdraws.
        if (posting) {
            const auto &a = account(state, f.id);
            auto available = a.cash - a.reserved_cash;
            auto payroll_now = daily_payroll(state, f.id);
            const auto budget = available / runway;
            auto can_finance = [&](Money offer) {
                return payroll_now <= budget &&
                       (offer == 0 || posting->openings <= (budget - payroll_now) / offer);
            };
            auto step = f.hiring.wage_step;
            if (!can_finance(posting->wage)) {
                if (step > 0 && posting->wage > f.hiring.wage_min) {
                    auto offer = posting->wage - std::min(step, posting->wage - f.hiring.wage_min);
                    auto ev = adjust_posting(*posting, "wage", offer, "PAYROLL_LOW");
                    d["event_ids"].push_back(ev.at("id"));
                    actions.push_back(
                        {{"action", "lower_offer"}, {"wage", posting->wage}, {"reason", "PAYROLL_LOW"}});
                }
                // Lowering once may still leave an unaffordable promise. Only advertise the openings
                // the remaining budget can finance at the actual offer; otherwise withdraw it.
                auto funded = payroll_now > budget ? 0
                              : posting->wage == 0 ? posting->openings
                                                   : (budget - payroll_now) / posting->wage;
                if (funded > 0 && funded < posting->openings) {
                    auto ev = adjust_posting(*posting, "openings", funded, "PAYROLL_LOW");
                    d["event_ids"].push_back(ev.at("id"));
                    actions.push_back({{"action", "remove_openings"}, {"reason", "PAYROLL_LOW"}});
                } else if (funded == 0) {
                    auto id = posting->id;
                    posting = nullptr;
                    auto ev = close_posting(id, "PAYROLL_LOW");
                    d["event_ids"].push_back(ev.at("id"));
                    actions.push_back({{"action", "withdraw_posting"}, {"reason", "PAYROLL_LOW"}});
                }
            } else if (posting->unfilled_days >= f.hiring.raise_after_unfilled_days) {
                const auto offer = posting->wage + std::min(step, f.hiring.wage_max - posting->wage);
                if (offer > posting->wage && can_finance(offer)) {
                    auto ev = adjust_posting(*posting, "wage", offer, "UNFILLED");
                    d["event_ids"].push_back(ev.at("id"));
                    actions.push_back(
                        {{"action", "raise_offer"}, {"wage", posting->wage}, {"reason", "UNFILLED"}});
                } else
                    d["exclusions"].push_back(
                        {{"action", "raise_offer"},
                         {"reason", offer > posting->wage ? "PAYROLL_LOW" : "OFFER_AT_MAXIMUM"}});
            }
        }
        d["alternatives"] = actions;
        d["chosen"] = actions.empty() ? Json(nullptr) : Json("adjust_staffing");
        d["reason"] = reason;
        d["outcome"] = actions.empty() ? "HOLD" : "ADJUSTED";
        record_decision(d);
    }
    // Households read the board. The unemployed always look; the employed look when their
    // preferences allow it, or at once when they have gone unpaid beyond their tolerance. A posting
    // must first be eligible (the firm's rules), then preferable (the household's rules): net wage
    // after commute at or above the reservation wage and, for a paid worker, enough of a gain after
    // enough tenure. Hires, switches and quits all take effect tomorrow.
    std::set<std::string> applied;
    Json search_totals = {{"searching", 0},
                          {"applied", 0},
                          {"hires", 0},
                          {"switches", 0},
                          {"quits", 0},
                          {"declined", 0},
                          {"ineligible", 0},
                          {"no_postings", 0},
                          {"declined_reasons", Json::object()},
                          {"ineligible_reasons", Json::object()}};
    auto tally = [&](const std::string &key, Amount n = 1) {
        search_totals[key] = add(search_totals.value(key, Amount{0}), n);
    };
    const auto count = state.households.size();
    auto rotation = static_cast<std::size_t>(today) % count;
    for (std::size_t n = 0; n < count; ++n) {
        auto &h = state.households[(n + rotation) % count];
        if (pending_for(h.id))
            continue;
        const auto &jp = h.job_preferences;
        const bool unemployed = h.employer.empty();
        const bool unpaid = !unemployed && h.unpaid_days >= jp.unpaid_tolerance_days;
        if (!unemployed && !unpaid && (!jp.search_while_employed || state.board.empty()))
            continue;
        auto commute = [&](const std::string &firm_id) {
            return mul(firm(state, firm_id).travel_minutes, h.time_value);
        };
        const auto experience = experience_days(state, h.id, today), tenure = tenure_days(state, h.id, today);
        const Money current_wage = unemployed ? 0 : wage_of(state, h.id);
        // A job that does not pay is worth nothing to compare against.
        const Money current_net = unemployed || unpaid ? 0 : current_wage - commute(h.employer);
        tally("searching");
        Json d = {{"kind", "household"},
                  {"actor", h.id},
                  {"policy", "job_search-2"},
                  {"product", ""},
                  {"constraints",
                   {{"status", unemployed ? "unemployed"
                               : unpaid   ? "employed_unpaid"
                                          : "employed"},
                    {"employer", h.employer},
                    {"current_wage", current_wage},
                    {"current_net_wage", current_net},
                    {"unpaid_days", h.unpaid_days},
                    {"tenure_days", tenure},
                    {"experience_days", experience},
                    {"preferences", jp}}},
                  {"knowledge", Json::array()},
                  {"alternatives", Json::array()},
                  {"exclusions", Json::array()},
                  {"chosen", nullptr},
                  {"event_ids", Json::array()},
                  {"order_ids", Json::array()}};
        Posting *best = nullptr;
        Money best_net = 0;
        Amount declined = 0, ineligible = 0;
        for (auto &p : state.board) {
            auto minutes = firm(state, p.firm).travel_minutes;
            auto net = p.wage - commute(p.firm);
            d["knowledge"].push_back({{"posting", p.id},
                                      {"firm", p.firm},
                                      {"wage", p.wage},
                                      {"commute_minutes", minutes},
                                      {"net_wage", net},
                                      {"openings", p.openings},
                                      {"min_experience_days", p.min_experience_days},
                                      {"accept_employed", p.accept_employed},
                                      {"rehire_cooldown_days", p.rehire_cooldown_days},
                                      {"day", today},
                                      {"source", "vacancy_board"}});
            std::string exclusion, stage = "eligibility";
            if (p.firm == h.employer)
                exclusion = "CURRENT_EMPLOYER";
            else if (p.openings <= 0)
                exclusion = "NO_OPENINGS";
            else if (experience < p.min_experience_days)
                exclusion = "NOT_ELIGIBLE_EXPERIENCE";
            else if (!unemployed && !p.accept_employed)
                exclusion = "NOT_ELIGIBLE_EMPLOYED";
            else
                for (auto &r : state.employment)
                    if (r.household == h.id && r.employer == p.firm && r.end != -1 &&
                        r.end_reason.rfind("LAYOFF_", 0) == 0 && today - r.end < p.rehire_cooldown_days)
                        exclusion = "REHIRE_COOLDOWN";
            if (exclusion.empty()) {
                stage = "preference";
                if (net < jp.reservation_wage)
                    exclusion = "BELOW_RESERVATION_WAGE";
                else if (!unemployed && !unpaid) {
                    if (tenure < jp.min_tenure_days)
                        exclusion = "TENURE_TOO_SHORT";
                    else if (add(net, -current_net) < jp.min_wage_gain ||
                             mul(std::max(net, Amount{0}), 100) <
                                 mul(std::max(current_net, Amount{0}), add(100, jp.min_wage_gain_percent)))
                        exclusion = "INSUFFICIENT_WAGE_GAIN";
                }
            }
            if (!exclusion.empty()) {
                d["exclusions"].push_back({{"posting", p.id},
                                           {"firm", p.firm},
                                           {"stage", stage},
                                           {"reason", exclusion},
                                           {"net_wage", net}});
                (stage == "preference" ? declined : ineligible) += 1;
                increment(search_totals[stage == "preference" ? "declined_reasons" : "ineligible_reasons"],
                          exclusion, 1);
                continue;
            }
            d["alternatives"].push_back({{"posting", p.id},
                                         {"firm", p.firm},
                                         {"wage", p.wage},
                                         {"net_wage", net},
                                         {"gain", add(net, -current_net)},
                                         {"openings", p.openings}});
            if (!best || net > best_net) {
                best = &p;
                best_net = net;
            }
        }
        Json search = {{"chosen", nullptr}, {"declined", declined}, {"ineligible", ineligible}};
        if (!best) {
            d["reason"] = d["knowledge"].empty() ? "NO_POSTINGS"
                          : declined             ? "NO_PREFERABLE_OFFER"
                                                 : "NO_ELIGIBLE_OFFER";
            d["outcome"] = d["knowledge"].empty() ? "NO_POSTINGS" : declined ? "DECLINED" : "INELIGIBLE";
            tally(d["knowledge"].empty() ? "no_postings" : declined ? "declined" : "ineligible");
        } else {
            --best->openings;
            best->unfilled_days = 0;
            best->applications = add(best->applications, 1);
            applied.insert(best->id);
            auto kind = unemployed ? "HIRED" : unpaid ? "QUIT_UNPAID" : "SWITCHED";
            auto ev = schedule(h.id, h.employer, best->firm, add(today, 1), best->wage, best->id, kind);
            d["event_ids"].push_back(ev.at("id"));
            d["chosen"] = best->id;
            d["reason"] = "HIGHEST_NET_WAGE";
            d["outcome"] = unemployed ? "HIRE_SCHEDULED" : unpaid ? "QUIT_SCHEDULED" : "SWITCH_SCHEDULED";
            search["chosen"] = best->id;
            tally("applied");
            tally(unemployed ? "hires" : unpaid ? "quits" : "switches");
        }
        search["outcome"] = d["outcome"];
        search["reason"] = d["reason"];
        if (!daily.is_null())
            daily["labor"][h.id]["search"] = search;
        record_decision(d);
    }
    if (!daily.is_null())
        daily["job_search"] = search_totals;
    // Postings that received nobody today age; filled ones leave the board.
    std::vector<std::string> filled;
    for (auto &p : state.board) {
        if (p.openings == 0)
            filled.push_back(p.id);
        else if (!applied.contains(p.id))
            p.unfilled_days = add(p.unfilled_days, 1);
    }
    for (auto &id : filled)
        close_posting(id, "FILLED");
}
void Engine::work() {
    struct Production {
        std::string firm;
        Amount quantity;
        std::size_t workers;
        Json evidence;
    };
    std::vector<Production> tasks;
    for (auto &f : state.firms) {
        std::vector<std::string> workers;
        std::map<std::string, Money> wages;
        Money payroll = 0;
        for (auto &h : state.households)
            if (h.employer == f.id) {
                workers.push_back(h.id);
                wages[h.id] = wage_of(state, h.id);
                payroll = add(payroll, wages[h.id]);
            }
        auto &a = account(state, f.id);
        auto d = firm_evidence(f.id, "financed_payroll-1",
                               {{"available_cash", a.cash - a.reserved_cash},
                                {"payroll", payroll},
                                {"workers", workers},
                                {"wage", f.wage},
                                {"wages", wages},
                                {"labor_required", f.labor_required}});
        bool eligible = static_cast<Amount>(workers.size()) >= f.labor_required;
        if (!eligible || a.cash - a.reserved_cash < payroll) {
            // Holding a job is not the same as working: these workers stay employed but idle.
            for (auto &h : state.households)
                if (h.employer == f.id)
                    h.unpaid_days = add(h.unpaid_days, 1);
            if (!workers.empty())
                f.payroll_missed_days = add(f.payroll_missed_days, 1);
            else
                f.payroll_missed_days = 0;
            auto reason = eligible ? "INSUFFICIENT_PAYROLL" : "INSUFFICIENT_LABOR";
            auto ev = emit("work_inactive", {{"firm", f.id}, {"reason", reason}});
            d["exclusions"].push_back({{"action", "pay_all_workers"}, {"reason", reason}});
            d["reason"] = reason;
            d["outcome"] = "INACTIVE";
            d["event_ids"].push_back(ev.at("id"));
            record_decision(d);
            continue;
        }
        // Output scales with the paid crew up to capacity; the default output_per_worker makes the
        // minimum crew reach capacity, which is the original fixed-staffing behaviour.
        Amount quantity = f.kind == "producer" ? mul_capped(f.output_per_worker,
                                                            static_cast<Amount>(workers.size()), f.capacity)
                                               : 0;
        for (auto &[p, input] : f.inputs)
            if (input > 0)
                quantity = std::min(quantity, (stock(a.stock, p) - stock(a.reserved_stock, p)) / input);
        auto production = firm_evidence(f.id, "paid_capacity_recipe-1",
                                        {{"capacity", f.capacity},
                                         {"output_per_worker", f.output_per_worker},
                                         {"inputs_per_unit", f.inputs},
                                         {"opening_stock", a.stock},
                                         {"reserved_stock", a.reserved_stock},
                                         {"paid_workers", workers.size()},
                                         {"labor_required", f.labor_required}},
                                        f.output);
        for (auto &w : workers)
            add(account(state, w).cash, wages.at(w));
        if (quantity) {
            add(stock(a.stock, f.output), quantity);
            for (auto &[p, input] : f.inputs)
                mul(input, quantity);
        }
        // Payroll is the transaction that turns cash into labor: each paid worker delivers one
        // worker-day to the firm, which production then consumes.
        for (auto &w : workers) {
            auto ev = emit(
                "wage", {{"employer", f.id}, {"household", w}, {"amount", wages.at(w)}, {"labor_units", 1}},
                {effect(f.id, "cash", -wages.at(w)), effect(w, "cash", wages.at(w)),
                 effect(f.id, "labor", 1)});
            d["event_ids"].push_back(ev.at("id"));
        }
        for (auto &h : state.households)
            if (h.employer == f.id)
                h.unpaid_days = 0;
        f.payroll_missed_days = 0;
        d["alternatives"].push_back({{"action", "pay_all_workers"}, {"amount", payroll}});
        d["chosen"] = "pay_all_workers";
        d["reason"] = "PAYROLL_FINANCED";
        d["outcome"] = "PAID";
        record_decision(d);
        if (f.kind == "producer")
            tasks.push_back({f.id, quantity, workers.size(), production});
    }
    for (auto &task : tasks) {
        auto &f = firm(state, task.firm);
        auto q = task.quantity;
        auto d = task.evidence;
        if (q) {
            // Output consumes the worker-days it needs; the rest of the crew is idle today.
            auto used = ceil_div(q, f.output_per_worker);
            Json effects = Json::array();
            for (auto &[p, input] : f.inputs)
                effects.push_back(effect(f.id, "stock", -mul(input, q), p));
            effects.push_back(effect(f.id, "stock", q, f.output));
            effects.push_back(effect(f.id, "labor", -used));
            auto ev = emit("production",
                           {{"firm", f.id},
                            {"product", f.output},
                            {"quantity", q},
                            {"inputs_per_unit", f.inputs},
                            {"capacity", f.capacity},
                            {"output_per_worker", f.output_per_worker},
                            {"paid_workers", task.workers},
                            {"labor_used", used},
                            {"labor_idle", static_cast<Amount>(task.workers) - used}},
                           effects);
            d["alternatives"].push_back({{"action", "produce"}, {"quantity", q}});
            d["chosen"] = "produce";
            d["outcome"] = "PRODUCED";
            d["event_ids"].push_back(ev.at("id"));
            d["reason"] = "MAX_FEASIBLE_OUTPUT";
        } else {
            d["outcome"] = "NO_OUTPUT";
            d["reason"] = f.capacity ? "INPUT_LIMIT" : "ZERO_CAPACITY";
        }
        if (q < f.capacity)
            d["exclusions"].push_back(
                {{"action", "produce_capacity"}, {"quantity", f.capacity}, {"reason", "INPUT_LIMIT"}});
        record_decision(d);
    }
    // Labor is perishable: worker-days not consumed by production are released today, as idle
    // crew at a producer or as the day's service at a retailer.
    for (auto &f : state.firms) {
        auto left = account(state, f.id).labor;
        if (left > 0)
            emit("labor_released",
                 {{"firm", f.id}, {"units", left}, {"use", f.kind == "producer" ? "idle" : "retail_service"}},
                 {effect(f.id, "labor", -left)});
    }
}
void Engine::procure() {
    for (auto &f : state.firms)
        for (auto &[p, target] : f.targets) {
            auto &a = account(state, f.id);
            Amount incoming = 0;
            for (auto &o : state.orders)
                if (o.buyer == f.id && o.product == p)
                    incoming = add(incoming, o.quantity);
            auto have = add(stock(a.stock, p), incoming);
            auto d = firm_evidence(f.id, "inventory_target-1",
                                   {{"target", target},
                                    {"on_hand", stock(a.stock, p)},
                                    {"incoming", incoming},
                                    {"available_cash", a.cash - a.reserved_cash},
                                    {"lead_days", f.lead_days}},
                                   p);
            if (have >= target) {
                d["reason"] = "TARGET_SATISFIED";
                d["outcome"] = "NO_ORDER";
                record_decision(d);
                continue;
            }
            auto seller = f.suppliers.at(p);
            auto price = firm(state, seller).prices.at(p);
            auto &b = account(state, seller);
            auto available = stock(b.stock, p) - stock(b.reserved_stock, p);
            d["knowledge"].push_back({{"supplier", seller},
                                      {"product", p},
                                      {"price", price},
                                      {"available_stock", available},
                                      {"day", state.next_day},
                                      {"source", "supplier_quote"}});
            auto q = std::min({target - have, (a.cash - a.reserved_cash) / price, available});
            if (q > 0) {
                auto order_id = state.next_order;
                auto ev = trade(f.id, seller, p, q, price, "wholesale", f.lead_days);
                d["event_ids"].push_back(ev);
                if (q == available && available < target - have) {
                    // The supplier, not the buyer's cash, cut the order short: a stock-out it observes.
                    auto shortfall = emit("supply_shortfall", {{"firm", f.id},
                                                               {"supplier", seller},
                                                               {"product", p},
                                                               {"desired", target - have},
                                                               {"supplied", q},
                                                               {"shortfall", target - have - q},
                                                               {"reason", "OUT_OF_STOCK"}});
                    d["event_ids"].push_back(shortfall.at("id"));
                }
                if (f.lead_days)
                    d["order_ids"].push_back("order:" + std::to_string(order_id));
                d["alternatives"].push_back({{"action", "order"}, {"quantity", q}, {"price", price}});
                d["chosen"] = "order";
                d["reason"] = "SELECTED_FEASIBLE_RESTOCK";
                d["outcome"] = f.lead_days ? "RESERVED" : "SETTLED";
                if (q < target - have)
                    d["exclusions"].push_back({{"action", "order_full_gap"},
                                               {"quantity", target - have},
                                               {"reason", "CASH_OR_STOCK_LIMIT"}});
            } else {
                auto reason = a.cash - a.reserved_cash < price ? "INSUFFICIENT_CASH" : "OUT_OF_STOCK";
                auto ev = emit("procurement_blocked", {{"firm", f.id},
                                                       {"supplier", seller},
                                                       {"product", p},
                                                       {"desired", target - have},
                                                       {"reason", reason}});
                d["event_ids"].push_back(ev.at("id"));
                d["exclusions"].push_back({{"action", "order"}, {"reason", reason}});
                d["reason"] = reason;
                d["outcome"] = "NO_ORDER";
            }
            record_decision(d);
        }
}
void Engine::observe(Household &h, const std::string &shop, const std::string &source) {
    auto &f = firm(state, shop);
    if (f.kind != "retailer")
        throw Error(2, "observation requires retailer");
    if (std::find(h.known_shops.begin(), h.known_shops.end(), shop) == h.known_shops.end())
        h.known_shops.push_back(shop);
    for (auto &[p, price] : f.prices) {
        Observation o{shop,
                      p,
                      price,
                      stock(account(state, shop).stock, p) - stock(account(state, shop).reserved_stock, p),
                      state.next_day,
                      source};
        auto it = std::find_if(h.knowledge.begin(), h.knowledge.end(),
                               [&](auto &k) { return k.shop == shop && k.product == p; });
        if (it == h.knowledge.end())
            h.knowledge.push_back(o);
        else
            *it = o;
    }
}
void Engine::publish() {
    for (std::size_t i = 0; i < state.households.size(); ++i) {
        auto &h = state.households[i];
        if (state.public_board) {
            for (auto &f : state.firms)
                if (f.kind == "retailer")
                    observe(h, f.id, "public_board");
            continue;
        }
        // Notices carry no travel/search cost: they model local posted information.
        // Only actual checkout trips consume the household shopping time budget.
        if (state.next_day % integer(scenario.at("refresh_days")) == 0) {
            auto known = h.known_shops;
            for (auto &k : known)
                observe(h, k, "local_notice");
        }
        if (state.next_day > 0 && state.next_day % integer(scenario.at("explore_days")) == 0) {
            std::vector<std::string> shops;
            for (auto &f : state.firms)
                if (f.kind == "retailer")
                    shops.push_back(f.id);
            if (!shops.empty()) {
                auto k = keyed(state.seed, static_cast<std::uint64_t>(state.next_day), i, 1) % shops.size();
                observe(h, shops[k], "directory_notice");
            }
        }
    }
}
void Engine::shop() {
    const auto count = state.households.size();
    auto rotation = static_cast<std::size_t>(state.next_day) % count;
    for (std::size_t n = 0; n < count; ++n) {
        auto &h = state.households[(n + rotation) % count];
        auto budget = h.budget, time = h.time_minutes;
        for (auto &[p, need] : h.needs) {
            auto &a = account(state, h.id);
            auto q = std::max(Amount{0}, need - stock(a.stock, p));
            if (!q)
                continue;
            ShoppingView v{h.id,   p,    state.next_day, q,           a.cash - a.reserved_cash,
                           budget, time, h.time_value,   h.knowledge, {},
                           {}};
            for (auto &f : state.firms)
                if (f.kind == "retailer" && f.prices.contains(p)) {
                    v.shop_ids.push_back(f.id);
                    v.travel[f.id] = f.travel_minutes;
                }
            if (state.next_decision >= static_cast<std::uint64_t>(std::numeric_limits<Amount>::max()))
                throw Error(5, "decision ID exhausted");
            auto d = choose(v);
            d["event_ids"] = Json::array();
            d["order_ids"] = Json::array();
            d["outcome"] = "NO_PURCHASE";
            if (!d["chosen"].is_null()) {
                auto shop = d["chosen"].get<std::string>();
                auto perceived = std::find_if(v.observations.begin(), v.observations.end(), [&](auto &o) {
                                     return o.shop == shop && o.product == p;
                                 })->price;
                auto actual = firm(state, shop).prices.at(p);
                observe(h, shop, "checkout");
                time -= v.travel.at(shop);
                d["checkout"] = {
                    {"actual_price", actual},
                    {"actual_available_stock",
                     stock(account(state, shop).stock, p) - stock(account(state, shop).reserved_stock, p)},
                    {"stale_price", perceived != actual}};
                if (perceived != actual)
                    d["checkout"]["reason"] = "STALE_PRICE";
                auto cost = mul(actual, q);
                if (cost > budget) {
                    auto rejected = emit("trade_rejected", {{"buyer", h.id},
                                                            {"seller", shop},
                                                            {"product", p},
                                                            {"quantity", q},
                                                            {"price", actual},
                                                            {"stage", "retail"},
                                                            {"reason", "BUDGET_LIMIT"},
                                                            {"remaining_budget", budget}});
                    d["event_ids"].push_back(rejected.at("id"));
                    d["outcome"] = "BUDGET_LIMIT";
                } else {
                    auto event_id = "event:" + std::to_string(state.next_event);
                    auto result = trade(h.id, shop, p, q, actual, "retail");
                    d["event_ids"].push_back(event_id);
                    if (!result.empty()) {
                        budget -= cost;
                        d["outcome"] = "PURCHASED";
                    } else
                        d["outcome"] =
                            (a.cash - a.reserved_cash < cost ? "INSUFFICIENT_CASH" : "OUT_OF_STOCK");
                }
            }
            record_decision(d);
        }
    }
}
void Engine::consume() {
    for (auto &h : state.households)
        for (auto &[p, need] : h.needs) {
            auto q = std::min(need, stock(account(state, h.id).stock, p));
            if (q)
                emit("consumption", {{"household", h.id}, {"product", p}, {"quantity", q}},
                     {effect(h.id, "stock", -q, p)});
            if (q < need)
                emit("unmet", {{"household", h.id}, {"product", p}, {"quantity", need - q}});
        }
}
void Engine::prices() {
    for (auto &f : state.firms)
        for (auto &[p, price] : f.prices) {
            auto old = price;
            auto q = stock(account(state, f.id).stock, p);
            auto target = stock(f.targets, p);
            auto d = firm_evidence(f.id, f.price_policy + "-1",
                                   {{"old_price", old},
                                    {"closing_stock", q},
                                    {"target", target},
                                    {"minimum", f.price_min},
                                    {"maximum", f.price_max},
                                    {"step", f.price_step}},
                                   p);
            if (f.price_policy == "inventory_band") {
                if (q < target / 2)
                    price = add(price, std::min(f.price_step, f.price_max - price));
                else if (q > target)
                    price = std::max(f.price_min, price - std::min(price, f.price_step));
            }
            if (price != old) {
                auto ev = emit("price_changed", {{"firm", f.id},
                                                 {"product", p},
                                                 {"old_price", old},
                                                 {"new_price", price},
                                                 {"policy", "inventory_band-1"}});
                d["event_ids"].push_back(ev.at("id"));
            }
            d["alternatives"].push_back({{"action", "post_price"}, {"price", price}});
            d["chosen"] = "post_price";
            d["reason"] = f.price_policy == "fixed"
                              ? "FIXED_PRICE"
                              : (price > old ? "LOW_INVENTORY"
                                             : (price < old ? "HIGH_INVENTORY" : "HOLD_BAND_OR_BOUND"));
            d["outcome"] = price == old ? "UNCHANGED" : "PRICE_CHANGED";
            record_decision(d);
        }
}
void Engine::intervene(const Json &intervention) {
    if (!intervention.is_object() || intervention.size() != 3 ||
        intervention.at("schema") != "intervention-1" || integer(intervention.at("day")) != state.next_day)
        throw Error(2, "invalid intervention schema/boundary");
    auto type = intervention.at("type").get<std::string>();
    if (type == "noop")
        return;
    if (type != "public_price_board")
        throw Error(2, "unknown intervention type");
    if (state.public_board)
        throw Error(2, "public board already enabled");
    state.public_board = true;
    phase = "intervention";
    emit("intervention", {{"type", type}});
}
Json Engine::step() {
    daily = {{"schema", "metric-1"},
             {"day", state.next_day},
             {"paid_workers", 0},
             {"wages", 0},
             {"labor_units", {{"delivered", 0}, {"used", 0}, {"idle", 0}, {"retail_service", 0}}},
             {"labor_flow", Json::object()},
             {"hires", 0},
             {"separations", 0},
             {"labor", Json::object()},
             {"inactive_firms", Json::object()},
             {"board_activity", Json::array()},
             {"job_search",
              {{"searching", 0},
               {"applied", 0},
               {"hires", 0},
               {"switches", 0},
               {"quits", 0},
               {"declined", 0},
               {"ineligible", 0},
               {"no_postings", 0},
               {"declined_reasons", Json::object()},
               {"ineligible_reasons", Json::object()}}},
             {"production", Json::object()},
             {"consumption", Json::object()},
             {"unmet", Json::object()},
             {"sales_cents", {{"retail", 0}, {"wholesale", 0}}},
             {"sales_units", {{"retail", Json::object()}, {"wholesale", Json::object()}}},
             {"realized_prices", Json::array()},
             {"failed_attempts", 0},
             {"stockouts", 0},
             {"firm_net_cash_flow", Json::object()}};
    sold_today.clear();
    shortfall_today.clear();
    phase = "arrivals";
    arrivals();
    phase = "employment";
    employment();
    phase = "work";
    work();
    phase = "procurement";
    procure();
    phase = "information";
    publish();
    phase = "shopping";
    shop();
    phase = "consumption";
    consume();
    phase = "pricing";
    prices();
    // Each firm remembers what it observed of demand today, over a rolling window.
    auto window = static_cast<std::size_t>(integer(scenario.at("employment").at("demand_window_days")));
    for (auto &f : state.firms) {
        auto observed = add(sold_today.contains(f.id) ? sold_today.at(f.id) : 0,
                            shortfall_today.contains(f.id) ? shortfall_today.at(f.id) : 0);
        f.demand_history.push_back(observed);
        if (f.demand_history.size() > window)
            f.demand_history.erase(f.demand_history.begin());
    }
    state.next_day = add(state.next_day, 1);
    validate_state(state, scenario);
    daily["household_cash"] = Json::object();
    daily["firm_cash"] = Json::object();
    daily["inventories"] = Json::object();
    daily["employment"] = Json::object();
    for (auto &a : state.accounts) {
        daily["inventories"][a.id] = a.stock;
        if (a.id.rfind("household:", 0) == 0)
            daily["household_cash"][a.id] = a.cash;
        else
            daily["firm_cash"][a.id] = a.cash;
    }
    // Per-person daily labor record: job held, whether work happened, payroll received,
    // and any employment change scheduled or taking effect today.
    Amount employed = 0;
    for (auto &h : state.households) {
        daily["employment"][h.id] = h.employer;
        auto &person = daily["labor"][h.id];
        for (auto key : {"change", "scheduled", "cancelled", "search"})
            if (!person.contains(key))
                person[key] = nullptr;
        if (!person.contains("worked"))
            person["worked"] = false;
        if (!person.contains("wage_paid"))
            person["wage_paid"] = 0;
        person["employer"] = h.employer.empty() ? Json(nullptr) : Json(h.employer);
        person["status"] = h.employer.empty() ? "unemployed" : "employed";
        person["idle_reason"] = nullptr;
        if (!h.employer.empty()) {
            ++employed;
            if (!person.at("worked").get<bool>())
                person["idle_reason"] = daily["inactive_firms"].value(h.employer, Json(nullptr));
        }
        person["unpaid_days"] = h.unpaid_days;
    }
    daily["employed"] = employed;
    daily["unemployed"] = static_cast<Amount>(state.households.size()) - employed;
    daily["pending_changes"] = state.pending_changes.size();
    daily["board"] = state.board;
    daily["demand"] = Json::object();
    for (auto &f : state.firms)
        daily["demand"][f.id] = f.demand_history.empty() ? 0 : f.demand_history.back();
    auto result = daily;
    daily = nullptr;
    return result;
}
} // namespace econ
