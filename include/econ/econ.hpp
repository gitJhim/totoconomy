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
inline constexpr auto model_version = "econ-2";
inline constexpr auto employment_fixed = "fixed";
inline constexpr auto employment_dynamic = "dynamic";
struct Error : std::runtime_error {
    int code;
    Error(int c, const std::string &s) : std::runtime_error(s), code(c) {}
};
Amount add(Amount a, Amount b);
Amount mul(Amount a, Amount b);
Amount ceil_div(Amount a, Amount b);
Amount mul_capped(Amount a, Amount b, Amount limit);
Amount integer(const Json &j);
std::string digest(const Json &j);
std::uint64_t keyed(std::uint64_t seed, std::uint64_t day, std::uint64_t actor, std::uint64_t purpose);
struct Account {
    EntityId id;
    Money cash = 0, reserved_cash = 0;
    std::map<ProductId, Quantity> stock, reserved_stock;
    // Labor on hand today, in worker-days: payroll delivers it, production consumes it, and the
    // remainder is released at the end of the work phase. It is never carried across days.
    Quantity labor = 0;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE(Account, id, cash, reserved_cash, stock, reserved_stock, labor)
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
// How a household judges a vacancy-board offer against its situation. Net wage is the offer
// minus the commute (travel_minutes x time_value). Scenario-level defaults apply per household.
struct JobPreferences {
    Money reservation_wage = 0;        // lowest net wage it will take at all
    Money min_wage_gain = 0;           // absolute net gain needed to leave a paying job
    Amount min_wage_gain_percent = 10; // relative net gain needed to leave a paying job
    Day min_tenure_days = 5;           // days in the current job before it will switch
    bool search_while_employed = true; // whether a paid worker reads the board at all
    Day unpaid_tolerance_days = 3;     // unpaid days after which any paying offer is preferable
    NLOHMANN_DEFINE_TYPE_INTRUSIVE(JobPreferences, reservation_wage, min_wage_gain, min_wage_gain_percent,
                                   min_tenure_days, search_while_employed, unpaid_tolerance_days)
};
struct Household {
    EntityId id, employer; // employer is empty while unemployed (dynamic employment only)
    Money budget = 0;
    Minutes time_minutes = 60;
    Money time_value = 0; // cents per minute in the choice score
    std::map<ProductId, Quantity> needs;
    std::vector<EntityId> known_shops;
    std::vector<Observation> knowledge;
    Day unpaid_days = 0; // consecutive days holding a job without receiving a wage
    JobPreferences job_preferences;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE(Household, id, employer, budget, time_minutes, time_value, needs,
                                   known_shops, knowledge, unpaid_days, job_preferences)
};
// How a firm advertises jobs: the offer starts at the firm's wage and moves in wage_step
// increments within [wage_min, wage_max]; the eligibility fields are copied onto each posting.
struct HiringPolicy {
    Money wage_min = 0, wage_max = 0, wage_step = 0;
    Day raise_after_unfilled_days = 2;
    Day min_experience_days = 0;  // total days the applicant has held any job
    bool accept_employed = true;  // whether currently employed applicants may apply
    Day rehire_cooldown_days = 2; // days after a layoff before this firm rehires that person
    NLOHMANN_DEFINE_TYPE_INTRUSIVE(HiringPolicy, wage_min, wage_max, wage_step, raise_after_unfilled_days,
                                   min_experience_days, accept_employed, rehire_cooldown_days)
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
    Quantity positions = 0;               // maximum headcount (dynamic employment never staffs above this)
    Quantity output_per_worker = 0;       // producer output per paid worker, capped by capacity
    HiringPolicy hiring;                  // wage offer bounds and eligibility for the vacancy board
    Day payroll_missed_days = 0;          // consecutive days with workers present but no payroll run
    std::vector<Quantity> demand_history; // units sold + stock-out shortfalls per day, newest last
    NLOHMANN_DEFINE_TYPE_INTRUSIVE(Firm, id, kind, wage, capacity, labor_required, output, inputs, prices,
                                   targets, suppliers, lead_days, travel_minutes, price_policy, price_min,
                                   price_max, price_step, positions, output_per_worker, hiring,
                                   payroll_missed_days, demand_history)
};
// One employment spell: the household holds a job at the employer from start (inclusive)
// until end (exclusive); end is -1 while the spell is open.
struct EmploymentRecord {
    EntityId id, household, employer;
    Day start = 0, end = -1;
    Money wage = 0;                 // the wage agreed for this spell; payroll pays it every active day
    std::string reason, end_reason; // why the spell opened / why it closed (empty while open)
    NLOHMANN_DEFINE_TYPE_INTRUSIVE(EmploymentRecord, id, household, employer, start, end, wage, reason,
                                   end_reason)
};
// A scheduled transition that takes effect in the employment phase of the effective day.
// Empty from_employer means a hire from unemployment; empty to_employer means a separation.
// Hires carry the wage and the board posting they answered.
struct EmploymentChange {
    EntityId id, household, from_employer, to_employer;
    Day requested = 0, effective = 0;
    Money wage = 0;
    EntityId posting;
    std::string reason;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE(EmploymentChange, id, household, from_employer, to_employer, requested,
                                   effective, wage, posting, reason)
};
// A vacancy-board entry: a firm's open positions at an advertised wage with eligibility rules.
struct Posting {
    EntityId id, firm;
    Quantity openings = 0;
    Money wage = 0;
    Day min_experience_days = 0;
    bool accept_employed = true;
    Day rehire_cooldown_days = 0;
    Day posted = 0, unfilled_days = 0; // unfilled_days: consecutive days with openings and no applicant
    Amount applications = 0;           // applications received since it was posted
    std::string reason;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE(Posting, id, firm, openings, wage, min_experience_days, accept_employed,
                                   rehire_cooldown_days, posted, unfilled_days, applications, reason)
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
    std::uint64_t seed = 0, next_event = 1, next_decision = 1, next_order = 1, next_employment = 1,
                  next_change = 1, next_posting = 1;
    bool public_board = false;
    std::string employment_mode = employment_fixed;
    std::vector<Account> accounts;
    std::vector<Household> households;
    std::vector<Firm> firms;
    std::vector<Order> orders;
    std::vector<EmploymentRecord> employment;
    std::vector<EmploymentChange> pending_changes;
    std::vector<Posting> board;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE(State, next_day, seed, next_event, next_decision, next_order,
                                   next_employment, next_change, next_posting, public_board, employment_mode,
                                   accounts, households, firms, orders, employment, pending_changes, board)
};
Json normalize(Json j);
bool valid_employment_mode(const std::string &mode);
State initial(const Json &scenario, std::uint64_t seed,
              const std::string &employment_mode = employment_fixed);
void validate_state(const State &s, const Json &scenario);
Account &account(State &s, const std::string &id);
const Account &account(const State &s, const std::string &id);
Firm &firm(State &s, const std::string &id);
Amount stock(const std::map<std::string, Amount> &m, const std::string &p);
// Total days a household has held jobs up to (excluding) today, from its employment records.
Day experience_days(const State &s, const EntityId &household, Day today);
// Payroll a firm owes per active day: the sum of the wages of its open employment spells.
Money daily_payroll(const State &s, const EntityId &firm);
// Days the household has held its current job (0 when unemployed).
Day tenure_days(const State &s, const EntityId &household, Day today);
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
    void employment();
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
    std::map<EntityId, Quantity> sold_today, shortfall_today; // per seller, feeding demand_history
    void settle(const Order &o, bool reserved, const std::string &stage);
    Json staffing_target(const Firm &f, Amount staying, Amount leaving);
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
    std::string employment = employment_fixed; // ignored when resuming: the checkpoint state decides
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
