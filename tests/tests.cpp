#include <econ/econ.hpp>
#include <fstream>
#include <iostream>
#include <limits>
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
static void accounting() {
    auto config = fixture();
    std::vector<Json> events;
    Engine e(initial(config, 42), config, [&](auto &j) { events.push_back(j); });
    e.work();
    REQUIRE(events.size() == 7);
    for (int i = 0; i < 6; ++i)
        REQUIRE(events[static_cast<std::size_t>(i)]["type"] == "wage");
    REQUIRE(events[6]["type"] == "production");
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
        test("atomic trades, rejection, payroll inactivity", transactions);
        test("checked arithmetic boundaries", arithmetic);
        test("recipe input accounting and capacity", recipe);
        test("bounded extreme price policy", price_boundaries);
        test("ID exhaustion is atomic", id_exhaustion);
        test("competing reservations, delayed settlement, pending checkpoint", [&] { reservations(root); });
        test("hidden store, discovery, checkout, refresh", information);
        test("strict scenario rejection", invalid_scenarios);
        test("60-day persistence, branches, traces, metrics, incompatible data", [&] { integration(root); });
        fs::remove_all(root);
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "FAIL " << e.what() << " (evidence retained in " << root << ")\n";
        return 1;
    }
}
