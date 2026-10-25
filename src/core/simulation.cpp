#include <algorithm>
#include <econ/econ.hpp>
#include <limits>
namespace econ {
Engine::Engine(State s, Json config, Sink ev, Sink dec, std::string level)
    : state(std::move(s)), scenario(std::move(config)), events(std::move(ev)), decisions(std::move(dec)),
      trace(std::move(level)) {}
static void increment(Json &j, const std::string &key, Amount n) {
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
        else
            throw Error(6, "unknown effect field");
    }
    for (const auto &[id, a] : staged) {
        (void)id;
        if (a.cash < 0 || a.reserved_cash < 0 || a.reserved_cash > a.cash)
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
        for (const auto &e : effects)
            if (e["field"] == "cash" && e.at("actor").get<std::string>().rfind("firm:", 0) == 0)
                increment(daily["firm_net_cash_flow"], e.at("actor"), integer(e.at("delta")));
    }
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
        for (auto &h : state.households)
            if (h.employer == f.id)
                workers.push_back(h.id);
        auto payroll = mul(f.wage, static_cast<Amount>(workers.size()));
        auto &a = account(state, f.id);
        auto d = firm_evidence(f.id, "financed_payroll-1",
                               {{"available_cash", a.cash - a.reserved_cash},
                                {"payroll", payroll},
                                {"workers", workers},
                                {"wage", f.wage},
                                {"labor_required", f.labor_required}});
        bool eligible = static_cast<Amount>(workers.size()) >= f.labor_required;
        if (!eligible || a.cash - a.reserved_cash < payroll) {
            auto reason = eligible ? "INSUFFICIENT_PAYROLL" : "INSUFFICIENT_LABOR";
            auto ev = emit("work_inactive", {{"firm", f.id}, {"reason", reason}});
            d["exclusions"].push_back({{"action", "pay_all_workers"}, {"reason", reason}});
            d["reason"] = reason;
            d["outcome"] = "INACTIVE";
            d["event_ids"].push_back(ev.at("id"));
            record_decision(d);
            continue;
        }
        Amount quantity = f.kind == "producer" ? f.capacity : 0;
        for (auto &[p, input] : f.inputs)
            if (input > 0)
                quantity = std::min(quantity, (stock(a.stock, p) - stock(a.reserved_stock, p)) / input);
        auto production = firm_evidence(f.id, "paid_capacity_recipe-1",
                                        {{"capacity", f.capacity},
                                         {"inputs_per_unit", f.inputs},
                                         {"opening_stock", a.stock},
                                         {"reserved_stock", a.reserved_stock},
                                         {"paid_workers", workers.size()},
                                         {"labor_required", f.labor_required}},
                                        f.output);
        for (auto &w : workers)
            add(account(state, w).cash, f.wage);
        if (quantity) {
            add(stock(a.stock, f.output), quantity);
            for (auto &[p, input] : f.inputs)
                mul(input, quantity);
        }
        for (auto &w : workers) {
            auto ev = emit("wage", {{"employer", f.id}, {"household", w}, {"amount", f.wage}},
                           {effect(f.id, "cash", -f.wage), effect(w, "cash", f.wage)});
            d["event_ids"].push_back(ev.at("id"));
        }
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
            Json effects = Json::array();
            for (auto &[p, input] : f.inputs)
                effects.push_back(effect(f.id, "stock", -mul(input, q), p));
            effects.push_back(effect(f.id, "stock", q, f.output));
            auto ev = emit("production",
                           {{"firm", f.id},
                            {"product", f.output},
                            {"quantity", q},
                            {"inputs_per_unit", f.inputs},
                            {"capacity", f.capacity},
                            {"paid_workers", task.workers}},
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
                auto ev =
                    emit("procurement_blocked",
                         {{"firm", f.id}, {"product", p}, {"desired", target - have}, {"reason", reason}});
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
             {"production", Json::object()},
             {"consumption", Json::object()},
             {"unmet", Json::object()},
             {"sales_cents", {{"retail", 0}, {"wholesale", 0}}},
             {"sales_units", {{"retail", Json::object()}, {"wholesale", Json::object()}}},
             {"realized_prices", Json::array()},
             {"failed_attempts", 0},
             {"stockouts", 0},
             {"firm_net_cash_flow", Json::object()}};
    phase = "arrivals";
    arrivals();
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
    for (auto &h : state.households)
        daily["employment"][h.id] = h.employer;
    auto result = daily;
    daily = nullptr;
    return result;
}
} // namespace econ
