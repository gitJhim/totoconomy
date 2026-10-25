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
Json normalize(Json j) {
    keys(j, {"schema", "name", "products", "households", "firms", "refresh_days", "explore_days",
             "public_board"});
    if (j.at("schema") != "scenario-1")
        throw Error(2, "unsupported scenario schema");
    defaults(j, {{"name", "town"}, {"refresh_days", 3}, {"explore_days", 7}, {"public_board", false}});
    if (!j["public_board"].is_boolean() || !j["name"].is_string())
        throw Error(2, "invalid scenario metadata");
    if (nonnegative(j["refresh_days"]) == 0 || nonnegative(j["explore_days"]) == 0)
        throw Error(2, "observation intervals must be positive");
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
        keys(f, {"id", "kind", "cash", "stock", "wage", "capacity", "labor_required", "output", "inputs",
                 "prices", "targets", "suppliers", "lead_days", "travel_minutes", "price_policy", "price_min",
                 "price_max", "price_step"});
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
                     {"price_step", 0}});
        const auto id = f.at("id").get<std::string>();
        if (!std::regex_match(id, std::regex("firm:[1-9][0-9]*")) || !ids.insert(id).second)
            throw Error(2, "invalid/duplicate firm ID");
        if (f.at("kind") != "producer" && f["kind"] != "retailer")
            throw Error(2, "unknown firm kind");
        for (auto k : {"cash", "wage", "capacity", "labor_required", "lead_days", "travel_minutes",
                       "price_min", "price_max", "price_step"})
            nonnegative(f[k]);
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
        if (f["kind"] == "retailer" && (integer(f["capacity"]) != 0 || !f["inputs"].empty()))
            throw Error(2, "retailer cannot declare production");
        if (!f["suppliers"].is_object())
            throw Error(2, "supplier map required");
    }
    for (auto &h : j["households"]) {
        keys(h, {"id", "cash", "stock", "employer", "budget", "needs", "known_shops", "time_minutes",
                 "time_value"});
        defaults(h, {{"cash", 0}, {"stock", Json::object()}, {"time_minutes", 60}, {"time_value", 0}});
        auto id = h.at("id").get<std::string>();
        if (!std::regex_match(id, std::regex("household:[1-9][0-9]*")) || !ids.insert(id).second)
            throw Error(2, "invalid/duplicate household ID");
        for (auto k : {"cash", "budget", "time_minutes", "time_value"})
            nonnegative(h.at(k));
        quantities(h.at("needs"));
        quantities(h["stock"]);
        auto e = h.at("employer").get<std::string>();
        if (!ids.contains(e) || e.rfind("firm:", 0) != 0)
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
    auto s = initial(j, 0);
    validate_state(s, j); // also check aggregate endowments for overflow
    return j;
}
State initial(const Json &j, std::uint64_t seed) {
    State s;
    if (seed > static_cast<std::uint64_t>(std::numeric_limits<Amount>::max()))
        throw Error(2, "seed exceeds signed 64-bit range");
    s.seed = seed;
    s.public_board = j.at("public_board");
    for (const auto &f : j.at("firms")) {
        s.firms.push_back(f.get<Firm>());
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
    for (auto &h : s.households) {
        account(s, h.id);
        account(s, h.employer);
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
    }
}
Json effect(const std::string &actor, const std::string &field, Amount delta, const std::string &product) {
    return {{"actor", actor}, {"field", field}, {"product", product}, {"delta", delta}};
}
} // namespace econ
