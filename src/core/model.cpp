#include <algorithm>
#include <charconv>
#include <econ/econ.hpp>
#include <iomanip>
#include <limits>
#include <regex>
#include <set>
#include <sstream>
namespace econ {
Amount add(Amount a, Amount b) {
    if ((b > 0 && a > std::numeric_limits<Amount>::max() - b) ||
        (b < 0 && a < std::numeric_limits<Amount>::min() - b))
        throw Error(5, "integer addition overflow");
    return a + b;
}
Amount mul(Amount a, Amount b) {
    if (a < 0 || b < 0)
        throw Error(2, "multiplication requires nonnegative operands");
    if (b && a > std::numeric_limits<Amount>::max() / b)
        throw Error(5, "integer multiplication overflow");
    return a * b;
}
Amount ceil_div(Amount a, Amount b) {
    if (a < 0 || b <= 0)
        throw Error(5, "ceiling division requires nonnegative numerator and positive denominator");
    return a / b + (a % b != 0);
}
Amount mul_capped(Amount a, Amount b, Amount limit) {
    if (a < 0 || b < 0 || limit < 0)
        throw Error(5, "capped multiplication requires nonnegative operands");
    return b && a > limit / b ? limit : a * b;
}
Amount integer(const Json &j) {
    if (!j.is_number_integer() ||
        (j.is_number_unsigned() &&
         j.get<std::uint64_t>() > static_cast<std::uint64_t>(std::numeric_limits<Amount>::max())))
        throw Error(2, "expected exact signed 64-bit integer");
    return j.get<Amount>();
}
std::string digest(const Json &j) {
    // FNV-1a-64 over canonical sorted-key compact UTF-8 JSON. Stable, non-cryptographic.
    std::uint64_t h = 14695981039346656037ULL;
    for (unsigned char c : j.dump()) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    std::ostringstream o;
    o << std::hex << std::setfill('0') << std::setw(16) << h;
    return o.str();
}
std::uint64_t keyed(std::uint64_t seed, std::uint64_t day, std::uint64_t actor, std::uint64_t purpose) {
    auto mix = [](std::uint64_t x) {
        x += 0x9e3779b97f4a7c15ULL;
        x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
        x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
        return x ^ (x >> 31);
    };
    return mix(seed ^ mix(day) ^ mix(actor + 0x1234) ^ mix(purpose + 0x5678));
}
Amount stock(const std::map<std::string, Amount> &m, const std::string &p) {
    auto it = m.find(p);
    return it == m.end() ? 0 : it->second;
}
Account &account(State &s, const std::string &id) {
    for (auto &a : s.accounts)
        if (a.id == id)
            return a;
    throw Error(2, "unknown account: " + id);
}
const Account &account(const State &s, const std::string &id) {
    for (const auto &a : s.accounts)
        if (a.id == id)
            return a;
    throw Error(2, "unknown account: " + id);
}
Firm &firm(State &s, const std::string &id) {
    for (auto &f : s.firms)
        if (f.id == id)
            return f;
    throw Error(2, "unknown firm: " + id);
}
Day experience_days(const State &s, const EntityId &household, Day today) {
    Day total = 0;
    for (auto &r : s.employment)
        if (r.household == household)
            total = add(total, std::max<Day>(0, (r.end == -1 ? today : r.end) - r.start));
    return total;
}
Day tenure_days(const State &s, const EntityId &household, Day today) {
    for (auto &r : s.employment)
        if (r.household == household && r.end == -1)
            return std::max<Day>(0, today - r.start);
    return 0;
}
Money daily_payroll(const State &s, const EntityId &firm_id) {
    Money total = 0;
    for (auto &r : s.employment)
        if (r.employer == firm_id && r.end == -1)
            total = add(total, r.wage);
    return total;
}
static void keys(const Json &j, const std::set<std::string> &allowed) {
    if (!j.is_object())
        throw Error(2, "expected object");
    for (auto it = j.begin(); it != j.end(); ++it)
        if (!allowed.contains(it.key()))
            throw Error(2, "unknown scenario field: " + it.key());
}
static void defaults(Json &target, const Json &d) {
    for (auto it = d.begin(); it != d.end(); ++it)
        if (!target.contains(it.key()))
            target[it.key()] = it.value();
}
static Amount nonnegative(const Json &j) {
    auto n = integer(j);
    if (n < 0)
        throw Error(2, "negative amount/capacity");
    return n;
}
bool valid_employment_mode(const std::string &mode) {
    return mode == employment_fixed || mode == employment_dynamic;
}
Json normalize(Json j) {
    keys(j, {"schema", "name", "products", "households", "firms", "refresh_days", "explore_days",
             "public_board", "employment"});
    if (j.at("schema") != "scenario-1")
        throw Error(2, "unsupported scenario schema");
    defaults(j, {{"name", "town"},
                 {"refresh_days", 3},
                 {"explore_days", 7},
                 {"public_board", false},
                 {"employment", Json::object()}});
    if (!j["public_board"].is_boolean() || !j["name"].is_string())
        throw Error(2, "invalid scenario metadata");
    if (nonnegative(j["refresh_days"]) == 0 || nonnegative(j["explore_days"]) == 0)
        throw Error(2, "observation intervals must be positive");
    // Dynamic-employment rule parameters. The mode itself is a run option, not scenario data,
    // so one scenario can be launched either way; these values are inert under fixed employment.
    auto &employment = j["employment"];
    keys(employment, {"notice_days", "demand_window_days", "inventory_cover_days", "payroll_runway_days",
                      "job_preferences"});
    defaults(employment, {{"notice_days", 1},
                          {"demand_window_days", 5},
                          {"inventory_cover_days", 3},
                          {"payroll_runway_days", 3},
                          {"job_preferences", Json::object()}});
    for (auto k : {"notice_days", "demand_window_days", "inventory_cover_days", "payroll_runway_days"})
        if (nonnegative(employment[k]) == 0)
            throw Error(2, "employment intervals must be positive");
    // Household job preferences: scenario-wide defaults, overridable per household.
    auto preferences = [&](Json &prefs, const Json &base) {
        keys(prefs, {"reservation_wage", "min_wage_gain", "min_wage_gain_percent", "min_tenure_days",
                     "search_while_employed", "unpaid_tolerance_days"});
        defaults(prefs, base);
        for (auto k : {"reservation_wage", "min_wage_gain", "min_wage_gain_percent", "min_tenure_days",
                       "unpaid_tolerance_days"})
            nonnegative(prefs[k]);
        if (!prefs["search_while_employed"].is_boolean())
            throw Error(2, "search_while_employed must be boolean");
        if (integer(prefs["unpaid_tolerance_days"]) == 0)
            throw Error(2, "unpaid_tolerance_days must be positive");
    };
    preferences(employment["job_preferences"], Json(JobPreferences{}));
    std::set<std::string> products, ids;
    if (!j.at("products").is_array() || j["products"].empty())
        throw Error(2, "products required");
    for (auto &p : j["products"]) {
        auto id = p.get<std::string>();
        if (!std::regex_match(id, std::regex("[a-z][a-z0-9_]*")) || !products.insert(id).second)
            throw Error(2, "invalid/duplicate product");
    }
    auto quantities = [&](const Json &values) {
        if (!values.is_object())
            throw Error(2, "product map required");
        for (auto it = values.begin(); it != values.end(); ++it) {
            if (!products.contains(it.key()))
                throw Error(2, "unknown product " + it.key());
            nonnegative(it.value());
        }
    };
    if (!j.at("firms").is_array() || j["firms"].empty() || !j.at("households").is_array() ||
        j["households"].empty())
        throw Error(2, "firms and households required");
    for (auto &f : j["firms"]) {
        keys(f, {"id",        "kind",           "cash",       "stock",          "wage",
                 "capacity",  "labor_required", "output",     "inputs",         "prices",
                 "targets",   "suppliers",      "lead_days",  "travel_minutes", "price_policy",
                 "price_min", "price_max",      "price_step", "positions",      "output_per_worker",
                 "hiring"});
        defaults(f, {{"cash", 0},
                     {"stock", Json::object()},
                     {"wage", 0},
                     {"capacity", 0},
                     {"labor_required", 0},
                     {"output", ""},
                     {"inputs", Json::object()},
                     {"prices", Json::object()},
                     {"targets", Json::object()},
                     {"suppliers", Json::object()},
                     {"lead_days", 0},
                     {"travel_minutes", 0},
                     {"price_policy", "fixed"},
                     {"price_min", 1},
                     {"price_max", 100000},
                     {"price_step", 0},
                     {"hiring", Json::object()}});
        const auto id = f.at("id").get<std::string>();
        if (!std::regex_match(id, std::regex("firm:[1-9][0-9]*")) || !ids.insert(id).second)
            throw Error(2, "invalid/duplicate firm ID");
        if (f.at("kind") != "producer" && f["kind"] != "retailer")
            throw Error(2, "unknown firm kind");
        for (auto k : {"cash", "wage", "capacity", "labor_required", "lead_days", "travel_minutes",
                       "price_min", "price_max", "price_step"})
            nonnegative(f[k]);
        if (f.contains("positions"))
            nonnegative(f["positions"]);
        if (f.contains("output_per_worker") && nonnegative(f["output_per_worker"]) == 0 &&
            f["kind"] == "producer")
            throw Error(2, "producer output_per_worker must be positive");
        // Vacancy-board policy: offers start at the firm's wage and move by wage_step within bounds.
        auto &hiring = f["hiring"];
        keys(hiring, {"wage_min", "wage_max", "wage_step", "raise_after_unfilled_days", "min_experience_days",
                      "accept_employed", "rehire_cooldown_days"});
        auto wage = nonnegative(f["wage"]);
        defaults(hiring, {{"wage_step", wage / 10},
                          {"wage_min", wage},
                          {"raise_after_unfilled_days", 2},
                          {"min_experience_days", 0},
                          {"accept_employed", true},
                          {"rehire_cooldown_days", 2}});
        if (!hiring.contains("wage_max"))
            hiring["wage_max"] = add(wage, mul(3, nonnegative(hiring["wage_step"])));
        for (auto k : {"wage_min", "wage_max", "wage_step", "raise_after_unfilled_days",
                       "min_experience_days", "rehire_cooldown_days"})
            nonnegative(hiring[k]);
        if (!hiring["accept_employed"].is_boolean())
            throw Error(2, "accept_employed must be boolean");
        if (integer(hiring["wage_min"]) > wage || integer(hiring["wage_max"]) < wage ||
            integer(hiring["raise_after_unfilled_days"]) == 0)
            throw Error(2, "wage offer bounds must contain the firm wage with a positive raise interval");
        for (auto k : {"stock", "inputs", "prices", "targets"})
            quantities(f[k]);
        for (auto it = f["prices"].begin(); it != f["prices"].end(); ++it)
            if (integer(it.value()) == 0)
                throw Error(2, "price must be positive");
        if (f["price_policy"] != "fixed" && f["price_policy"] != "inventory_band")
            throw Error(2, "unknown price policy");
        if (integer(f["price_min"]) < 1 || integer(f["price_min"]) > integer(f["price_max"]))
            throw Error(2, "invalid price bounds");
        if (f["price_policy"] == "inventory_band")
            for (auto it = f["prices"].begin(); it != f["prices"].end(); ++it)
                if (integer(it.value()) < integer(f["price_min"]) ||
                    integer(it.value()) > integer(f["price_max"]))
                    throw Error(2, "initial price outside policy bounds");
        if (f["kind"] == "producer" && (!products.contains(f["output"].get<std::string>()) ||
                                        integer(f["labor_required"]) < 1 || integer(f["wage"]) < 1))
            throw Error(2, "producer needs output and paid labor");
        if (f["kind"] == "retailer" && (integer(f["capacity"]) != 0 || !f["inputs"].empty() ||
                                        (f.contains("output_per_worker") && integer(f["output_per_worker"]))))
            throw Error(2, "retailer cannot declare production");
        // Default: the minimum crew produces the full capacity, as before per-worker output existed.
        if (!f.contains("output_per_worker"))
            f["output_per_worker"] =
                f["kind"] == "producer"
                    ? std::max(Amount{1}, ceil_div(integer(f["capacity"]), integer(f["labor_required"])))
                    : 0;
        if (!f["suppliers"].is_object())
            throw Error(2, "supplier map required");
    }
    for (auto &h : j["households"]) {
        keys(h, {"id", "cash", "stock", "employer", "budget", "needs", "known_shops", "time_minutes",
                 "time_value", "job_preferences"});
        defaults(h, {{"cash", 0},
                     {"stock", Json::object()},
                     {"time_minutes", 60},
                     {"time_value", 0},
                     {"job_preferences", Json::object()}});
        preferences(h["job_preferences"], employment["job_preferences"]);
        auto id = h.at("id").get<std::string>();
        if (!std::regex_match(id, std::regex("household:[1-9][0-9]*")) || !ids.insert(id).second)
            throw Error(2, "invalid/duplicate household ID");
        for (auto k : {"cash", "budget", "time_minutes", "time_value"})
            nonnegative(h.at(k));
        quantities(h.at("needs"));
        quantities(h["stock"]);
        // An empty employer means unemployed at the start: under fixed employment the household
        // never works; under dynamic employment it looks for a vacancy.
        if (h.at("employer").is_null())
            h["employer"] = "";
        auto e = h.at("employer").get<std::string>();
        if (!e.empty() && (!ids.contains(e) || e.rfind("firm:", 0) != 0))
            throw Error(2, "invalid employer");
        if (!h.at("known_shops").is_array())
            throw Error(2, "known_shops must be array");
        std::set<std::string> known;
        for (auto &k : h["known_shops"]) {
            auto s = k.get<std::string>();
            if (!ids.contains(s) || s.rfind("firm:", 0) != 0 || !known.insert(s).second)
                throw Error(2, "invalid/duplicate known shop");
            bool retail = false;
            for (auto &f : j["firms"])
                if (f["id"] == s)
                    retail = f["kind"] == "retailer";
            if (!retail)
                throw Error(2, "known shop is not retailer");
        }
    }
    for (auto &f : j["firms"]) {
        // Desired headcount defaults to the scenario's initial staffing of this firm.
        if (!f.contains("positions")) {
            Amount staffed = 0;
            for (auto &h : j["households"])
                if (h["employer"] == f["id"])
                    ++staffed;
            f["positions"] = staffed;
        }
        for (auto it = f["suppliers"].begin(); it != f["suppliers"].end(); ++it) {
            if (!products.contains(it.key()) || !ids.contains(it.value().get<std::string>()) ||
                it.value() == f["id"])
                throw Error(2, "invalid supplier");
            bool offer = false;
            for (auto &s : j["firms"])
                if (s["id"] == it.value())
                    offer = s["prices"].contains(it.key());
            if (!offer)
                throw Error(2, "supplier does not sell product");
        }
        for (auto it = f["targets"].begin(); it != f["targets"].end(); ++it)
            if (!f["suppliers"].contains(it.key()))
                throw Error(2, "procurement target lacks supplier");
    }
    auto order = [](const Json &a, const Json &b) {
        return a.at("id").get<std::string>() < b.at("id").get<std::string>();
    };
    std::sort(j["households"].begin(), j["households"].end(), order);
    std::sort(j["firms"].begin(), j["firms"].end(), order);
    std::sort(j["products"].begin(), j["products"].end());
    for (auto mode : {employment_fixed, employment_dynamic})
        validate_state(initial(j, 0, mode), j); // also check aggregate endowments for overflow
    return j;
}
State initial(const Json &j, std::uint64_t seed, const std::string &employment_mode) {
    State s;
    if (seed > static_cast<std::uint64_t>(std::numeric_limits<Amount>::max()))
        throw Error(2, "seed exceeds signed 64-bit range");
    if (!valid_employment_mode(employment_mode))
        throw Error(2, "employment mode must be fixed or dynamic");
    s.seed = seed;
    s.public_board = j.at("public_board");
    s.employment_mode = employment_mode;
    for (const auto &f : j.at("firms")) {
        Json x = f;
        x["payroll_missed_days"] = 0;
        x["demand_history"] = Json::array();
        s.firms.push_back(x.get<Firm>());
        Account a;
        a.id = f.at("id");
        a.cash = integer(f.at("cash"));
        a.stock = f.at("stock").get<std::map<std::string, Amount>>();
        for (auto &p : j.at("products")) {
            a.stock.try_emplace(p.get<std::string>(), 0);
            a.reserved_stock[p.get<std::string>()] = 0;
        }
        s.accounts.push_back(a);
    }
    for (const auto &h : j.at("households")) {
        Json x = h;
        x["knowledge"] = Json::array();
        x["unpaid_days"] = 0;
        s.households.push_back(x.get<Household>());
        Account a;
        a.id = h.at("id");
        a.cash = integer(h.at("cash"));
        a.stock = h.at("stock").get<std::map<std::string, Amount>>();
        for (auto &p : j.at("products")) {
            a.stock.try_emplace(p.get<std::string>(), 0);
            a.reserved_stock[p.get<std::string>()] = 0;
        }
        s.accounts.push_back(a);
    }
    for (auto &h : s.households)
        for (auto &k : h.known_shops) {
            auto &f = firm(s, k);
            for (auto &[p, price] : f.prices)
                h.knowledge.push_back({k, p, price, stock(account(s, k).stock, p), -1, "initial_visit"});
        }
    // Every scenario job is an open employment spell from the first simulated day.
    for (auto &h : s.households)
        if (!h.employer.empty())
            s.employment.push_back({"employment:" + std::to_string(s.next_employment++), h.id, h.employer,
                                    s.next_day, -1, firm(s, h.employer).wage, "INITIAL", ""});
    return s;
}
void validate_state(const State &s, const Json &scenario) {
    if (s.seed > static_cast<std::uint64_t>(std::numeric_limits<Amount>::max()))
        throw Error(5, "seed out of range");
    if (s.next_day < 0 || s.next_event == 0 || s.next_order == 0 || s.next_decision == 0 ||
        s.next_event > static_cast<std::uint64_t>(std::numeric_limits<Amount>::max()) ||
        s.next_order > static_cast<std::uint64_t>(std::numeric_limits<Amount>::max()) ||
        s.next_decision > static_cast<std::uint64_t>(std::numeric_limits<Amount>::max()))
        throw Error(5, "invalid clock/sequence");
    Amount total = 0;
    std::map<std::string, Amount> goods;
    std::set<std::string> ids;
    std::map<std::string, Amount> reserved_cash;
    std::map<std::pair<std::string, std::string>, Amount> reserved_goods;
    for (auto &o : s.orders) {
        if (!std::regex_match(o.id, std::regex("order:[1-9][0-9]*")) || !ids.insert(o.id).second ||
            o.quantity <= 0 || o.price <= 0 || o.arrival < s.next_day ||
            std::find(scenario.at("products").begin(), scenario.at("products").end(), Json(o.product)) ==
                scenario.at("products").end())
            throw Error(5, "invalid pending order");
        if (o.buyer == o.seller || std::stoull(o.id.substr(6)) >= s.next_order)
            throw Error(5, "invalid pending order identity");
        account(s, o.buyer);
        account(s, o.seller);
        reserved_cash[o.buyer] = add(reserved_cash[o.buyer], mul(o.quantity, o.price));
        reserved_goods[{o.seller, o.product}] = add(reserved_goods[{o.seller, o.product}], o.quantity);
    }
    ids.clear();
    for (const auto &a : s.accounts) {
        if (a.stock.size() != scenario.at("products").size() ||
            a.reserved_stock.size() != scenario.at("products").size())
            throw Error(5, "invalid inventory products");
        if (!ids.insert(a.id).second || a.cash < 0 || a.reserved_cash < 0 || a.reserved_cash > a.cash ||
            a.reserved_cash != reserved_cash[a.id])
            throw Error(5, "invalid cash/reservation");
        if (a.labor != 0)
            throw Error(5, "labor is not storable across day boundaries");
        total = add(total, a.cash);
        for (auto &p : scenario.at("products")) {
            auto product = p.get<std::string>();
            if (!a.stock.contains(product) || !a.reserved_stock.contains(product))
                throw Error(5, "incomplete inventory product map");
            auto q = stock(a.stock, product), r = stock(a.reserved_stock, product);
            if (q < 0 || r < 0 || r > q || r != reserved_goods[{a.id, product}])
                throw Error(5, "invalid stock/reservation");
            goods[product] = add(goods[product], q);
        }
    }
    Amount endowed = 0;
    for (auto kind : {"firms", "households"})
        for (auto &entity : scenario.at(kind))
            endowed = add(endowed, integer(entity.at("cash")));
    if (total != endowed)
        throw Error(5, "cash conservation violation");
    if (s.accounts.size() != s.households.size() + s.firms.size())
        throw Error(5, "account count mismatch");
    if (!valid_employment_mode(s.employment_mode))
        throw Error(5, "invalid employment mode");
    const bool dynamic = s.employment_mode == employment_dynamic;
    if (s.next_employment == 0 || s.next_change == 0 ||
        s.next_employment > static_cast<std::uint64_t>(std::numeric_limits<Amount>::max()) ||
        s.next_change > static_cast<std::uint64_t>(std::numeric_limits<Amount>::max()))
        throw Error(5, "invalid employment sequence");
    for (auto &h : s.households) {
        account(s, h.id);
        if (!h.employer.empty() && h.employer.rfind("firm:", 0) != 0)
            throw Error(5, "invalid employer");
        if (!h.employer.empty())
            account(s, h.employer);
        if (h.unpaid_days < 0 || (h.employer.empty() && h.unpaid_days != 0))
            throw Error(5, "invalid unpaid-day counter");
        auto &jp = h.job_preferences;
        if (jp.reservation_wage < 0 || jp.min_wage_gain < 0 || jp.min_wage_gain_percent < 0 ||
            jp.min_tenure_days < 0 || jp.unpaid_tolerance_days <= 0)
            throw Error(5, "invalid job preferences");
        std::set<std::string> known;
        for (auto &shop : h.known_shops) {
            account(s, shop);
            if (!known.insert(shop).second)
                throw Error(5, "duplicate known shop");
        }
        std::set<std::pair<std::string, std::string>> observations;
        for (auto &o : h.knowledge) {
            account(s, o.shop);
            if (!known.contains(o.shop) || !observations.insert({o.shop, o.product}).second ||
                !account(s, o.shop).stock.contains(o.product) || o.price < 1 || o.stock < 0 ||
                o.day >= s.next_day || o.day < -1)
                throw Error(5, "invalid observation");
        }
    }
    for (auto &f : s.firms) {
        account(s, f.id);
        for (auto &[p, price] : f.prices)
            if (price < 1)
                throw Error(5, "invalid current price");
        if (f.positions < 0 || f.payroll_missed_days < 0 || f.output_per_worker < 0 ||
            (f.kind == "producer" && f.output_per_worker == 0) ||
            f.demand_history.size() >
                static_cast<std::size_t>(integer(scenario.at("employment").at("demand_window_days"))))
            throw Error(5, "invalid staffing counters");
        for (auto q : f.demand_history)
            if (q < 0)
                throw Error(5, "invalid demand observation");
    }
    // Employment records: each employed household has exactly one open spell naming its employer;
    // unemployed households have none. Closed spells are history and must be well-formed.
    std::map<std::string, std::string> open_spells;
    ids.clear();
    for (auto &r : s.employment) {
        if (!std::regex_match(r.id, std::regex("employment:[1-9][0-9]*")) || !ids.insert(r.id).second ||
            std::stoull(r.id.substr(11)) >= s.next_employment)
            throw Error(5, "invalid employment record identity");
        if (r.household.rfind("household:", 0) != 0 || r.employer.rfind("firm:", 0) != 0)
            throw Error(5, "invalid employment record parties");
        account(s, r.household);
        account(s, r.employer);
        if (r.start < 0 || r.start > s.next_day || (r.end != -1 && (r.end < r.start || r.end > s.next_day)) ||
            r.wage < 0)
            throw Error(5, "invalid employment record interval");
        if ((r.end == -1) != r.end_reason.empty())
            throw Error(5, "employment record end reason disagrees with its end");
        if (r.end == -1 && !open_spells.emplace(r.household, r.employer).second)
            throw Error(5, "household holds two jobs");
        if (!dynamic && (r.end != -1 || r.reason != "INITIAL"))
            throw Error(5, "fixed employment cannot change");
    }
    for (auto &h : s.households) {
        auto it = open_spells.find(h.id);
        if (h.employer.empty() ? it != open_spells.end()
                               : (it == open_spells.end() || it->second != h.employer))
            throw Error(5, "employment record disagrees with household employer");
    }
    // Pending changes: at most one per household, consistent with the current job, still in the future.
    ids.clear();
    std::set<std::string> changing;
    for (auto &c : s.pending_changes) {
        if (!dynamic)
            throw Error(5, "fixed employment cannot schedule changes");
        if (!std::regex_match(c.id, std::regex("change:[1-9][0-9]*")) || !ids.insert(c.id).second ||
            std::stoull(c.id.substr(7)) >= s.next_change)
            throw Error(5, "invalid employment change identity");
        if (c.household.rfind("household:", 0) != 0 || !changing.insert(c.household).second)
            throw Error(5, "household has two pending employment changes");
        account(s, c.household);
        if (c.from_employer == c.to_employer ||
            (!c.to_employer.empty() && c.to_employer.rfind("firm:", 0) != 0))
            throw Error(5, "invalid employment change parties");
        if (!c.to_employer.empty())
            account(s, c.to_employer);
        bool current = false;
        for (auto &h : s.households)
            if (h.id == c.household && h.employer == c.from_employer)
                current = true;
        if (!current)
            throw Error(5, "pending employment change does not match current job");
        if (c.requested < 0 || c.effective < s.next_day || c.effective <= c.requested)
            throw Error(5, "invalid employment change timing");
        if (c.wage < 0 || (c.to_employer.empty() && (c.wage != 0 || !c.posting.empty())) ||
            (!c.posting.empty() && (!std::regex_match(c.posting, std::regex("posting:[1-9][0-9]*")) ||
                                    std::stoull(c.posting.substr(8)) >= s.next_posting)))
            throw Error(5, "invalid employment change terms");
    }
    // Vacancy board: open postings only, one firm each, offers within that firm's policy bounds.
    ids.clear();
    if (s.next_posting == 0 ||
        s.next_posting > static_cast<std::uint64_t>(std::numeric_limits<Amount>::max()))
        throw Error(5, "invalid posting sequence");
    for (auto &p : s.board) {
        if (!dynamic)
            throw Error(5, "fixed employment has no vacancy board");
        if (!std::regex_match(p.id, std::regex("posting:[1-9][0-9]*")) || !ids.insert(p.id).second ||
            std::stoull(p.id.substr(8)) >= s.next_posting)
            throw Error(5, "invalid posting identity");
        if (p.firm.rfind("firm:", 0) != 0)
            throw Error(5, "invalid posting firm");
        account(s, p.firm);
        const Firm *owner = nullptr;
        for (auto &f : s.firms)
            if (f.id == p.firm)
                owner = &f;
        if (!owner || p.openings <= 0 || p.wage < owner->hiring.wage_min || p.wage > owner->hiring.wage_max ||
            p.min_experience_days < 0 || p.rehire_cooldown_days < 0 || p.posted < 0 ||
            p.posted > s.next_day || p.unfilled_days < 0 || p.applications < 0)
            throw Error(5, "invalid posting terms");
        for (auto &other : s.board)
            if (&other != &p && other.firm == p.firm)
                throw Error(5, "firm has two postings");
    }
}
Json effect(const std::string &actor, const std::string &field, Amount delta, const std::string &product) {
    return {{"actor", actor}, {"field", field}, {"product", product}, {"delta", delta}};
}
} // namespace econ
