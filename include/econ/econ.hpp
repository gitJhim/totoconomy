#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace econ {
using Json = nlohmann::json;
// Semantic role types; checked arithmetic shares signed 64-bit storage.
// These aliases document units at interfaces, not compile-time dimensional enforcement.
using Amount = std::int64_t;
using Money = std::int64_t;     // cents
using Quantity = std::int64_t;  // whole units of one product
using UnitPrice = std::int64_t; // cents per whole unit
using Day = std::int64_t;
using Minutes = std::int64_t;
using EntityId = std::string;
using ProductId = std::string;
inline constexpr auto model_version = "econ-1";
struct Error : std::runtime_error {
    int code;
    Error(int c, const std::string &s) : std::runtime_error(s), code(c) {}
};
Amount add(Amount a, Amount b);
Amount mul(Amount a, Amount b);
Amount integer(const Json &j);
std::string digest(const Json &j);
std::uint64_t keyed(std::uint64_t seed, std::uint64_t day, std::uint64_t actor, std::uint64_t purpose);
struct Account {
    EntityId id;
    Money cash = 0, reserved_cash = 0;
    std::map<ProductId, Quantity> stock, reserved_stock;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE(Account, id, cash, reserved_cash, stock, reserved_stock)
};
struct Observation {
    EntityId shop;
    ProductId product;
    UnitPrice price = 0;
    Quantity stock = 0;
    Day day = 0;
    std::string source;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE(Observation, shop, product, price, stock, day, source)
};
struct Household {
    EntityId id, employer;
    Money budget = 0;
    Minutes time_minutes = 60;
    Money time_value = 0; // cents per minute in the choice score
    std::map<ProductId, Quantity> needs;
    std::vector<EntityId> known_shops;
    std::vector<Observation> knowledge;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE(Household, id, employer, budget, time_minutes, time_value, needs,
                                   known_shops, knowledge)
};
struct Firm {
    EntityId id;
    std::string kind;
    Money wage = 0;
    Quantity capacity = 0, labor_required = 0;
    ProductId output;
    std::map<ProductId, Quantity> inputs, targets;
    std::map<ProductId, UnitPrice> prices;
    std::map<ProductId, EntityId> suppliers;
    Day lead_days = 0;
    Minutes travel_minutes = 0;
    std::string price_policy = "fixed";
    UnitPrice price_min = 1, price_max = 100000, price_step = 0;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE(Firm, id, kind, wage, capacity, labor_required, output, inputs, prices,
                                   targets, suppliers, lead_days, travel_minutes, price_policy, price_min,
                                   price_max, price_step)
};
struct Order {
    EntityId id, buyer, seller;
    ProductId product;
    Quantity quantity = 0;
    UnitPrice price = 0;
    Day arrival = 0;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE(Order, id, buyer, seller, product, quantity, price, arrival)
};
struct State {
    Day next_day = 0;
    std::uint64_t seed = 0, next_event = 1, next_decision = 1, next_order = 1;
    bool public_board = false;
    std::vector<Account> accounts;
    std::vector<Household> households;
    std::vector<Firm> firms;
    std::vector<Order> orders;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE(State, next_day, seed, next_event, next_decision, next_order, public_board,
                                   accounts, households, firms, orders)
};
Json normalize(Json j);
State initial(const Json &scenario, std::uint64_t seed);
void validate_state(const State &s, const Json &scenario);
Account &account(State &s, const std::string &id);
const Account &account(const State &s, const std::string &id);
Firm &firm(State &s, const std::string &id);
Amount stock(const std::map<std::string, Amount> &m, const std::string &p);
Json effect(const std::string &actor, const std::string &field, Amount delta,
            const std::string &product = "");
using Sink = std::function<void(const Json &)>;
class Engine {
  public:
    State state;
    Json scenario;
    Sink events, decisions;
    std::string trace;
    Engine(State s, Json config, Sink ev = {}, Sink dec = {}, std::string level = "audit");
    Json emit(const std::string &type, Json data = Json::object(), Json effects = Json::array());
    std::string trade(const std::string &buyer, const std::string &seller, const std::string &product,
                      Quantity q, UnitPrice price, const std::string &stage, Day delay = 0);
    void record_decision(Json evidence);
    void arrivals();
    void work();
    void procure();
    void observe(Household &h, const std::string &shop, const std::string &source);
    void publish();
    void shop();
    void consume();
    void prices();
    Json step();
    void intervene(const Json &intervention);

  private:
    Json daily;
    std::string phase = "operations";
    void settle(const Order &o, bool reserved, const std::string &stage);
};
// A policy receives memories and household constraints only, never authoritative firms/accounts.
struct ShoppingView {
    EntityId actor;
    ProductId product;
    Day day;
    Quantity quantity;
    Money cash, budget;
    Minutes time_minutes;
    Money time_value;
    std::vector<Observation> observations;
    std::map<EntityId, Minutes> travel;
    std::vector<EntityId> shop_ids;
};
Json choose(const ShoppingView &view);
Json read_json(const std::filesystem::path &p);
void write_json(const std::filesystem::path &p, const Json &j);
void scan_lines(const std::filesystem::path &p, const Sink &sink);
Json checkpoint(const State &s, const Json &scenario, const std::string &origin,
                const std::string &parent_run);
struct Loaded {
    State state;
    Json scenario;
    std::string origin, parent_run, checkpoint_digest;
};
Loaded load_checkpoint(const std::filesystem::path &p);
struct RunOptions {
    std::filesystem::path out;
    Day until = 0, checkpoint_every = 0;
    std::string trace = "audit";
    bool overwrite = false;
    std::optional<Json> intervention;
    std::optional<Loaded> loaded;
};
Json run(const Json &scenario, std::uint64_t seed, const RunOptions &options);
Json verify(const std::filesystem::path &directory);
Json inspect(const std::filesystem::path &directory, const std::string &id);
Json explain(const std::filesystem::path &directory, const std::string &id);
Json compare(const std::filesystem::path &baseline, const std::filesystem::path &variant, Day from,
             Day until);
} // namespace econ
