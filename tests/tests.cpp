#include <econ/econ.hpp>
#include <fstream>
#include <iostream>
#include <limits>
#include <set>
#include <unistd.h>
using namespace econ;
namespace fs = std::filesystem;
#define REQUIRE(x)                                                                                           \
    do {                                                                                                     \
        if (!(x))                                                                                            \
            throw std::runtime_error(std::string(__FILE__) + ":" + std::to_string(__LINE__) + " " #x);       \
    } while (false)
static void fails(const std::function<void()> &f, int code = 0) {
    bool caught = false;
    try {
        f();
    } catch (const Error &e) {
        caught = true;
        if (code)
            REQUIRE(e.code == code);
    } catch (const Json::exception &) {
        caught = true;
    }
    REQUIRE(caught);
}
static Json fixture() {
    return normalize(read_json(fs::path(ECON_SOURCE) / "scenarios/accounting_cycle.json"));
}
static Json town() {
    return normalize(read_json(fs::path(ECON_SOURCE) / "scenarios/demo_town.json"));
}
static std::string bytes(const fs::path &p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}
static Json tree(const fs::path &p) {
    Json j = Json::object();
    for (auto &e : fs::recursive_directory_iterator(p))
        if (e.is_regular_file())
            j[fs::relative(e.path(), p).string()] = digest(Json(bytes(e.path())));
    return j;
}
static std::vector<Json> facts(const fs::path &p, Day start = 0) {
    std::vector<Json> result;
    scan_lines(p / "events.jsonl", [&](const Json &e) {
        if (integer(e.at("day")) >= start)
            result.push_back(e);
    });
    return result;
}
static std::vector<Json> of_type(const std::vector<Json> &events, const std::string &type) {
    std::vector<Json> result;
    for (auto &e : events)
        if (e.at("type") == type)
            result.push_back(e);
    return result;
}
static void accounting() {
    auto config = fixture();
    std::vector<Json> events;
    Engine e(initial(config, 42), config, [&](auto &j) { events.push_back(j); });
    e.work();
    REQUIRE(events.size() == 8);
    for (int i = 0; i < 6; ++i)
        REQUIRE(events[static_cast<std::size_t>(i)]["type"] == "wage");
    REQUIRE(events[6]["type"] == "production");
    REQUIRE(events[7]["type"] == "labor_released"); // the retailer's three worker-days became service
    REQUIRE(account(e.state, "firm:1").cash == 0);
    REQUIRE(account(e.state, "firm:2").cash == 3000);
    for (auto &h : e.state.households)
        REQUIRE(account(e.state, h.id).cash == 1000);
    REQUIRE(stock(account(e.state, "firm:1").stock, "food") == 6);
    e.procure();
    REQUIRE(account(e.state, "firm:1").cash == 3000);
    REQUIRE(account(e.state, "firm:2").cash == 0);
    REQUIRE(stock(account(e.state, "firm:2").stock, "food") == 6);
    e.publish();
    e.shop();
    REQUIRE(account(e.state, "firm:2").cash == 6000);
    for (auto &h : e.state.households) {
        REQUIRE(account(e.state, h.id).cash == 0);
        REQUIRE(stock(account(e.state, h.id).stock, "food") == 1);
    }
    e.consume();
    for (auto &a : e.state.accounts)
        REQUIRE(stock(a.stock, "food") == 0);
    Engine full(initial(config, 42), config);
    Amount produced = 0, consumed = 0;
    for (int d = 0; d < 30; ++d) {
        auto m = full.step();
        produced += integer(m["production"]["food"]);
        consumed += integer(m["consumption"]["food"]);
        REQUIRE(m["unmet"].empty());
        REQUIRE(integer(m["wages"]) == 6000);
    }
    REQUIRE(produced == 180);
    REQUIRE(consumed == 180);
    REQUIRE(account(full.state, "firm:1").cash == 3000);
    REQUIRE(account(full.state, "firm:2").cash == 6000);
    for (auto &h : full.state.households)
        REQUIRE(account(full.state, h.id).cash == 0);
    for (auto &a : full.state.accounts)
        REQUIRE(stock(a.stock, "food") == 0);
}
static void labor_ledger(const fs::path &root) {
    // Payroll is the transaction that turns cash into labor; production consumes exactly the
    // worker-days it needs; the rest is released the same day, so no labor ever crosses a boundary.
    auto config = fixture();
    std::vector<Json> events;
    Engine e(initial(config, 42), config, [&](auto &j) { events.push_back(j); });
    e.work();
    for (auto &w : of_type(events, "wage")) {
        REQUIRE(w["data"]["labor_units"] == 1);
        REQUIRE(w["effects"].size() == 3 && w["effects"][2]["field"] == "labor" &&
                w["effects"][2]["delta"] == 1 && w["effects"][2]["actor"] == w["data"]["employer"]);
    }
    auto production = of_type(events, "production");
    REQUIRE(production.size() == 1 && production[0]["data"]["labor_used"] == 3 &&
            production[0]["data"]["labor_idle"] == 0);
    REQUIRE(production[0]["effects"].back()["field"] == "labor" &&
            production[0]["effects"].back()["delta"] == -3);
    auto released = of_type(events, "labor_released");
    REQUIRE(released.size() == 1 && released[0]["data"]["firm"] == "firm:2" &&
            released[0]["data"]["units"] == 3 && released[0]["data"]["use"] == "retail_service");
    for (auto &a : e.state.accounts)
        REQUIRE(a.labor == 0);
    // An input-limited plant uses only the crew the output needs and idles the rest.
    auto limited = fixture();
    limited["products"].push_back("grain");
    limited["firms"][0]["inputs"] = {{"grain", 2}};
    limited["firms"][0]["stock"] = {{"grain", 7}};
    limited = normalize(limited);
    events.clear();
    Engine l(initial(limited, 1), limited, [&](auto &j) { events.push_back(j); });
    auto metric = l.step();
    production = of_type(events, "production");
    REQUIRE(production[0]["data"]["quantity"] == 3 && production[0]["data"]["labor_used"] == 2 &&
            production[0]["data"]["labor_idle"] == 1);
    bool idle = false;
    for (auto &r : of_type(events, "labor_released"))
        if (r["data"]["firm"] == "firm:1")
            idle = r["data"]["use"] == "idle" && r["data"]["units"] == 1;
    REQUIRE(idle);
    REQUIRE(metric["labor_units"] ==
            Json({{"delivered", 6}, {"used", 2}, {"idle", 1}, {"retail_service", 3}}));
    REQUIRE(metric["labor_flow"]["firm:1"] == Json({{"delivered", 3}, {"used", 2}, {"idle", 1}}));
    REQUIRE(metric["labor_flow"]["firm:2"] == Json({{"delivered", 3}, {"retail_service", 3}}));
    // No payroll, no labor, no production: an inactive firm records nothing in the labor ledger.
    auto broke = fixture();
    broke["firms"][0]["cash"] = 2999;
    broke = normalize(broke);
    events.clear();
    Engine b(initial(broke, 1), broke, [&](auto &j) { events.push_back(j); });
    auto idle_day = b.step();
    REQUIRE(of_type(events, "production").empty() && integer(idle_day["labor_units"]["delivered"]) == 3);
    REQUIRE(!idle_day["labor_flow"].contains("firm:1"));
    // Labor cannot be stored: a state carrying labor across a boundary is invalid.
    auto bad = e.state;
    bad.accounts[0].labor = 1;
    fails([&] { validate_state(bad, config); }, 5);
    // The verifier rebuilds the labor ledger: a wage that claims no labor, or metrics that disagree, fail.
    RunOptions opt;
    opt.out = root / "labor_ledger";
    opt.until = 3;
    run(config, 1, opt);
    auto events_path = opt.out / "events.jsonl";
    auto log = bytes(events_path);
    auto pos = log.find("\"type\":\"wage\"");
    auto line_start = log.rfind('\n', pos) + 1, line_end = log.find('\n', pos);
    auto line = Json::parse(log.substr(line_start, line_end - line_start));
    auto untouched = line;
    line["effects"].erase(2);
    {
        std::ofstream out(events_path);
        out << log.substr(0, line_start) << line.dump() << log.substr(line_end);
    }
    REQUIRE(verify(opt.out)["passed"] == false);
    {
        std::ofstream out(events_path);
        out << log;
    }
    auto metrics_path = opt.out / "metrics.jsonl";
    auto contents = bytes(metrics_path);
    auto newline = contents.find('\n');
    auto first = Json::parse(contents.substr(0, newline));
    first["labor_units"]["used"] = 0;
    {
        std::ofstream out(metrics_path);
        out << first.dump() << contents.substr(newline);
    }
    auto report = verify(opt.out);
    REQUIRE(report["passed"] == false);
    bool labor_failed = false;
    for (auto &c : report["checks"])
        if (c["check"] == "per_person_labor_records_reconcile" && c["status"] == "failed")
            labor_failed = true;
    REQUIRE(labor_failed);
}
static void firm_decisions() {
    auto config = fixture();
    std::vector<Json> decisions;
    Engine e(initial(config, 42), config, {}, [&](const Json &d) { decisions.push_back(d); });
    e.step();
    REQUIRE(decisions.size() == 12);
    REQUIRE(decisions[0]["actor"] == "firm:1");
    REQUIRE(decisions[0]["policy"] == "financed_payroll-1");
    REQUIRE(decisions[0]["constraints"]["payroll"] == 3000);
    REQUIRE(decisions[0]["event_ids"].size() == 3);
    REQUIRE(decisions[2]["policy"] == "paid_capacity_recipe-1");
    REQUIRE(decisions[2]["outcome"] == "PRODUCED");
    REQUIRE(decisions[3]["policy"] == "inventory_target-1");
    REQUIRE(decisions[3]["outcome"] == "SETTLED");
    REQUIRE(decisions.back()["policy"] == "fixed-1");
    REQUIRE(decisions.back()["outcome"] == "UNCHANGED");
}
static void transactions() {
    auto config = fixture();
    Engine e(initial(config, 1), config);
    auto before = Json(e.state.accounts);
    REQUIRE(e.trade("household:1", "firm:2", "food", 1, 1000, "retail").empty());
    REQUIRE(Json(e.state.accounts) == before);
    REQUIRE(e.trade("firm:2", "firm:1", "food", 1, 500, "wholesale").empty());
    REQUIRE(Json(e.state.accounts) == before);
    e.work();
    REQUIRE(!e.trade("firm:2", "firm:1", "food", 6, 500, "wholesale").empty());
    REQUIRE(!e.trade("household:1", "firm:2", "food", 1, 1000, "retail").empty());
    REQUIRE(account(e.state, "household:1").cash == 0);
    REQUIRE(stock(account(e.state, "household:1").stock, "food") == 1);
    fails([&] { e.trade("firm:2", "firm:1", "food", 0, 500, "wholesale"); });
    fails([&] { e.trade("bad", "firm:1", "food", 1, 500, "wholesale"); });
    Engine inactive(initial(config, 1), config);
    account(inactive.state, "firm:1").cash = 2999;
    inactive.work();
    REQUIRE(stock(account(inactive.state, "firm:1").stock, "food") == 0);
    REQUIRE(account(inactive.state, "household:1").cash == 0);
}
static void arithmetic() {
    auto max = std::numeric_limits<Amount>::max(), min = std::numeric_limits<Amount>::min();
    REQUIRE(add(max, 0) == max);
    REQUIRE(add(min, 0) == min);
    REQUIRE(mul(max, 1) == max);
    REQUIRE(mul(0, max) == 0);
    fails([&] { add(max, 1); }, 5);
    fails([&] { add(min, -1); }, 5);
    fails([&] { mul(max, 2); }, 5);
    fails([&] { mul(-1, 2); }, 2);
    fails([] { integer(Json(1.5)); }, 2);
    fails([] { integer(Json(std::numeric_limits<std::uint64_t>::max())); }, 2);
    auto config = fixture();
    Engine e(initial(config, 1), config);
    account(e.state, "firm:1").cash = max;
    account(e.state, "firm:1").stock["food"] = 1;
    auto before = Json(e.state.accounts);
    fails([&] { e.trade("firm:2", "firm:1", "food", 1, 1, "wholesale"); }, 5);
    REQUIRE(Json(e.state.accounts) == before);
}
static void id_exhaustion() {
    auto config = fixture();
    Engine e(initial(config, 1), config);
    e.work();
    auto before = Json(e.state.accounts);
    e.state.next_event = static_cast<std::uint64_t>(std::numeric_limits<Amount>::max());
    fails([&] { e.trade("firm:2", "firm:1", "food", 1, 500, "wholesale"); }, 5);
    REQUIRE(Json(e.state.accounts) == before);
    e.state.next_event = 100;
    e.state.next_order = static_cast<std::uint64_t>(std::numeric_limits<Amount>::max());
    fails([&] { e.trade("firm:2", "firm:1", "food", 1, 500, "wholesale", 1); }, 5);
    REQUIRE(Json(e.state.accounts) == before);
    REQUIRE(e.state.orders.empty());
}
static void price_boundaries() {
    auto config = town();
    Engine e(initial(config, 1), config);
    auto &retailer = firm(e.state, "firm:3");
    retailer.price_step = std::numeric_limits<Amount>::max();
    retailer.price_max = std::numeric_limits<Amount>::max();
    account(e.state, "firm:3").stock["food"] = 0;
    e.prices();
    REQUIRE(retailer.prices["food"] == std::numeric_limits<Amount>::max());
    account(e.state, "firm:3").stock["food"] = 100;
    e.prices();
    REQUIRE(retailer.prices["food"] == retailer.price_min);
}
static void recipe() {
    auto config = fixture();
    config["products"].push_back("grain");
    config["firms"][0]["inputs"] = {{"grain", 2}};
    config["firms"][0]["stock"] = {{"grain", 7}};
    config = normalize(config);
    Engine e(initial(config, 1), config);
    e.work();
    REQUIRE(stock(account(e.state, "firm:1").stock, "food") == 3);
    REQUIRE(stock(account(e.state, "firm:1").stock, "grain") == 1);
    auto capacity = fixture();
    capacity["products"].push_back("grain");
    capacity["firms"][0]["inputs"] = {{"grain", 2}};
    capacity["firms"][0]["stock"] = {{"grain", 100}};
    capacity = normalize(capacity);
    Engine f(initial(capacity, 1), capacity);
    f.work();
    REQUIRE(stock(account(f.state, "firm:1").stock, "food") == 6);
    REQUIRE(stock(account(f.state, "firm:1").stock, "grain") == 88);
}
static void reservations(const fs::path &root) {
    auto config = fixture();
    auto s = initial(config, 1);
    account(s, "firm:1").stock["food"] = 10;
    Engine e(s, config);
    REQUIRE(!e.trade("firm:2", "firm:1", "food", 6, 500, "wholesale", 2).empty());
    auto before = Json(e.state.accounts);
    REQUIRE(e.trade("firm:2", "firm:1", "food", 1, 4000, "wholesale", 2).empty());
    REQUIRE(e.trade("firm:2", "firm:1", "food", 5, 1, "wholesale", 2).empty());
    REQUIRE(before == Json(e.state.accounts));
    e.arrivals();
    REQUIRE(stock(account(e.state, "firm:2").stock, "food") == 0);
    e.state.next_day = 1;
    e.arrivals();
    REQUIRE(e.state.orders.size() == 1);
    e.state.next_day = 2;
    auto origin = digest(Json{{"scenario", config}, {"seed", e.state.seed}, {"model", model_version}});
    auto cp = root / "pending.json";
    write_json(cp, checkpoint(e.state, config, origin, root.string()));
    auto loaded = load_checkpoint(cp);
    Engine restored(loaded.state, loaded.scenario);
    restored.arrivals();
    REQUIRE(restored.state.orders.empty());
    REQUIRE(stock(account(restored.state, "firm:2").stock, "food") == 6);
    REQUIRE(stock(account(restored.state, "firm:1").stock, "food") == 4);
    REQUIRE(account(restored.state, "firm:2").cash == 3000);
    REQUIRE(account(restored.state, "firm:2").reserved_cash == 0);
    REQUIRE(stock(account(restored.state, "firm:1").reserved_stock, "food") == 0);
    auto after = Json(restored.state.accounts);
    restored.arrivals();
    REQUIRE(Json(restored.state.accounts) == after);
}
static void information() {
    ShoppingView v{"household:1",
                   "food",
                   0,
                   1,
                   1000,
                   1000,
                   60,
                   0,
                   {{"firm:2", "food", 900, 10, -1, "visit"}},
                   {{"firm:2", 5}, {"firm:3", 5}},
                   {"firm:2", "firm:3"}};
    auto d = choose(v);
    REQUIRE(d["chosen"] == "firm:2");
    REQUIRE(d["exclusions"][0]["reason"] == "UNKNOWN_SHOP");
    REQUIRE(!d["exclusions"][0].contains("perceived_price"));
    v.observations.push_back({"firm:3", "food", 500, 10, 0, "public_board"});
    REQUIRE(choose(v)["chosen"] == "firm:3");
    v.cash = 0;
    REQUIRE(choose(v)["chosen"].is_null());
    auto config = fixture();
    config["firms"].push_back({{"id", "firm:3"},
                               {"kind", "retailer"},
                               {"cash", 0},
                               {"stock", {{"food", 10}}},
                               {"prices", {{"food", 500}}}});
    config = normalize(config);
    auto s = initial(config, 42);
    account(s, "firm:2").stock["food"] = 10;
    account(s, "household:1").cash = 1000;
    std::vector<Json> decisions;
    Engine e(s, config, {}, [&](auto &j) { decisions.push_back(j); });
    e.observe(e.state.households[0], "firm:2", "visit");
    e.shop();
    REQUIRE(decisions[0]["chosen"] == "firm:2");
    REQUIRE(stock(account(e.state, "household:1").stock, "food") == 1);
    auto board = s;
    board.public_board = true;
    Engine b(board, config, {}, [&](auto &j) { decisions.push_back(j); });
    b.publish();
    decisions.clear();
    b.shop();
    REQUIRE(decisions[0]["chosen"] == "firm:3");
    REQUIRE(decisions[0]["outcome"] == "PURCHASED");
    std::vector<Json> stale_events;
    Engine stale(
        s, config, [&](const Json &event) { stale_events.push_back(event); },
        [&](auto &j) { decisions.push_back(j); });
    stale.observe(stale.state.households[0], "firm:2", "visit");
    firm(stale.state, "firm:2").prices["food"] = 2000;
    auto before = Json(stale.state.accounts);
    decisions.clear();
    stale.shop();
    REQUIRE(decisions[0]["checkout"]["stale_price"] == true);
    REQUIRE(decisions[0]["outcome"] == "BUDGET_LIMIT");
    REQUIRE(decisions[0]["event_ids"].size() == 1);
    REQUIRE(stale_events[0]["type"] == "trade_rejected");
    REQUIRE(stale_events[0]["data"]["reason"] == "BUDGET_LIMIT");
    REQUIRE(Json(stale.state.accounts) == before);
    REQUIRE(stale.state.households[0].knowledge[0].price == 2000);
    // Refresh after a remembered stockout permits a later purchase.
    Engine refresh(s, config);
    account(refresh.state, "firm:2").stock["food"] = 0;
    refresh.observe(refresh.state.households[0], "firm:2", "visit");
    account(refresh.state, "firm:2").stock["food"] = 10;
    refresh.state.next_day = 3;
    refresh.publish();
    REQUIRE(refresh.state.households[0].knowledge[0].stock == 10);
}
static void invalid_scenarios() {
    auto valid = fixture();
    auto bad = valid;
    bad["schema"] = "scenario-9";
    fails([&] { normalize(bad); });
    bad = valid;
    bad["firms"][0]["capacity"] = -1;
    fails([&] { normalize(bad); });
    bad = valid;
    bad["households"][1]["id"] = bad["households"][0]["id"];
    fails([&] { normalize(bad); });
    bad = valid;
    bad["households"][0]["employer"] = "firm:99";
    fails([&] { normalize(bad); });
    bad = valid;
    bad["firms"][0]["prices"]["food"] = 1.5;
    fails([&] { normalize(bad); });
    bad = valid;
    bad["firms"][0]["capacity"] = Json(std::numeric_limits<std::uint64_t>::max());
    fails([&] { normalize(bad); });
    bad = valid;
    bad["typo"] = 1;
    fails([&] { normalize(bad); });
    bad = valid;
    bad["firms"][1]["suppliers"]["food"] = "household:1";
    fails([&] { normalize(bad); });
}
static Json rich_retailer(Json config) {
    // Fund both firms so that payroll runway never moves staffing unless a test makes it.
    config["firms"][0]["cash"] = 60000;
    config["firms"][1]["cash"] = 60000;
    return config;
}
static void labor_arithmetic(const fs::path &root) {
    const auto maximum = std::numeric_limits<Amount>::max();
    REQUIRE(ceil_div(maximum, 3) == maximum / 3 + 1);
    REQUIRE(ceil_div(0, maximum) == 0);
    REQUIRE(mul_capped(maximum, 3, 6) == 6);
    for (auto capacity : {Amount{0}, maximum}) {
        auto config = read_json(fs::path(ECON_SOURCE) / "scenarios/accounting_cycle.json");
        config["firms"][0]["capacity"] = capacity;
        config = normalize(config);
        REQUIRE(normalize(config) == config);
        REQUIRE(integer(config["firms"][0]["output_per_worker"]) > 0);
        for (auto mode : {"fixed", "dynamic"}) {
            RunOptions opt;
            opt.out = root / ("capacity-" + std::to_string(capacity) + "-" + mode);
            opt.until = 1;
            opt.employment = mode;
            opt.checkpoint_every = 1;
            REQUIRE(run(config, 42, opt)["verification"]["passed"] == true);
            REQUIRE(load_checkpoint(opt.out / "checkpoints/day-0001.json").state.next_day == 1);
        }
    }
    auto config = fixture();
    config["firms"][0]["output_per_worker"] = maximum;
    config = normalize(config);
    Engine e(initial(config, 42), config);
    auto metric = e.step();
    REQUIRE(metric["production"]["food"] == 6 && metric["labor_units"]["used"] == 1);
    auto invalid = e.state;
    invalid.firms[0].output_per_worker = 0;
    fails([&] { validate_state(invalid, config); }, 5);
    // Large offer steps clamp at the bound without signed addition overflow.
    config = rich_retailer(fixture());
    config["firms"][0]["positions"] = 4;
    config["firms"][0]["output_per_worker"] = 1;
    config["firms"][0]["hiring"] = {{"wage_step", maximum}, {"wage_min", 0}, {"wage_max", maximum}};
    config = normalize(config);
    auto s = initial(config, 1, "dynamic");
    s.board.push_back({"posting:1", "firm:1", 1, 1000, 0, true, 2, 0, 2, 0, "HIRE_TEST"});
    s.next_posting = 2;
    Engine offer(s, config);
    offer.employment();
    REQUIRE(offer.state.board[0].wage == 1000); // the maximum offer cannot be financed
    // A retailer may offer zero wages. When a raised offer becomes unaffordable it can return to
    // its zero floor, and the persisted verifier must accept that legitimate adjustment.
    config = rich_retailer(fixture());
    config["firms"][1]["cash"] = 10000;
    config["firms"][1]["wage"] = 0;
    config["firms"][1]["positions"] = 4;
    config["firms"][1]["prices"]["food"] = 1;
    config["firms"][1]["hiring"] = {{"wage_step", 200}, {"wage_min", 0}, {"wage_max", 200}};
    RunOptions opt;
    opt.out = root / "zero-offer";
    opt.employment = "dynamic";
    opt.until = 10;
    REQUIRE(run(config, 1, opt)["verification"]["passed"] == true);
    bool lowered_to_zero = false;
    for (auto &event : facts(opt.out))
        if (event["type"] == "posting_adjusted" && event["data"]["field"] == "wage" &&
            event["data"]["new"] == 0)
            lowered_to_zero = true;
    REQUIRE(lowered_to_zero);
}
static void dynamic_basics(const fs::path &root) {
    auto config = fixture();
    // Fixed employment is inert: no employment-phase activity, every scenario job is one open INITIAL spell
    // at the firm's wage, and neither a vacancy board nor pending changes may exist.
    std::vector<Json> events;
    Engine e(initial(config, 42), config, [&](auto &j) { events.push_back(j); });
    e.step();
    REQUIRE(e.state.employment_mode == "fixed");
    REQUIRE(e.state.employment.size() == 6 && e.state.pending_changes.empty() && e.state.board.empty());
    for (auto &r : e.state.employment)
        REQUIRE(r.end == -1 && r.reason == "INITIAL" && r.start == 0 && r.wage == 1000 &&
                r.end_reason.empty());
    for (auto &ev : events)
        REQUIRE(ev.at("phase") != "employment");
    REQUIRE(firm(e.state, "firm:1").demand_history == std::vector<Amount>{6});
    REQUIRE(firm(e.state, "firm:1").output_per_worker == 2 && firm(e.state, "firm:2").output_per_worker == 0);
    REQUIRE(firm(e.state, "firm:1").hiring.wage_min == 1000 &&
            firm(e.state, "firm:1").hiring.wage_max == 1300 &&
            firm(e.state, "firm:1").hiring.wage_step == 100);
    auto bad = e.state;
    bad.pending_changes.push_back({"change:1", "household:1", "firm:1", "", 0, 2, 0, "", "HIRED"});
    bad.next_change = 2;
    fails([&] { validate_state(bad, config); }, 5);
    bad = e.state;
    bad.board.push_back({"posting:1", "firm:1", 1, 1000, 0, true, 0, 0, 0, 0, "X"});
    bad.next_posting = 2;
    fails([&] { validate_state(bad, config); }, 5);
    fails([&] { initial(config, 1, "random"); }, 2);
    RunOptions opt;
    opt.out = root / "bad_mode";
    opt.until = 1;
    opt.employment = "flexible";
    fails([&] { run(config, 1, opt); }, 2);
    for (auto tweak : std::vector<std::function<void(Json &)>>{
             [](Json &j) { j["employment"]["notice_days"] = 0; },
             [](Json &j) { j["employment"]["typo"] = 1; },
             [](Json &j) { j["employment"]["demand_window_days"] = 0; },
             [](Json &j) { j["firms"][0]["output_per_worker"] = 0; },
             [](Json &j) { j["firms"][1]["output_per_worker"] = 3; },
             [](Json &j) { j["firms"][0]["hiring"]["wage_min"] = 1001; },
             [](Json &j) { j["firms"][0]["hiring"]["wage_max"] = 999; },
             [](Json &j) { j["firms"][0]["hiring"]["accept_employed"] = "yes"; },
             [](Json &j) { j["firms"][0]["hiring"]["raise_after_unfilled_days"] = 0; },
             [](Json &j) { j["firms"][0]["hiring"]["typo"] = 1; }}) {
        auto invalid = config;
        tweak(invalid);
        fails([&] { normalize(invalid); }, 2);
    }
    // A dynamic state with a posting whose offer is outside the firm's bounds is rejected.
    auto dynamic = initial(config, 1, "dynamic");
    dynamic.board.push_back({"posting:1", "firm:1", 1, 1400, 0, true, 2, 0, 0, 0, "HIRE_TEST"});
    dynamic.next_posting = 2;
    fails([&] { validate_state(dynamic, config); }, 5);
    dynamic.board[0].wage = 1300;
    validate_state(dynamic, config);
    REQUIRE(experience_days(dynamic, "household:1", 4) == 4);
    REQUIRE(daily_payroll(dynamic, "firm:1") == 3000);
}
static void dynamic_board(const fs::path &) {
    // Hiring through the board: a firm short of its target posts openings at its wage; an unemployed
    // household applies, the opening is consumed and the posting closes as FILLED. The hire only holds
    // the job, works and is paid from the effective day.
    {
        auto hiring = rich_retailer(fixture());
        hiring["households"][3]["employer"] = "";
        hiring["firms"][1]["positions"] = 3;
        hiring = normalize(hiring);
        std::vector<Json> events, decisions;
        Engine e(
            initial(hiring, 1, "dynamic"), hiring, [&](auto &j) { events.push_back(j); },
            [&](auto &j) { decisions.push_back(j); });
        auto day0 = e.step();
        auto opened = of_type(events, "posting_opened");
        REQUIRE(opened.size() == 1 && opened[0]["data"]["firm"] == "firm:2" &&
                opened[0]["data"]["posting"]["openings"] == 1 &&
                opened[0]["data"]["posting"]["wage"] == 1000 &&
                opened[0]["data"]["reason"] == "HIRE_OVERHEAD_STAFFING");
        auto scheduled = of_type(events, "employment_scheduled");
        REQUIRE(scheduled.size() == 1 && scheduled[0]["data"]["household"] == "household:4" &&
                scheduled[0]["data"]["to_employer"] == "firm:2" && scheduled[0]["data"]["effective"] == 1 &&
                scheduled[0]["data"]["wage"] == 1000 && scheduled[0]["data"]["posting"] == "posting:1");
        auto closed = of_type(events, "posting_closed");
        REQUIRE(closed.size() == 1 && closed[0]["data"]["reason"] == "FILLED");
        REQUIRE(e.state.board.empty() && day0["board"].empty() && day0["board_activity"].size() == 2);
        bool searched = false, planned = false;
        for (auto &d : decisions) {
            if (d["policy"] == "job_search-2" && d["constraints"]["status"] == "unemployed") {
                searched = true;
                REQUIRE(d["actor"] == "household:4" && d["outcome"] == "HIRE_SCHEDULED" &&
                        d["chosen"] == "posting:1" && d["constraints"]["experience_days"] == 0 &&
                        d["knowledge"][0]["source"] == "vacancy_board");
            }
            if (d["policy"] == "staffing_plan-1" && d["actor"] == "firm:2") {
                planned = true;
                REQUIRE(d["outcome"] == "ADJUSTED" && d["reason"] == "OVERHEAD_STAFFING" &&
                        d["constraints"]["target"] == 3 && d["alternatives"][0]["action"] == "post_openings");
            }
        }
        REQUIRE(searched && planned);
        auto &person = day0["labor"]["household:4"];
        REQUIRE(person["status"] == "unemployed" && person["worked"] == false && person["wage_paid"] == 0);
        REQUIRE(person["scheduled"]["posting"] == "posting:1" && person["change"].is_null());
        events.clear();
        auto day1 = e.step();
        auto changed = of_type(events, "employment_changed");
        REQUIRE(changed.size() == 1 && changed[0]["data"]["opened_record"] == "employment:6");
        REQUIRE(e.state.employment[5].wage == 1000 && e.state.employment[5].reason == "HIRED");
        REQUIRE(day1["labor"]["household:4"]["worked"] == true &&
                day1["labor"]["household:4"]["wage_paid"] == 1000);
        REQUIRE(integer(day1["paid_workers"]) == 6 && integer(day1["hires"]) == 1);
    }
    // Eligibility and offers: a posting nobody qualifies for stays unfilled, so the firm raises its
    // offer every raise_after_unfilled_days up to wage_max and then records that it is at the maximum.
    // Workers left unpaid (an understaffed producer cannot run) quit to an eligible posting elsewhere,
    // and the wage they agreed is the advertised one at the time, not the firm's base wage.
    {
        auto config = fixture();
        config["firms"][0]["cash"] = 50000;
        config["firms"][0]["labor_required"] = 4;
        config["firms"][0]["positions"] = 4;
        config["firms"][0]["hiring"] = {{"min_experience_days", 100}};
        config["firms"][1]["cash"] = 50000;
        config["firms"][1]["positions"] = 4;
        config["firms"][1]["wage"] = 900;
        config["firms"][1]["hiring"] = Json::object(); // bounds follow the new wage
        config = normalize(config);
        std::vector<Json> events, decisions;
        Engine e(
            initial(config, 1, "dynamic"), config, [&](auto &j) { events.push_back(j); },
            [&](auto &j) { decisions.push_back(j); });
        for (int d = 0; d < 3; ++d)
            e.step();
        auto adjusted = of_type(events, "posting_adjusted");
        REQUIRE(adjusted.size() == 2);
        REQUIRE(adjusted[0]["day"] == 2 && adjusted[0]["data"]["firm"] == "firm:1" &&
                adjusted[0]["data"]["field"] == "wage" && adjusted[0]["data"]["old"] == 1000 &&
                adjusted[0]["data"]["new"] == 1100 && adjusted[0]["data"]["reason"] == "UNFILLED");
        REQUIRE(adjusted[1]["data"]["firm"] == "firm:2" && adjusted[1]["data"]["new"] == 990);
        REQUIRE(of_type(events, "employment_scheduled").empty());
        events.clear();
        decisions.clear();
        auto day3 = e.step();
        auto scheduled = of_type(events, "employment_scheduled");
        REQUIRE(scheduled.size() == 1 && scheduled[0]["data"]["from_employer"] == "firm:1" &&
                scheduled[0]["data"]["to_employer"] == "firm:2" &&
                scheduled[0]["data"]["reason"] == "QUIT_UNPAID" && scheduled[0]["data"]["wage"] == 990 &&
                scheduled[0]["data"]["posting"] == "posting:2");
        int searches = 0, staying = 0;
        for (auto &d : decisions)
            if (d["policy"] == "job_search-2" && d["constraints"]["status"] == "employed_unpaid") {
                ++searches;
                REQUIRE(d["constraints"]["unpaid_days"] == 3 && d["constraints"]["current_net_wage"] == 0);
                bool own_firm = false;
                for (auto &x : d["exclusions"])
                    if (x["posting"] == "posting:1" && x["reason"] == "CURRENT_EMPLOYER" &&
                        x["stage"] == "eligibility")
                        own_firm = true;
                REQUIRE(own_firm);
                if (d["outcome"] == "INELIGIBLE") {
                    ++staying;
                    REQUIRE(d["reason"] == "NO_ELIGIBLE_OFFER");
                }
            }
        REQUIRE(searches == 3 && staying == 2);
        REQUIRE(integer(day3["job_search"]["quits"]) == 1 && integer(day3["job_search"]["applied"]) == 1);
        auto who = scheduled[0]["data"]["household"].get<std::string>();
        REQUIRE(day3["labor"][who]["scheduled"]["wage"] == 990);
        events.clear();
        auto day4 = e.step();
        auto changed = of_type(events, "employment_changed");
        REQUIRE(changed.size() == 1 && changed[0]["data"]["opened_record"] == "employment:7");
        REQUIRE(day4["labor"][who]["employer"] == "firm:2" && day4["labor"][who]["wage_paid"] == 990);
        REQUIRE(daily_payroll(e.state, "firm:2") == 3690);
        // The vacated seat widens firm:1's posting, whose offer keeps climbing to the bound.
        adjusted = of_type(events, "posting_adjusted");
        REQUIRE(adjusted.size() == 2 && adjusted[0]["data"]["field"] == "openings" &&
                adjusted[0]["data"]["new"] == 2 &&
                adjusted[0]["data"]["reason"] == "STAFFING_TARGET_RAISED" &&
                adjusted[1]["data"]["new"] == 1200);
        decisions.clear();
        for (int d = 0; d < 4; ++d)
            e.step();
        REQUIRE(e.state.board.size() == 1 && e.state.board[0].wage == 1300);
        bool at_maximum = false;
        for (auto &d : decisions)
            if (d["policy"] == "staffing_plan-1" && d["actor"] == "firm:1" && d["day"] == 8)
                for (auto &x : d["exclusions"])
                    if (x["action"] == "raise_offer" && x["reason"] == "OFFER_AT_MAXIMUM")
                        at_maximum = true;
        REQUIRE(at_maximum);
    }
    // Requirements: employed applicants can be refused, and a firm will not rehire someone it laid
    // off until the cooldown has passed.
    {
        auto config = rich_retailer(fixture());
        config["firms"][0]["cash"] = 50000;
        config["firms"][0]["labor_required"] = 4;
        config["firms"][0]["positions"] = 4;
        config["firms"][0]["hiring"] = {{"accept_employed", false}};
        config["firms"][1]["positions"] = 4;
        config["firms"][1]["wage"] = 900;
        config["firms"][1]["hiring"] = {{"accept_employed", false}};
        config = normalize(config);
        std::vector<Json> decisions;
        Engine e(initial(config, 1, "dynamic"), config, {}, [&](auto &j) { decisions.push_back(j); });
        for (int d = 0; d < 4; ++d)
            e.step();
        int refused = 0;
        for (auto &d : decisions)
            if (d["policy"] == "job_search-2" && d["day"] == 3 &&
                d["constraints"]["status"] == "employed_unpaid") {
                REQUIRE(d["outcome"] == "INELIGIBLE");
                for (auto &x : d["exclusions"])
                    if (x["reason"] == "NOT_ELIGIBLE_EMPLOYED")
                        ++refused;
            }
        REQUIRE(refused == 3);
        auto cooldown = rich_retailer(fixture());
        cooldown["firms"][0]["cash"] =
            2999; // cannot carry its crew: laid off, then re-posts once cash recovers
        cooldown["firms"][0]["prices"]["food"] = 1500;
        cooldown["firms"][0]["stock"] = {{"food", 12}};
        cooldown["firms"][0]["hiring"] = {{"rehire_cooldown_days", 30}};
        cooldown = normalize(cooldown);
        std::vector<Json> events;
        decisions.clear();
        Engine c(
            initial(cooldown, 1, "dynamic"), cooldown, [&](auto &j) { events.push_back(j); },
            [&](auto &j) { decisions.push_back(j); });
        for (int d = 0; d < 4; ++d)
            c.step();
        REQUIRE(of_type(events, "employment_changed").size() == 3);
        for (auto &r : c.state.employment)
            if (r.employer == "firm:1")
                REQUIRE(r.end == 1 && r.end_reason == "LAYOFF_PAYROLL_RUNWAY");
        REQUIRE(!of_type(events, "posting_opened").empty());
        int cooled = 0;
        for (auto &d : decisions)
            if (d["policy"] == "job_search-2")
                for (auto &x : d["exclusions"])
                    if (x["reason"] == "REHIRE_COOLDOWN")
                        ++cooled;
        REQUIRE(cooled > 0);
        REQUIRE(c.state.households[0].employer.empty());
    }
}
static void dynamic_staffing(const fs::path &) {
    // Payroll runway: a producer that cannot carry its minimum crew for payroll_runway_days is
    // mothballed with notice; the workers stay employed, idle and unpaid until the effective day.
    {
        auto thin = rich_retailer(fixture());
        thin["firms"][0]["cash"] = 2999;
        thin = normalize(thin);
        std::vector<Json> events, decisions;
        Engine e(
            initial(thin, 1, "dynamic"), thin, [&](auto &j) { events.push_back(j); },
            [&](auto &j) { decisions.push_back(j); });
        auto day0 = e.step();
        auto scheduled = of_type(events, "employment_scheduled");
        REQUIRE(scheduled.size() == 3);
        for (auto &s : scheduled)
            REQUIRE(s["data"]["reason"] == "LAYOFF_PAYROLL_RUNWAY" && s["data"]["effective"] == 1);
        REQUIRE(scheduled[0]["data"]["household"] == "household:3"); // newest spell first
        for (auto &d : decisions)
            if (d["policy"] == "staffing_plan-1" && d["actor"] == "firm:1")
                REQUIRE(d["reason"] == "PAYROLL_RUNWAY" && d["constraints"]["target"] == 0 &&
                        d["constraints"]["affordable_workers"] == 0 &&
                        d["constraints"]["unconstrained_target"] == 3 && d["alternatives"].size() == 3 &&
                        d["event_ids"].size() == 3);
        REQUIRE(day0["labor"]["household:3"]["status"] == "employed" &&
                day0["labor"]["household:3"]["idle_reason"] == "INSUFFICIENT_PAYROLL" &&
                day0["labor"]["household:3"]["scheduled"]["reason"] == "LAYOFF_PAYROLL_RUNWAY");
        REQUIRE(of_type(events, "posting_opened").empty());
        events.clear();
        auto day1 = e.step();
        REQUIRE(of_type(events, "employment_changed").size() == 3);
        REQUIRE(integer(day1["separations"]) == 3 && integer(day1["employed"]) == 3);
        REQUIRE(e.state.households[2].employer.empty() && e.state.employment[2].end == 1);
    }
    // Cancellation: a layoff under notice is withdrawn when the target recovers before it takes effect.
    {
        auto recover = rich_retailer(fixture());
        recover["employment"] = {{"notice_days", 2}};
        recover["firms"][0]["cash"] = 8999;
        recover["firms"][0]["prices"]["food"] = 600;
        recover = normalize(recover);
        std::vector<Json> events;
        Engine e(initial(recover, 1, "dynamic"), recover, [&](auto &j) { events.push_back(j); });
        e.step();
        REQUIRE(of_type(events, "employment_scheduled").size() == 3 && e.state.pending_changes.size() == 3);
        events.clear();
        auto day1 = e.step();
        auto cancelled = of_type(events, "employment_change_cancelled");
        REQUIRE(cancelled.size() == 3 && cancelled[0]["data"]["reason"] == "STAFFING_TARGET_RECOVERED" &&
                cancelled[0]["data"]["household"] == "household:1"); // latest scheduled first
        REQUIRE(e.state.pending_changes.empty());
        REQUIRE(day1["labor"]["household:1"]["cancelled"]["reason"] == "STAFFING_TARGET_RECOVERED");
        REQUIRE(integer(day1["paid_workers"]) == 6);
        e.step();
        REQUIRE(of_type(events, "employment_changed").empty());
        for (auto &h : e.state.households)
            REQUIRE(!h.employer.empty());
    }
    // Demand-driven targets with per-worker output: once the window is full the crew follows observed
    // demand and inventory cover. Surplus stock trims the crew to the minimum (LAYOFF_INVENTORY_SURPLUS);
    // a demand-free window idles the plant (LAYOFF_NO_DEMAND); empty shelves ask for restocking hires.
    {
        auto base = rich_retailer(fixture());
        base["employment"] = {{"demand_window_days", 2}, {"inventory_cover_days", 2}};
        base["firms"][0]["cash"] = 100000;
        base["firms"][0]["labor_required"] = 1;
        base["firms"][0]["positions"] = 6;
        base["firms"][0]["output_per_worker"] = 1;
        auto restock = normalize(base);
        std::vector<Json> events, decisions;
        Engine e(
            initial(restock, 1, "dynamic"), restock, [&](auto &j) { events.push_back(j); },
            [&](auto &j) { decisions.push_back(j); });
        auto day0 = e.step();
        REQUIRE(integer(day0["production"]["food"]) == 3);   // 3 workers x 1 unit, below capacity 6
        REQUIRE(integer(day0["demand"]["firm:1"]) == 3 + 3); // 3 sold plus a 3-unit supply shortfall
        REQUIRE(of_type(events, "supply_shortfall").size() == 1 &&
                of_type(events, "supply_shortfall")[0]["data"]["shortfall"] == 3);
        auto opened = of_type(events, "posting_opened");
        REQUIRE(opened.size() == 1 && opened[0]["data"]["reason"] == "HIRE_INSUFFICIENT_HISTORY" &&
                opened[0]["data"]["posting"]["openings"] == 3); // short history: staff for capacity
        e.step();
        events.clear();
        decisions.clear();
        e.step();
        bool planned = false;
        for (auto &d : decisions)
            if (d["policy"] == "staffing_plan-1" && d["actor"] == "firm:1") {
                planned = true;
                REQUIRE(d["reason"] == "RESTOCKING" && d["constraints"]["demand_per_day"] == 6 &&
                        d["constraints"]["on_hand"] == 0 && d["constraints"]["wanted_output"] == 12 &&
                        d["constraints"]["desired_output"] == 6 && d["constraints"]["max_workers"] == 6 &&
                        d["constraints"]["target"] == 6);
            }
        // Nobody is free to hire, so the posting stays open and its offer has started climbing.
        REQUIRE(planned && e.state.board.size() == 1 && e.state.board[0].openings == 3 &&
                e.state.board[0].wage == 1100);
        auto surplus = base;
        surplus["firms"][0]["stock"] = {{"food", 60}};
        surplus = normalize(surplus);
        events.clear();
        decisions.clear();
        Engine s(
            initial(surplus, 1, "dynamic"), surplus, [&](auto &j) { events.push_back(j); },
            [&](auto &j) { decisions.push_back(j); });
        for (int d = 0; d < 3; ++d)
            s.step();
        auto layoffs = of_type(events, "employment_scheduled");
        REQUIRE(layoffs.size() == 2 && layoffs[0]["day"] == 2 &&
                layoffs[0]["data"]["reason"] == "LAYOFF_INVENTORY_SURPLUS");
        for (auto &d : decisions)
            if (d["policy"] == "staffing_plan-1" && d["actor"] == "firm:1" && d["day"] == 2)
                REQUIRE(d["reason"] == "INVENTORY_SURPLUS" && d["constraints"]["target"] == 1 &&
                        d["constraints"]["desired_output"] == 0);
        auto idle = base;
        idle["firms"][1]["targets"] = Json::object(); // the retailer never buys: no demand observed
        idle["firms"][1]["suppliers"] = Json::object();
        idle = normalize(idle);
        events.clear();
        Engine n(initial(idle, 1, "dynamic"), idle, [&](auto &j) { events.push_back(j); });
        for (int d = 0; d < 3; ++d)
            n.step();
        layoffs = of_type(events, "employment_scheduled");
        REQUIRE(layoffs.size() == 3 && layoffs[0]["day"] == 2 &&
                layoffs[0]["data"]["reason"] == "LAYOFF_NO_DEMAND");
        REQUIRE((firm(n.state, "firm:1").demand_history == std::vector<Amount>{0, 0}));
    }
    // Expensive contracts reduce the affordable target, rather than advertising hires against the
    // cheaper base wage. Incumbents continue receiving their agreed wage.
    {
        auto tight = rich_retailer(fixture());
        tight["firms"][0]["cash"] = 15000;
        tight["firms"][0]["prices"]["food"] = 1200; // revenue just above payroll keeps cash flat
        tight["firms"][0]["output_per_worker"] = 1;
        tight["firms"][0]["positions"] = 5;
        tight["firms"][0]["hiring"] = {{"wage_min", 800}};
        tight = normalize(tight);
        auto s = initial(tight, 1, "dynamic");
        s.employment[0].wage = 1500; // an incumbent on an expensive contract
        std::vector<Json> events, decisions;
        Engine e(s, tight, [&](auto &j) { events.push_back(j); }, [&](auto &j) { decisions.push_back(j); });
        for (int d = 0; d < 3; ++d)
            e.step();
        REQUIRE(of_type(events, "posting_opened").size() == 1);
        auto adjusted = of_type(events, "posting_adjusted");
        REQUIRE(adjusted.size() == 1 && adjusted[0]["day"] == 2 && adjusted[0]["data"]["new"] == 1100);
        REQUIRE(e.state.board.size() == 1 && e.state.board[0].openings == 1);
        REQUIRE(decisions[0]["constraints"]["target"] == 4 && decisions[0]["reason"] == "PAYROLL_RUNWAY");
        // The expensive incumbent is paid the agreed wage, and payroll is the sum of agreed wages.
        int expensive = 0;
        for (auto &w : of_type(events, "wage"))
            if (w["data"]["household"] == "household:1") {
                REQUIRE(w["data"]["amount"] == 1500);
                ++expensive;
            }
        REQUIRE(expensive == 3);
    }
    // A raised posting may need both a lower wage and fewer openings. Lowering one step must not
    // leave unfunded openings available for applicants to accept.
    {
        auto tight = rich_retailer(fixture());
        tight["firms"][0]["cash"] = 15000;
        tight["firms"][0]["output_per_worker"] = 1;
        tight["firms"][0]["positions"] = 5;
        tight["households"][3]["employer"] = "";
        tight = normalize(tight);
        auto s = initial(tight, 1, "dynamic");
        s.board.push_back({"posting:1", "firm:1", 2, 1300, 0, true, 2, 0, 2, 0, "HIRE_TEST"});
        s.next_posting = 2;
        std::vector<Json> events;
        Engine e(s, tight, [&](auto &j) { events.push_back(j); });
        e.employment();
        auto adjusted = of_type(events, "posting_adjusted");
        REQUIRE(adjusted.size() == 2 && adjusted[0]["data"]["new"] == 1200 &&
                adjusted[1]["data"]["field"] == "openings" && adjusted[1]["data"]["new"] == 1);
        REQUIRE(e.state.pending_changes.size() == 1 && e.state.pending_changes[0].wage == 1200);
        REQUIRE((daily_payroll(e.state, "firm:1") + e.state.pending_changes[0].wage) * 3 <= 15000);
    }
    // Retention itself must use negotiated wages: at 1,500 per contract only two workers fit this
    // runway. The minimum crew is three, so the plant schedules layoffs instead of an unfunded hold.
    {
        auto tight = rich_retailer(fixture());
        tight["firms"][0]["cash"] = 9000;
        tight = normalize(tight);
        auto s = initial(tight, 1, "dynamic");
        for (auto &r : s.employment)
            if (r.employer == "firm:1")
                r.wage = 1500;
        Engine e(s, tight);
        e.employment();
        REQUIRE(e.state.pending_changes.size() == 3);
        for (auto &c : e.state.pending_changes)
            REQUIRE(c.reason == "LAYOFF_PAYROLL_RUNWAY");
        // After notice, an empty inactive plant no longer accumulates a payroll-miss streak.
        e.state.firms[0].payroll_missed_days = 2;
        e.step();
        e.step();
        REQUIRE(e.state.firms[0].payroll_missed_days == 0);
    }
}
static void household_preferences(const fs::path &) {
    // Both the employed and the unemployed read the board; a household's own rules decide whether an
    // eligible offer is preferable, and the decision records the rule that said no.
    auto base = rich_retailer(fixture());
    base["firms"][1]["positions"] = 4; // one opening at the retailer
    base["firms"][1]["wage"] = 1200;
    base["firms"][1]["hiring"] = {{"wage_step", 0}}; // a steady offer, so wages in assertions are exact
    for (auto &h : base["households"])
        h.erase("job_preferences"); // let scenario-level rules apply unless a test sets a household's own
    auto switching = normalize(base);
    {
        std::vector<Json> events, decisions;
        Engine e(
            initial(switching, 1, "dynamic"), switching, [&](auto &j) { events.push_back(j); },
            [&](auto &j) { decisions.push_back(j); });
        for (int d = 0; d < 5; ++d)
            e.step();
        // Paid workers at firm:1 see a 20% better offer but hold until min_tenure_days (5) have passed.
        int held = 0;
        for (auto &d : decisions)
            if (d["policy"] == "job_search-2" && d["constraints"]["status"] == "employed" &&
                d["actor"] == "household:1") {
                ++held;
                REQUIRE(d["outcome"] == "DECLINED" && d["reason"] == "NO_PREFERABLE_OFFER");
                REQUIRE(d["exclusions"][0]["stage"] == "preference" &&
                        d["exclusions"][0]["reason"] == "TENURE_TOO_SHORT");
                REQUIRE(d["constraints"]["current_wage"] == 1000 &&
                        d["constraints"]["preferences"]["min_tenure_days"] == 5);
            }
        REQUIRE(held == 5 && of_type(events, "employment_scheduled").empty());
        events.clear();
        decisions.clear();
        auto day5 = e.step();
        auto scheduled = of_type(events, "employment_scheduled");
        REQUIRE(scheduled.size() == 1 && scheduled[0]["data"]["reason"] == "SWITCHED" &&
                scheduled[0]["data"]["from_employer"] == "firm:1" &&
                scheduled[0]["data"]["to_employer"] == "firm:2" && scheduled[0]["data"]["wage"] == 1200 &&
                scheduled[0]["data"]["effective"] == 6);
        auto who = scheduled[0]["data"]["household"].get<std::string>();
        auto &js = day5["job_search"];
        REQUIRE(integer(js["switches"]) == 1 && integer(js["applied"]) == 1 &&
                integer(js["searching"]) == 6 && integer(js["ineligible"]) == 5 &&
                integer(js["ineligible_reasons"]["NO_OPENINGS"]) == 2 &&
                integer(js["ineligible_reasons"]["CURRENT_EMPLOYER"]) == 3 && integer(js["declined"]) == 0);
        REQUIRE(day5["labor"][who]["search"]["outcome"] == "SWITCH_SCHEDULED" &&
                day5["labor"][who]["search"]["chosen"] == "posting:1");
        for (auto &d : decisions)
            if (d["policy"] == "job_search-2" && d["actor"] == who)
                REQUIRE(d["alternatives"][0]["gain"] == 200 && d["reason"] == "HIGHEST_NET_WAGE");
        events.clear();
        auto day6 = e.step();
        REQUIRE(of_type(events, "employment_changed").size() == 1);
        REQUIRE(day6["labor"][who]["employer"] == "firm:2" && day6["labor"][who]["wage_paid"] == 1200 &&
                day6["labor"][who]["change"]["reason"] == "SWITCHED");
        REQUIRE(integer(day6["hires"]) == 1 && integer(day6["separations"]) == 1);
        REQUIRE(!of_type(events, "posting_opened").empty()); // firm:1 backfills the vacated seat
    }
    // Gain thresholds: a relative or absolute minimum gain turns the same offer down.
    for (auto rule :
         std::vector<std::pair<std::string, Amount>>{{"min_wage_gain_percent", 30}, {"min_wage_gain", 300}}) {
        auto picky = base;
        picky["employment"]["job_preferences"] = {{"min_tenure_days", 0}, {rule.first, rule.second}};
        picky = normalize(picky);
        std::vector<Json> decisions;
        Engine e(initial(picky, 1, "dynamic"), picky, {}, [&](auto &j) { decisions.push_back(j); });
        e.step();
        int declined = 0;
        for (auto &d : decisions)
            if (d["policy"] == "job_search-2" && d["constraints"]["employer"] == "firm:1") {
                ++declined;
                REQUIRE(d["outcome"] == "DECLINED" &&
                        d["exclusions"][0]["reason"] == "INSUFFICIENT_WAGE_GAIN");
            }
        REQUIRE(declined == 3 && e.state.pending_changes.empty());
    }
    // Commute: the offer is judged net of travel time valued at the household's time_value.
    {
        auto far = base;
        far["employment"]["job_preferences"] = {{"min_tenure_days", 0}};
        far["firms"][1]["travel_minutes"] = 50;
        for (auto &h : far["households"])
            h["time_value"] = 5;
        far = normalize(far);
        std::vector<Json> decisions;
        Engine e(initial(far, 1, "dynamic"), far, {}, [&](auto &j) { decisions.push_back(j); });
        e.step();
        int judged = 0;
        for (auto &d : decisions)
            if (d["policy"] == "job_search-2" && d["constraints"]["employer"] == "firm:1") {
                ++judged;
                REQUIRE(d["knowledge"][0]["net_wage"] == 950 && d["knowledge"][0]["commute_minutes"] == 50);
                REQUIRE(d["exclusions"][0]["reason"] == "INSUFFICIENT_WAGE_GAIN" &&
                        d["exclusions"][0]["net_wage"] == 950);
            }
        REQUIRE(judged == 3 && e.state.pending_changes.empty());
    }
    // Loyalty: a household that does not search while paid records no search at all.
    {
        auto loyal = base;
        loyal["employment"]["job_preferences"] = {{"min_tenure_days", 0}};
        loyal["households"][0]["job_preferences"] = {{"search_while_employed", false}};
        loyal = normalize(loyal);
        std::vector<Json> decisions;
        Engine e(initial(loyal, 1, "dynamic"), loyal, {}, [&](auto &j) { decisions.push_back(j); });
        auto day0 = e.step();
        for (auto &d : decisions)
            REQUIRE(!(d["policy"] == "job_search-2" && d["actor"] == "household:1"));
        REQUIRE(day0["labor"]["household:1"]["search"].is_null() &&
                integer(day0["job_search"]["searching"]) == 5);
        REQUIRE(day0["labor"]["household:2"]["search"]["outcome"] == "SWITCH_SCHEDULED");
    }
    // Reservation wage: the unemployed decline offers below it and take the offer once it is raised.
    {
        auto reserved = base;
        reserved["households"][3]["employer"] = "";
        reserved["households"][3]["job_preferences"] = {{"reservation_wage", 1300}};
        reserved["employment"]["job_preferences"] = {{"search_while_employed", false}};
        reserved["firms"][1]["hiring"] = Json::object(); // default raises: +120 after two unfilled days
        reserved = normalize(reserved);
        std::vector<Json> events, decisions;
        Engine e(
            initial(reserved, 1, "dynamic"), reserved, [&](auto &j) { events.push_back(j); },
            [&](auto &j) { decisions.push_back(j); });
        auto day0 = e.step();
        REQUIRE(day0["labor"]["household:4"]["search"]["outcome"] == "DECLINED");
        REQUIRE(integer(day0["job_search"]["declined_reasons"]["BELOW_RESERVATION_WAGE"]) == 1 &&
                integer(day0["job_search"]["searching"]) == 1);
        e.step();
        events.clear();
        auto day2 = e.step(); // the offer is raised to 1,320 after two unfilled days and is now acceptable
        REQUIRE(of_type(events, "posting_adjusted").size() == 1 &&
                of_type(events, "posting_adjusted")[0]["data"]["new"] == 1320);
        auto scheduled = of_type(events, "employment_scheduled");
        REQUIRE(scheduled.size() == 1 && scheduled[0]["data"]["household"] == "household:4" &&
                scheduled[0]["data"]["wage"] == 1320 && scheduled[0]["data"]["effective"] == 3);
        REQUIRE(day2["labor"]["household:4"]["search"]["outcome"] == "HIRE_SCHEDULED" &&
                integer(day2["job_search"]["hires"]) == 1);
        REQUIRE(e.state.board.empty() || e.state.board[0].applications == 1);
    }
    // Unpaid tolerance is per household: a worker with tolerance 1 leaves after one unpaid day.
    {
        auto impatient = rich_retailer(fixture());
        impatient["firms"][0]["labor_required"] = 4; // firm:1 cannot run: its workers go unpaid
        impatient["firms"][0]["positions"] = 4;
        impatient["firms"][0]["hiring"] = {{"min_experience_days", 100}};
        impatient["firms"][1]["positions"] = 4;
        impatient["households"][0]["job_preferences"] = {{"unpaid_tolerance_days", 1}};
        impatient = normalize(impatient);
        std::vector<Json> events;
        Engine e(initial(impatient, 1, "dynamic"), impatient, [&](auto &j) { events.push_back(j); });
        e.step();
        events.clear();
        e.step();
        auto scheduled = of_type(events, "employment_scheduled");
        REQUIRE(scheduled.size() == 1 && scheduled[0]["data"]["household"] == "household:1" &&
                scheduled[0]["data"]["reason"] == "QUIT_UNPAID" && scheduled[0]["data"]["effective"] == 2);
    }
    // Validation of preference rules.
    for (auto tweak : std::vector<std::function<void(Json &)>>{
             [](Json &j) { j["employment"]["job_preferences"]["unpaid_tolerance_days"] = 0; },
             [](Json &j) { j["employment"]["job_preferences"]["typo"] = 1; },
             [](Json &j) { j["households"][0]["job_preferences"]["search_while_employed"] = 1; },
             [](Json &j) { j["households"][0]["job_preferences"]["reservation_wage"] = -1; }}) {
        auto invalid = base;
        tweak(invalid);
        fails([&] { normalize(invalid); }, 2);
    }
    auto merged = normalize(base);
    REQUIRE(merged["households"][0]["job_preferences"]["min_tenure_days"] == 5 &&
            merged["employment"]["job_preferences"]["search_while_employed"] == true);
}
static void dynamic_persistence(const fs::path &root) {
    // A dynamic run verifies, checkpoints carry pending changes and the board, and resuming from one
    // reproduces the straight run exactly. Tampering with a labor record or the board is detected.
    auto scenario = normalize(read_json(fs::path(ECON_SOURCE) / "scenarios/labor_market.json"));
    RunOptions opt;
    opt.out = root / "dynamic";
    opt.until = 20;
    opt.checkpoint_every = 1;
    opt.employment = "dynamic";
    auto straight = run(scenario, 7, opt);
    REQUIRE(straight["verification"]["passed"] == true);
    REQUIRE(read_json(opt.out / "manifest.json")["employment"] == "dynamic");
    std::optional<fs::path> with_pending, with_board;
    for (auto &entry : fs::directory_iterator(opt.out / "checkpoints")) {
        auto cp = load_checkpoint(entry.path());
        if (!cp.state.pending_changes.empty() && !with_pending)
            with_pending = entry.path();
        if (!cp.state.board.empty() && !with_board)
            with_board = entry.path();
    }
    REQUIRE(with_pending.has_value() && with_board.has_value());
    std::set<fs::path> checkpoints = {*with_pending, *with_board};
    for (auto cp : checkpoints) {
        auto loaded = load_checkpoint(cp);
        REQUIRE(loaded.state.employment_mode == "dynamic");
        opt.out = root / ("dynamic_resumed_" + cp.stem().string());
        opt.loaded = loaded;
        opt.employment = "fixed"; // ignored: the checkpoint decides
        auto resumed = run(scenario, 7, opt);
        REQUIRE(resumed["state_digest"] == straight["state_digest"]);
        REQUIRE(read_json(opt.out / "manifest.json")["employment"] == "dynamic");
        REQUIRE(facts(opt.out) == facts(root / "dynamic", loaded.state.next_day));
    }
    auto events = facts(root / "dynamic");
    std::set<std::string> reasons;
    for (auto &e : events)
        if (e["type"] == "posting_adjusted" || e["type"] == "posting_closed" ||
            e["type"] == "employment_scheduled")
            reasons.insert(e["data"]["reason"].get<std::string>());
    for (auto needed : {"UNFILLED", "FILLED", "HIRED", "SWITCHED", "QUIT_UNPAID", "LAYOFF_PAYROLL_RUNWAY"})
        REQUIRE(reasons.contains(needed));
    // Every household-initiated change starts the next day; layoffs follow the notice period.
    for (auto &e : events)
        if (e["type"] == "employment_scheduled") {
            auto lead = integer(e["data"]["effective"]) - integer(e["day"]);
            REQUIRE(lead == 1);
        }
    std::set<std::string> exclusions;
    scan_lines(root / "dynamic/decisions.jsonl", [&](const Json &d) {
        if (d.at("policy") == "job_search-2")
            for (auto &x : d.at("exclusions"))
                exclusions.insert(x.at("reason").get<std::string>());
    });
    for (auto needed : {"NOT_ELIGIBLE_EXPERIENCE", "REHIRE_COOLDOWN", "NO_OPENINGS", "BELOW_RESERVATION_WAGE",
                        "TENURE_TOO_SHORT", "INSUFFICIENT_WAGE_GAIN"})
        REQUIRE(exclusions.contains(needed));
    auto inspection = inspect(root / "dynamic", "firm:2");
    REQUIRE(inspection["employment_mode"] == "dynamic" && inspection["employment"].size() >= 2 &&
            inspection.contains("board"));
    auto fixed_out = root / "fixed_same_scenario";
    opt.loaded.reset();
    opt.out = fixed_out;
    opt.employment = "fixed";
    auto fixed_run = run(scenario, 7, opt);
    REQUIRE(fixed_run["state_digest"] != straight["state_digest"]);
    REQUIRE(of_type(facts(fixed_out), "employment_changed").empty() &&
            of_type(facts(fixed_out), "posting_opened").empty());
    auto paired = compare(fixed_out, root / "dynamic", 0, 20);
    REQUIRE(paired["comparison_kind"] == "employment_modes" && paired["employment"]["baseline"] == "fixed" &&
            paired["employment"]["variant"] == "dynamic");
    REQUIRE(paired["baseline"]["periods"] == 20 && paired["variant"]["periods"] == 20);
    REQUIRE(paired["baseline"]["hires"] == 0 && integer(paired["variant"]["hires"]) > 0);
    REQUIRE(integer(paired["baseline"]["unemployed_person_days"]) == 40);
    Amount applied = 0;
    scan_lines(root / "dynamic/metrics.jsonl",
               [&](const Json &m) { applied += integer(m["job_search"]["applied"]); });
    REQUIRE(paired["variant"]["job_search"]["applied"] == applied);
    REQUIRE(compare(fixed_out, root / "dynamic", 5, 15)["variant"]["periods"] == 10);
    fails([&] { compare(fixed_out, root / ("dynamic_resumed_" + with_pending->stem().string()), 10, 20); },
          4);
    // Manifest mode labels are verified against the states, rather than trusted in comparisons.
    auto manifest_path = root / "dynamic/manifest.json";
    auto original_manifest = read_json(manifest_path), wrong_mode = original_manifest;
    wrong_mode["employment"] = "fixed";
    write_json(manifest_path, wrong_mode);
    REQUIRE(verify(root / "dynamic")["passed"] == false);
    write_json(manifest_path, original_manifest);
    opt.out = root / "dynamic-summary";
    opt.employment = "dynamic";
    opt.trace = "summary";
    REQUIRE(run(scenario, 7, opt)["state_digest"] == straight["state_digest"]);
    REQUIRE(facts(opt.out) == events);
    opt.trace = "audit";
    opt.loaded = load_checkpoint(*with_pending);
    opt.out = root / "dynamic-noop";
    opt.intervention = {{"schema", "intervention-1"}, {"day", opt.loaded->state.next_day}, {"type", "noop"}};
    REQUIRE(run(scenario, 7, opt)["state_digest"] == straight["state_digest"]);
    REQUIRE(facts(opt.out) == facts(root / "dynamic", opt.loaded->state.next_day));
    opt.out = root / "dynamic-board";
    (*opt.intervention)["type"] = "public_price_board";
    REQUIRE(run(scenario, 7, opt)["verification"]["passed"] == true);
    REQUIRE(read_json(opt.out / "manifest.json")["employment"] == "dynamic");
    auto tamper = [&](const std::function<void(Json &)> &edit, const std::string &failing_check) {
        auto metrics_path = root / "dynamic/metrics.jsonl";
        auto contents = bytes(metrics_path);
        auto newline = contents.find('\n');
        auto first = Json::parse(contents.substr(0, newline));
        edit(first);
        {
            std::ofstream out(metrics_path);
            out << first.dump() << contents.substr(newline);
        }
        auto report = verify(root / "dynamic");
        REQUIRE(report["passed"] == false);
        bool failed = false;
        for (auto &c : report["checks"])
            if (c["check"] == failing_check && c["status"] == "failed")
                failed = true;
        REQUIRE(failed);
        {
            std::ofstream out(metrics_path);
            out << contents;
        }
    };
    tamper([](Json &m) { m["labor"]["household:1"]["worked"] = false; },
           "per_person_labor_records_reconcile");
    tamper([](Json &m) { m["board"][0]["wage"] = 1; }, "per_person_labor_records_reconcile");
    tamper([](Json &m) { m["demand"]["firm:1"] = 999; }, "per_person_labor_records_reconcile");
    tamper([](Json &m) { m["job_search"]["hires"] = 9; }, "per_person_labor_records_reconcile");
    tamper([](Json &m) { m["job_search"]["searching"] = 99; }, "job_search_metrics_reconcile");
    tamper([](Json &m) { m["job_search"]["declined_reasons"]["TENURE_TOO_SHORT"] = 42; },
           "job_search_metrics_reconcile");
    tamper([](Json &m) { m["labor"]["household:12"]["search"]["outcome"] = "HIRE_SCHEDULED"; },
           "job_search_metrics_reconcile");
    REQUIRE(verify(root / "dynamic")["passed"] == true);
    // A self-consistent search record naming a different posting still cannot disagree with the
    // employment promise in the ledger.
    auto decision_path = root / "dynamic/decisions.jsonl", metric_path = root / "dynamic/metrics.jsonl";
    auto decision_log = bytes(decision_path), metric_log = bytes(metric_path);
    std::vector<Json> decisions, metrics;
    scan_lines(decision_path, [&](const Json &d) { decisions.push_back(d); });
    scan_lines(metric_path, [&](const Json &m) { metrics.push_back(m); });
    bool edited_search = false;
    for (auto &d : decisions)
        if (d["policy"] == "job_search-2" && d["outcome"] == "HIRE_SCHEDULED") {
            d["chosen"] = "posting:99999";
            metrics[static_cast<std::size_t>(integer(d["day"]))]["labor"][d["actor"].get<std::string>()]
                   ["search"]["chosen"] = d["chosen"];
            edited_search = true;
            break;
        }
    REQUIRE(edited_search);
    {
        std::ofstream out(decision_path), met(metric_path);
        for (auto &d : decisions)
            out << d.dump() << '\n';
        for (auto &m : metrics)
            met << m.dump() << '\n';
    }
    REQUIRE(verify(root / "dynamic")["passed"] == false);
    {
        std::ofstream out(decision_path), met(metric_path);
        out << decision_log;
        met << metric_log;
    }
    // An event log claiming a hire at a wage the board never advertised is rejected.
    auto events_path = root / "dynamic/events.jsonl";
    auto log = bytes(events_path);
    auto pos = log.find("\"reason\":\"HIRED\"");
    REQUIRE(pos != std::string::npos);
    auto line_start = log.rfind('\n', pos) + 1, line_end = log.find('\n', pos);
    auto line = Json::parse(log.substr(line_start, line_end - line_start));
    line["data"]["wage"] = integer(line["data"]["wage"]) + 1;
    line["data"]["change"]["wage"] = line["data"]["wage"];
    {
        std::ofstream out(events_path);
        out << log.substr(0, line_start) << line.dump() << log.substr(line_end);
    }
    REQUIRE(verify(root / "dynamic")["passed"] == false);
}
static void integration(const fs::path &root) {
    auto config = town();
    RunOptions opt;
    opt.out = root / "baseline";
    opt.until = 60;
    opt.checkpoint_every = 15;
    auto baseline = run(config, 42, opt);
    REQUIRE(baseline["verification"]["passed"] == true);
    auto parent_before = tree(opt.out);
    auto checkpoint_path = opt.out / "checkpoints/day-0030.json";
    auto loaded = load_checkpoint(checkpoint_path);
    REQUIRE(!loaded.state.orders.empty());
    auto baseline_facts = facts(opt.out, 30);
    opt.out = root / "resumed";
    opt.loaded = loaded;
    auto resumed = run(config, 42, opt);
    REQUIRE(resumed["state_digest"] == baseline["state_digest"]);
    REQUIRE(facts(opt.out) == baseline_facts);
    opt.out = root / "noop";
    opt.intervention = Json{{"schema", "intervention-1"}, {"day", 30}, {"type", "noop"}};
    auto noop = run(config, 42, opt);
    REQUIRE(noop["state_digest"] == baseline["state_digest"]);
    REQUIRE(facts(opt.out) == baseline_facts);
    opt.out = root / "board";
    opt.intervention = Json{{"schema", "intervention-1"}, {"day", 30}, {"type", "public_price_board"}};
    auto board = run(config, 42, opt);
    REQUIRE(board["verification"]["passed"] == true);
    auto board_facts = facts(opt.out);
    int interventions = 0;
    for (auto &e : board_facts)
        if (e["type"] == "intervention") {
            ++interventions;
            REQUIRE(e["day"] == 30);
        }
    REQUIRE(interventions == 1);
    auto board_initial = read_json(opt.out / "initial_state.json");
    REQUIRE(board_initial["state"] == Json(loaded.state));
    REQUIRE(parent_before == tree(root / "baseline"));
    auto result = compare(root / "baseline", root / "board", 30, 60);
    REQUIRE(result["from_day"] == 30);
    REQUIRE(result["baseline"]["periods"] == 30);
    REQUIRE(compare(root / "baseline", root / "resumed", 30, 60)["baseline"] ==
            compare(root / "baseline", root / "resumed", 30, 60)["variant"]);
    fails([&] { compare(root / "baseline", root / "resumed", 0, 60); }, 4);
    opt.loaded.reset();
    opt.intervention.reset();
    opt.out = root / "repeat";
    auto repeat = run(config, 42, opt);
    REQUIRE(repeat["state_digest"] == baseline["state_digest"]);
    REQUIRE(facts(opt.out) == facts(root / "baseline"));
    REQUIRE(bytes(opt.out / "decisions.jsonl") == bytes(root / "baseline/decisions.jsonl"));
    opt.out = root / "summary";
    opt.trace = "summary";
    auto summary = run(config, 42, opt);
    REQUIRE(summary["state_digest"] == baseline["state_digest"]);
    REQUIRE(facts(opt.out) == facts(root / "baseline"));
    opt.out = root / "different_seed";
    run(config, 43, opt);
    fails([&] { compare(root / "baseline", opt.out, 0, 60); }, 4);
    REQUIRE(inspect(root / "baseline", "household:7")["account"]["id"] == "household:7");
    std::string order_decision;
    scan_lines(root / "baseline/decisions.jsonl", [&](const Json &d) {
        if (order_decision.empty() && !d.at("order_ids").empty())
            order_decision = d.at("id").get<std::string>();
    });
    REQUIRE(!order_decision.empty());
    auto delivery_explanation = explain(root / "baseline", order_decision);
    bool reserved = false, settled = false;
    for (auto &event : delivery_explanation["events"]) {
        if (event["type"] == "order_reserved")
            reserved = true;
        if (event["type"] == "trade")
            settled = true;
    }
    REQUIRE(reserved && settled);
    auto explanation = explain(root / "baseline", "decision:1");
    REQUIRE(explanation["decision"]["actor"].is_string());
    fails([&] { explain(root / "baseline", "decision:999999"); }, 2);
    auto bad = read_json(checkpoint_path);
    bad["schema"] = "checkpoint-99";
    write_json(root / "bad.json", bad);
    fails([&] { load_checkpoint(root / "bad.json"); }, 4);
    {
        std::ofstream out(root / "bad.json");
        out << "{\"schema\":";
    }
    fails([&] { load_checkpoint(root / "bad.json"); });
    bad = read_json(checkpoint_path);
    bad["state"].erase("orders");
    bad.erase("checkpoint_digest");
    bad["checkpoint_digest"] = digest(bad);
    write_json(root / "bad.json", bad);
    fails([&] { load_checkpoint(root / "bad.json"); }, 4);
    opt.loaded = loaded;
    opt.until = 29;
    opt.out = root / "earlier";
    fails([&] { run(config, 42, opt); }, 2);
    opt.until = 60;
    opt.out = root / "baseline";
    fails([&] { run(config, 42, opt); }, 3);
    opt.loaded.reset();
    opt.out = root / "unknown_intervention";
    opt.intervention = {{"schema", "intervention-1"}, {"type", "magic"}, {"day", 0}};
    fails([&] { run(config, 42, opt); }, 2);
    RunOptions empty;
    empty.out = root / "empty_resume";
    empty.loaded = loaded;
    empty.until = 30;
    REQUIRE(run(config, 42, empty)["state_digest"] == digest(Json(loaded.state)));
    empty.out = root / "empty_branch";
    empty.intervention = {{"schema", "intervention-1"}, {"type", "noop"}, {"day", 30}};
    fails([&] { run(config, 42, empty); }, 2);
    empty.until = 31;
    empty.out = root / "baseline/nested_branch";
    fails([&] { run(config, 42, empty); }, 3);
    // Missing requested evidence must never be treated as a verified run.
    auto saved_decisions = bytes(root / "noop/decisions.jsonl");
    fs::remove(root / "noop/decisions.jsonl");
    fails([&] { verify(root / "noop"); }, 3);
    {
        std::ofstream out(root / "noop/decisions.jsonl");
        out << saved_decisions;
    }
    // Independent verifier must detect tampering, not trust the prior verification report.
    auto metrics_path = root / "repeat/metrics.jsonl";
    auto contents = bytes(metrics_path);
    auto newline = contents.find('\n');
    auto first = Json::parse(contents.substr(0, newline));
    first["wages"] = 999;
    {
        std::ofstream out(metrics_path);
        out << first.dump() << contents.substr(newline);
    }
    REQUIRE(verify(root / "repeat")["passed"] == false);
    auto event_path = root / "summary/events.jsonl";
    auto events = bytes(event_path);
    auto event_end = events.find('\n');
    {
        std::ofstream out(event_path);
        out << events.substr(event_end + 1);
    }
    REQUIRE(verify(root / "summary")["passed"] == false);
}
int main() {
    auto root = fs::temp_directory_path() / ("econ-tests-" + std::to_string(getpid()));
    try {
        REQUIRE(!fs::exists(root));
        fs::create_directories(root);
        auto test = [&](const char *name, const std::function<void()> &f) {
            f();
            std::cout << "PASS " << name << '\n';
        };
        test("accounting intermediate and 30-day", accounting);
        test("firm policy evidence and result links", firm_decisions);
        test("labor ledger: payroll buys labor, production consumes it, verifier rebuilds it",
             [&] { labor_ledger(root); });
        test("atomic trades, rejection, payroll inactivity", transactions);
        test("checked arithmetic boundaries", arithmetic);
        test("recipe input accounting and capacity", recipe);
        test("bounded extreme price policy", price_boundaries);
        test("ID exhaustion is atomic", id_exhaustion);
        test("competing reservations, delayed settlement, pending checkpoint", [&] { reservations(root); });
        test("hidden store, discovery, checkout, refresh", information);
        test("strict scenario rejection", invalid_scenarios);
        test("labor arithmetic, zero capacity, extreme capacity and offer bounds",
             [&] { labor_arithmetic(root); });
        test("dynamic employment: modes, records, validation", [&] { dynamic_basics(root); });
        test("vacancy board: postings, eligibility, offers, quitting", [&] { dynamic_board(root); });
        test("staffing plans: runway, cancellation, demand, inventory, tight payroll",
             [&] { dynamic_staffing(root); });
        test("household job preferences, switching, search metrics", [&] { household_preferences(root); });
        test("dynamic persistence, resume fidelity, tamper detection", [&] { dynamic_persistence(root); });
        test("60-day persistence, branches, traces, metrics, incompatible data", [&] { integration(root); });
        fs::remove_all(root);
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "FAIL " << e.what() << " (evidence retained in " << root << ")\n";
        return 1;
    }
}
