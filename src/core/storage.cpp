#include <chrono>
#include <econ/econ.hpp>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <sys/resource.h>
namespace econ {
namespace fs = std::filesystem;
Json read_json(const fs::path &p) {
    std::ifstream in(p);
    if (!in)
        throw Error(3, "cannot read " + p.string());
    try {
        Json j;
        in >> j;
        in >> std::ws;
        if (!in.eof())
            throw Error(2, "trailing data in " + p.string());
        return j;
    } catch (const Json::exception &e) {
        throw Error(2, "invalid JSON in " + p.string() + ": " + e.what());
    }
}
void write_json(const fs::path &p, const Json &j) {
    auto tmp = p;
    tmp += ".tmp";
    {
        std::ofstream o(tmp);
        if (!o)
            throw Error(3, "cannot write " + tmp.string());
        o << j.dump(2) << '\n';
        o.close();
        if (!o)
            throw Error(3, "write failed " + tmp.string());
    }
    fs::rename(tmp, p);
}
void scan_lines(const fs::path &p, const Sink &sink) {
    std::ifstream in(p);
    if (!in)
        throw Error(3, "cannot read " + p.string());
    std::string line;
    std::size_t n = 0;
    while (std::getline(in, line)) {
        ++n;
        try {
            sink(Json::parse(line));
        } catch (const Json::exception &e) {
            throw Error(2, "invalid JSONL " + p.string() + ":" + std::to_string(n) + ": " + e.what());
        }
    }
    if (in.bad())
        throw Error(3, "read failed " + p.string());
}
Json checkpoint(const State &s, const Json &scenario, const std::string &origin,
                const std::string &parent_run) {
    Json j = {
        {"schema", "checkpoint-1"},        {"model", model_version}, {"scenario", scenario},    {"state", s},
        {"state_digest", digest(Json(s))}, {"origin", origin},       {"parent_run", parent_run}};
    j["checkpoint_digest"] = digest(j);
    return j;
}
Loaded load_checkpoint(const fs::path &p) {
    try {
        auto j = read_json(p);
        if (j.at("schema") != "checkpoint-1" || j.at("model") != model_version)
            throw Error(4, "incompatible checkpoint schema/model");
        auto hash = j.at("checkpoint_digest").get<std::string>();
        j.erase("checkpoint_digest");
        if (hash != digest(j))
            throw Error(4, "checkpoint integrity mismatch");
        auto normalized = normalize(j.at("scenario"));
        if (normalized != j.at("scenario"))
            throw Error(4, "checkpoint config is not normalized");
        std::function<void(const Json &)> exact_numbers = [&](const Json &value) {
            if (value.is_number())
                integer(value);
            else if (value.is_structured())
                for (auto &child : value)
                    exact_numbers(child);
        };
        exact_numbers(j.at("state"));
        auto s = j.at("state").get<State>();
        if (j.at("state_digest") != digest(Json(s)))
            throw Error(4, "checkpoint state digest mismatch");
        validate_state(s, normalized);
        // Fixed model structure must match scenario, while memories/prices may evolve.
        if (!valid_employment_mode(s.employment_mode))
            throw Error(4, "checkpoint employment mode unknown");
        auto base = initial(normalized, s.seed, s.employment_mode);
        if (base.accounts.size() != s.accounts.size() || base.households.size() != s.households.size() ||
            base.firms.size() != s.firms.size())
            throw Error(4, "checkpoint entity mismatch");
        for (std::size_t i = 0; i < base.accounts.size(); ++i)
            if (base.accounts[i].id != s.accounts[i].id ||
                base.accounts[i].stock.size() != s.accounts[i].stock.size())
                throw Error(4, "checkpoint accounts mismatch");
        for (std::size_t i = 0; i < base.households.size(); ++i) {
            auto actual = Json(s.households[i]), expected = Json(base.households[i]);
            // Memories and payroll streaks evolve; under dynamic employment so does the employer.
            for (auto key : {"knowledge", "known_shops", "unpaid_days"}) {
                actual.erase(key);
                expected.erase(key);
            }
            if (s.employment_mode == employment_dynamic) {
                actual.erase("employer");
                expected.erase("employer");
            }
            if (actual != expected)
                throw Error(4, "checkpoint household config mismatch");
        }
        for (std::size_t i = 0; i < base.firms.size(); ++i) {
            auto actual = Json(s.firms[i]), expected = Json(base.firms[i]);
            for (auto key : {"payroll_missed_days", "demand_history"}) {
                actual.erase(key);
                expected.erase(key);
            }
            if (s.firms[i].prices.size() != base.firms[i].prices.size())
                throw Error(4, "checkpoint prices mismatch");
            for (auto &[product, price] : s.firms[i].prices) {
                if (!base.firms[i].prices.contains(product) ||
                    (s.firms[i].price_policy == "fixed" && price != base.firms[i].prices.at(product)) ||
                    (s.firms[i].price_policy == "inventory_band" &&
                     (price < s.firms[i].price_min || price > s.firms[i].price_max)))
                    throw Error(4, "checkpoint price policy violation");
            }
            actual.erase("prices");
            expected.erase("prices");
            if (actual != expected)
                throw Error(4, "checkpoint firm config mismatch");
        }
        auto origin = digest(Json{{"scenario", normalized}, {"seed", s.seed}, {"model", model_version}});
        if (j.at("origin") != origin)
            throw Error(4, "checkpoint origin mismatch");
        return {s, normalized, origin, j.at("parent_run"), hash};
    } catch (const Json::exception &e) {
        throw Error(4, std::string("incomplete/corrupt checkpoint: ") + e.what());
    }
}
static Json state_file(const State &s) {
    return {{"schema", "state-1"}, {"model", model_version}, {"state", s}, {"digest", digest(Json(s))}};
}
static bool ancestor(const fs::path &a, const fs::path &b) {
    auto ai = a.begin(), bi = b.begin();
    for (; ai != a.end(); ++ai, ++bi)
        if (bi == b.end() || *ai != *bi)
            return false;
    return true;
}
Json run(const Json &config, std::uint64_t seed, const RunOptions &opt) {
    auto scenario = normalize(config);
    if (!valid_employment_mode(opt.employment))
        throw Error(2, "employment must be fixed or dynamic");
    State s = opt.loaded ? opt.loaded->state : initial(scenario, seed, opt.employment);
    if (opt.loaded && scenario != opt.loaded->scenario)
        throw Error(4, "resume scenario mismatch");
    if (opt.until < s.next_day || opt.until > 10000000 || opt.checkpoint_every < 0)
        throw Error(2, "invalid until-day/checkpoint interval");
    if (opt.trace != "audit" && opt.trace != "summary")
        throw Error(2, "trace must be audit or summary");
    if (opt.intervention && opt.until == s.next_day)
        throw Error(2, "branch must advance beyond intervention boundary");
    if (opt.intervention) {
        Engine trial(s, scenario);
        trial.intervene(*opt.intervention);
    }
    auto out = fs::weakly_canonical(fs::absolute(opt.out));
    if (opt.out.empty() || ancestor(out, fs::current_path()))
        throw Error(3, "unsafe output location");
    if (opt.loaded && (ancestor(out, fs::weakly_canonical(opt.loaded->parent_run)) ||
                       ancestor(fs::weakly_canonical(opt.loaded->parent_run), out)))
        throw Error(3, "cannot overwrite checkpoint parent");
    if (fs::exists(out)) {
        if (!opt.overwrite)
            throw Error(3, "output already exists: " + out.string());
        fs::remove_all(out);
    }
    fs::create_directories(out / "checkpoints");
    std::string origin = digest(Json{{"scenario", scenario}, {"seed", s.seed}, {"model", model_version}});
    Json manifest = {
        {"schema", "manifest-1"},
        {"model", model_version},
        {"status", "running"},
        {"scenario_digest", digest(scenario)},
        {"seed", s.seed},
        {"origin", origin},
        {"build", ECON_BUILD},
        {"from_day", s.next_day},
        {"until_day", opt.until},
        {"trace", opt.trace},
        {"employment", s.employment_mode},
        {"parent", opt.loaded ? Json{{"run", opt.loaded->parent_run},
                                     {"checkpoint_digest", opt.loaded->checkpoint_digest},
                                     {"state_digest", digest(Json(s))}}
                              : Json(nullptr)},
        {"intervention", opt.intervention ? *opt.intervention : Json(nullptr)},
        {"artifacts",
         {"scenario.normalized.json", "initial_state.json", "events.jsonl", "decisions.jsonl",
          "metrics.jsonl", "replay.jsonl", "final_state.json", "verification.json", "checkpoints/"}}};
    manifest["run_id"] = digest(Json{{"origin", origin},
                                     {"employment", s.employment_mode},
                                     {"initial", digest(Json(s))},
                                     {"intervention", manifest["intervention"]},
                                     {"until", opt.until}});
    write_json(out / "manifest.json", manifest);
    write_json(out / "scenario.normalized.json", scenario);
    write_json(out / "initial_state.json", state_file(s));
    std::ofstream ev(out / "events.jsonl"), dec(out / "decisions.jsonl"), met(out / "metrics.jsonl"),
        replay(out / "replay.jsonl");
    if (!ev || !dec || !met || !replay)
        throw Error(3, "cannot open run artifacts");
    auto start = std::chrono::steady_clock::now();
    double serialization = 0;
    auto timed = [&](std::ofstream &stream, const Json &record) {
        auto a = std::chrono::steady_clock::now();
        stream << record.dump() << '\n';
        if (!stream)
            throw Error(3, "artifact write failed");
        serialization += std::chrono::duration<double>(std::chrono::steady_clock::now() - a).count();
    };
    std::uint64_t decisions_count = 0;
    Engine engine(
        s, scenario, [&](auto &e) { timed(ev, e); },
        [&](auto &d) {
            ++decisions_count;
            timed(dec, d);
        },
        opt.trace);
    try {
        if (opt.intervention)
            engine.intervene(*opt.intervention);
        while (engine.state.next_day < opt.until) {
            auto metric = engine.step();
            timed(met, metric);
            Json display = {{"schema", "replay-1"},
                            {"next_day", engine.state.next_day},
                            {"accounts", engine.state.accounts},
                            {"firms", engine.state.firms},
                            {"households", Json::array()},
                            {"pending_changes", engine.state.pending_changes},
                            {"board", engine.state.board}};
            for (auto &h : engine.state.households)
                display["households"].push_back({{"id", h.id},
                                                 {"employer", h.employer},
                                                 {"needs", h.needs},
                                                 {"unpaid_days", h.unpaid_days},
                                                 {"job_preferences", h.job_preferences}});
            timed(replay, display);
            if (opt.checkpoint_every && engine.state.next_day % opt.checkpoint_every == 0) {
                std::ostringstream name;
                name << "day-" << std::setfill('0') << std::setw(4) << engine.state.next_day << ".json";
                write_json(out / "checkpoints" / name.str(),
                           checkpoint(engine.state, scenario, origin, out.string()));
            }
        }
        ev.close();
        dec.close();
        met.close();
        replay.close();
        if (!ev || !dec || !met || !replay)
            throw Error(3, "artifact flush failed");
        write_json(out / "final_state.json", state_file(engine.state));
        manifest["state_digest"] = digest(Json(engine.state));
        manifest["status"] = "complete";
        auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        struct rusage usage{};
        getrusage(RUSAGE_SELF, &usage);
        manifest["performance"] = {
            {"elapsed_seconds", elapsed},
            {"jsonl_serialization_write_seconds", serialization},
            {"other_execution_seconds", elapsed - serialization},
            {"decisions", decisions_count},
            {"decisions_per_second", elapsed > 0 ? static_cast<double>(decisions_count) / elapsed : 0.0},
            {"days_per_second", elapsed > 0 ? static_cast<double>(opt.until - s.next_day) / elapsed : 0.0},
            {"employment_changes", engine.state.next_change - s.next_change},
            {"peak_rss_kib", usage.ru_maxrss},
            {"households", s.households.size()},
            {"firms", s.firms.size()}};
        write_json(out / "manifest.json", manifest);
        auto verification = verify(out);
        write_json(out / "verification.json", verification);
        if (!verification.at("passed").get<bool>())
            throw Error(5, "persisted run verification failed: " + verification.dump());
        std::uintmax_t bytes = 0;
        for (auto &e : fs::recursive_directory_iterator(out))
            if (e.is_regular_file())
                bytes += e.file_size();
        manifest["performance"]["output_bytes"] = bytes;
        write_json(out / "manifest.json", manifest);
        return {{"run", out.string()},
                {"state_digest", manifest["state_digest"]},
                {"verification", verification},
                {"performance", manifest["performance"]}};
    } catch (...) {
        manifest["status"] = "failed";
        write_json(out / "manifest.json", manifest);
        throw;
    }
}
static void inc(Json &j, const std::string &k, Amount n) {
    if (j.is_null())
        j = Json::object();
    j[k] = add(j.value(k, Amount{0}), n);
}
Json verify(const fs::path &dir) {
    Json checks = Json::array();
    auto check = [&](const std::string &name, bool pass) {
        checks.push_back({{"check", name}, {"status", pass ? "passed" : "failed"}});
    };
    auto manifest = read_json(dir / "manifest.json"),
         scenario = normalize(read_json(dir / "scenario.normalized.json")),
         start = read_json(dir / "initial_state.json"), finish = read_json(dir / "final_state.json");
    if (manifest.at("schema") != "manifest-1" || manifest.at("model") != model_version ||
        start.at("schema") != "state-1" || finish.at("schema") != "state-1" ||
        start.at("model") != model_version || finish.at("model") != model_version)
        throw Error(4, "unsupported run/state schema");
    auto initial_state = start.at("state").get<State>(), final_state = finish.at("state").get<State>();
    validate_state(initial_state, scenario);
    validate_state(final_state, scenario);
    check("complete_manifest", manifest.at("status") == "complete");
    check("scenario_digest", manifest.at("scenario_digest") == digest(scenario));
    check("state_digests", start.at("digest") == digest(Json(initial_state)) &&
                               finish.at("digest") == digest(Json(final_state)) &&
                               manifest.at("state_digest") == finish.at("digest"));
    check(
        "origin_and_seed",
        manifest.at("seed") == initial_state.seed && manifest.at("seed") == final_state.seed &&
            manifest.at("origin") ==
                digest(Json{{"scenario", scenario}, {"seed", initial_state.seed}, {"model", model_version}}));
    check("interval", manifest.at("from_day") == initial_state.next_day &&
                          manifest.at("until_day") == final_state.next_day);
    check("employment_mode", manifest.at("employment") == initial_state.employment_mode &&
                                 initial_state.employment_mode == final_state.employment_mode);
    auto reconstructed = initial_state;
    std::uint64_t sequence = initial_state.next_event;
    Day expected_day = initial_state.next_day;
    std::ifstream event_file(dir / "events.jsonl");
    if (!event_file)
        throw Error(3, "missing event log");
    std::optional<Json> next;
    auto advance = [&]() {
        std::string line;
        if (std::getline(event_file, line))
            next = Json::parse(line);
        else {
            if (event_file.bad())
                throw Error(3, "event read failed");
            next.reset();
        }
    };
    advance();
    std::map<std::string, Order> pending;
    for (auto &o : initial_state.orders)
        pending[o.id] = o;
    std::map<std::string, EmploymentChange> pending_changes;
    for (auto &c : initial_state.pending_changes)
        pending_changes[c.id] = c;
    auto change_sequence = initial_state.next_change, employment_sequence = initial_state.next_employment,
         posting_sequence = initial_state.next_posting;
    auto posting_of = [&](const std::string &id) -> Posting * {
        for (auto &p : reconstructed.board)
            if (p.id == id)
                return &p;
        return nullptr;
    };
    auto open_wage = [&](const std::string &household) -> std::optional<Money> {
        for (auto &r : reconstructed.employment)
            if (r.household == household && r.end == -1)
                return r.wage;
        return {};
    };
    const auto demand_window =
        static_cast<std::size_t>(integer(scenario.at("employment").at("demand_window_days")));
    // Search outcomes are decisions, not ledger events: the metrics scan keeps what it reads so the
    // decision scan below can reconcile it (job_search totals and each person's search record).
    std::map<Day, Json> recorded_search, recorded_person_search, recorded_applications;
    auto household_of = [&](const std::string &id) -> Household & {
        for (auto &h : reconstructed.households)
            if (h.id == id)
                return h;
        throw Error(4, "unknown household in event log: " + id);
    };
    const bool dynamic = initial_state.employment_mode == employment_dynamic;
    Amount event_count = 0, metric_count = 0;
    bool ledger_ok = true, metrics_ok = true, orders_ok = true, semantics_ok = true, labor_ok = true;
    Json totals = {
        {"production", Json::object()}, {"consumption", Json::object()}, {"unmet", Json::object()}};
    scan_lines(dir / "metrics.jsonl", [&](const Json &metric) {
        auto day = integer(metric.at("day"));
        if (metric.at("schema") != "metric-1" || day != expected_day || day >= final_state.next_day)
            metrics_ok = false;
        Json production = Json::object(), consumption = Json::object(), unmet = Json::object(),
             flow = Json::object(), sales = {{"retail", 0}, {"wholesale", 0}},
             units = {{"retail", Json::object()}, {"wholesale", Json::object()}}, realized = Json::array();
        Amount wages = 0, paid = 0, failed = 0, stockouts = 0, hires = 0, separations = 0;
        std::set<std::string> paid_households, producing_firms, changed_today;
        std::map<std::string, Amount> paid_amount;
        std::map<std::pair<std::string, std::string>, Amount> consumed_by, unmet_by;
        Json inactive = Json::object(), labor = Json::object(), board_activity = Json::array(),
             labor_units = {{"delivered", 0}, {"used", 0}, {"idle", 0}, {"retail_service", 0}},
             labor_flow = Json::object();
        std::map<std::string, Amount> sold, shortfall;
        std::set<std::string> applied;
        Json search_from_events = {{"applied", 0}, {"hires", 0}, {"switches", 0}, {"quits", 0}};
        int phase_rank = -1;
        const std::map<std::string, int> phases = {
            {"intervention", 0}, {"arrivals", 1}, {"employment", 2},  {"work", 3},   {"procurement", 4},
            {"information", 5},  {"shopping", 6}, {"consumption", 7}, {"pricing", 8}};
        while (next && integer(next->at("day")) == day) {
            auto e = *next;
            auto type = e.at("type").get<std::string>();
            auto &d = e.at("data");
            ++event_count;
            auto phase = e.at("phase").get<std::string>();
            if (!phases.contains(phase) || phases.at(phase) < phase_rank)
                ledger_ok = false;
            else
                phase_rank = phases.at(phase);
            if (!dynamic && phase == "employment")
                ledger_ok = false;
            if (e.at("schema") != "event-1" || e.at("id") != "event:" + std::to_string(sequence++))
                ledger_ok = false;
            Json required = Json::array();
            if (type == "trade") {
                auto q = integer(d.at("quantity")), price = integer(d.at("price")), amount = mul(q, price);
                auto buyer = d.at("buyer").get<std::string>(), seller = d.at("seller").get<std::string>(),
                     p = d.at("product").get<std::string>();
                if (q <= 0 || price <= 0 || amount != integer(d.at("amount")))
                    semantics_ok = false;
                required = {effect(buyer, "cash", -amount), effect(seller, "cash", amount),
                            effect(seller, "stock", -q, p), effect(buyer, "stock", q, p)};
                if (!d.at("order").is_null()) {
                    auto id = d.at("order").get<std::string>();
                    if (!pending.contains(id))
                        orders_ok = false;
                    else {
                        auto o = pending.at(id);
                        if (o.arrival != day || o.buyer != buyer || o.seller != seller || o.product != p ||
                            o.quantity != q || o.price != price)
                            orders_ok = false;
                        pending.erase(id);
                    }
                    required.push_back(effect(buyer, "reserved_cash", -amount));
                    required.push_back(effect(seller, "reserved_stock", -q, p));
                }
                auto stage = d.at("stage").get<std::string>();
                if (stage != "retail" && stage != "wholesale")
                    semantics_ok = false;
                sold[seller] = add(sold[seller], q);
                inc(sales, stage, amount);
                inc(units[stage], p, q);
                realized.push_back({{"stage", stage}, {"product", p}, {"price", price}, {"quantity", q}});
            } else if (type == "order_reserved") {
                auto o = d.at("order").get<Order>();
                if (pending.contains(o.id) || o.arrival <= day)
                    orders_ok = false;
                pending[o.id] = o;
                required = {effect(o.buyer, "reserved_cash", mul(o.price, o.quantity)),
                            effect(o.seller, "reserved_stock", o.quantity, o.product)};
            } else if (type == "wage") {
                auto amount = integer(d.at("amount"));
                auto employer = d.at("employer").get<std::string>(), h = d.at("household").get<std::string>();
                firm(reconstructed, employer);
                bool employed = false;
                for (auto &household : reconstructed.households)
                    if (household.id == h && household.employer == employer)
                        employed = true;
                // The wage paid is the one agreed for the open spell, not necessarily the firm's base wage.
                if (phase != "work" || !employed || open_wage(h) != amount ||
                    !paid_households.insert(h).second || integer(d.at("labor_units")) != 1)
                    semantics_ok = false;
                // Payroll buys one worker-day of labor, which only production may consume.
                required = {effect(employer, "cash", -amount), effect(h, "cash", amount),
                            effect(employer, "labor", 1)};
                wages = add(wages, amount);
                paid_amount[h] = amount;
                ++paid;
                inc(labor_units, "delivered", 1);
                inc(labor_flow[employer], "delivered", 1);
            } else if (type == "production") {
                auto q = integer(d.at("quantity"));
                auto id = d.at("firm").get<std::string>(), p = d.at("product").get<std::string>();
                auto &f = firm(reconstructed, id);
                Amount paid_labor = 0;
                for (auto &h : reconstructed.households)
                    if (h.employer == id && paid_households.contains(h.id))
                        ++paid_labor;
                auto used = integer(d.at("labor_used"));
                if (phase != "work" || f.kind != "producer" || q <= 0 || q > f.capacity ||
                    q > mul_capped(f.output_per_worker, paid_labor, f.capacity) || p != f.output ||
                    d.at("inputs_per_unit") != Json(f.inputs) || paid_labor < f.labor_required ||
                    !producing_firms.insert(id).second || used != ceil_div(q, f.output_per_worker) ||
                    integer(d.at("paid_workers")) != paid_labor || used > account(reconstructed, id).labor ||
                    integer(d.at("labor_idle")) != integer(d.at("paid_workers")) - used)
                    semantics_ok = false;
                for (auto &[input, n] : f.inputs)
                    required.push_back(effect(id, "stock", -mul(q, n), input));
                required.push_back(effect(id, "stock", q, p));
                required.push_back(effect(id, "labor", -used));
                inc(production, p, q);
                inc(labor_units, "used", used);
                inc(labor_flow[id], "used", used);
            } else if (type == "labor_released") {
                // Whatever payroll bought and production did not use is released the same day.
                auto id = d.at("firm").get<std::string>();
                auto released = integer(d.at("units"));
                auto use = d.at("use").get<std::string>();
                auto &f = firm(reconstructed, id);
                if (released <= 0 || released != account(reconstructed, id).labor || phase != "work" ||
                    use != (f.kind == "producer" ? "idle" : "retail_service"))
                    semantics_ok = false;
                required = {effect(id, "labor", -released)};
                inc(labor_units, use, released);
                inc(labor_flow[id], use, released);
            } else if (type == "consumption") {
                auto q = integer(d.at("quantity"));
                if (q <= 0)
                    semantics_ok = false;
                required = {effect(d.at("household"), "stock", -q, d.at("product"))};
                inc(consumption, d.at("product"), q);
                auto key =
                    std::make_pair(d.at("household").get<std::string>(), d.at("product").get<std::string>());
                consumed_by[key] = add(consumed_by[key], q);
            } else if (type == "unmet") {
                auto q = integer(d.at("quantity"));
                if (q <= 0)
                    semantics_ok = false;
                inc(unmet, d.at("product"), q);
                auto key =
                    std::make_pair(d.at("household").get<std::string>(), d.at("product").get<std::string>());
                unmet_by[key] = add(unmet_by[key], q);
            } else if (type == "trade_rejected") {
                ++failed;
                if (d.at("reason") == "OUT_OF_STOCK") {
                    ++stockouts;
                    auto seller = d.at("seller").get<std::string>();
                    shortfall[seller] = add(shortfall[seller], integer(d.at("quantity")));
                }
            } else if (type == "procurement_blocked") {
                if (d.at("reason") == "OUT_OF_STOCK") {
                    auto supplier = d.at("supplier").get<std::string>();
                    shortfall[supplier] = add(shortfall[supplier], integer(d.at("desired")));
                }
            } else if (type == "supply_shortfall") {
                auto supplier = d.at("supplier").get<std::string>();
                auto lacking = integer(d.at("shortfall"));
                if (lacking <= 0 || lacking != integer(d.at("desired")) - integer(d.at("supplied")) ||
                    d.at("reason") != "OUT_OF_STOCK")
                    semantics_ok = false;
                shortfall[supplier] = add(shortfall[supplier], lacking);
            } else if (type == "price_changed") {
                auto &f = firm(reconstructed, d.at("firm"));
                auto p = d.at("product").get<std::string>();
                if (f.prices.at(p) != integer(d.at("old_price")))
                    semantics_ok = false;
                f.prices[p] = integer(d.at("new_price"));
            } else if (type == "intervention") {
                if (d.at("type") != "public_price_board" || reconstructed.public_board)
                    semantics_ok = false;
                reconstructed.public_board = true;
            } else if (type == "work_inactive") {
                inactive[d.at("firm").get<std::string>()] = d.at("reason");
            } else if (type == "posting_opened") {
                auto p = d.at("posting").get<Posting>();
                bool duplicate = false;
                for (auto &other : reconstructed.board)
                    if (other.firm == p.firm)
                        duplicate = true;
                auto &f = firm(reconstructed, p.firm);
                if (!dynamic || phase != "employment" ||
                    p.id != "posting:" + std::to_string(posting_sequence++) || duplicate || p.openings <= 0 ||
                    p.posted != day || p.unfilled_days != 0 || p.applications != 0 ||
                    p.wage < f.hiring.wage_min || p.wage > f.hiring.wage_max ||
                    p.min_experience_days != f.hiring.min_experience_days ||
                    p.accept_employed != f.hiring.accept_employed ||
                    p.rehire_cooldown_days != f.hiring.rehire_cooldown_days || d.at("firm") != p.firm ||
                    d.at("reason") != p.reason)
                    semantics_ok = false;
                reconstructed.board.push_back(p);
                board_activity.push_back({{"event", e.at("id")},
                                          {"type", type},
                                          {"firm", p.firm},
                                          {"posting", p.id},
                                          {"reason", p.reason}});
            } else if (type == "posting_adjusted") {
                auto *p = posting_of(d.at("posting").get<std::string>());
                auto field = d.at("field").get<std::string>();
                auto value = integer(d.at("new"));
                if (!dynamic || phase != "employment" || !p || d.at("firm") != p->firm ||
                    (field != "wage" && field != "openings") || value < 0 ||
                    (field == "openings" && value == 0))
                    semantics_ok = false;
                else {
                    auto &f = firm(reconstructed, p->firm);
                    if (integer(d.at("old")) != (field == "wage" ? p->wage : p->openings) ||
                        (field == "wage" && (value < f.hiring.wage_min || value > f.hiring.wage_max)))
                        semantics_ok = false;
                    (field == "wage" ? p->wage : p->openings) = value;
                    if (field == "wage")
                        p->unfilled_days = 0;
                    board_activity.push_back({{"event", e.at("id")},
                                              {"type", type},
                                              {"firm", p->firm},
                                              {"posting", p->id},
                                              {"reason", d.at("reason")}});
                }
            } else if (type == "posting_closed") {
                auto id = d.at("posting").get<std::string>();
                auto *p = posting_of(id);
                if (!dynamic || phase != "employment" || !p || d.at("firm") != p->firm ||
                    (d.at("reason") == "FILLED") != (p->openings == 0))
                    semantics_ok = false;
                else {
                    board_activity.push_back({{"event", e.at("id")},
                                              {"type", type},
                                              {"firm", p->firm},
                                              {"posting", p->id},
                                              {"reason", d.at("reason")}});
                    for (std::size_t i = 0; i < reconstructed.board.size(); ++i)
                        if (reconstructed.board[i].id == id)
                            reconstructed.board.erase(reconstructed.board.begin() +
                                                      static_cast<std::ptrdiff_t>(i));
                }
            } else if (type == "employment_change_cancelled") {
                auto c = d.at("change").get<EmploymentChange>();
                if (!dynamic || phase != "employment" || !pending_changes.contains(c.id) ||
                    Json(pending_changes.at(c.id)) != Json(c) || !c.to_employer.empty() ||
                    d.at("household") != c.household || d.at("from_employer") != c.from_employer ||
                    d.at("to_employer") != c.to_employer)
                    semantics_ok = false;
                pending_changes.erase(c.id);
                labor[c.household]["cancelled"] = {{"id", c.id},
                                                   {"from_employer", c.from_employer},
                                                   {"to_employer", c.to_employer},
                                                   {"reason", d.at("reason")}};
            } else if (type == "employment_scheduled") {
                // A scheduled change is a promise for a future day; it changes no job today.
                auto c = d.at("change").get<EmploymentChange>();
                auto &h = household_of(c.household);
                bool already = false;
                for (auto &[id, other] : pending_changes)
                    if (other.household == c.household)
                        already = true;
                if (!dynamic || phase != "employment" ||
                    c.id != "change:" + std::to_string(change_sequence++) || already || c.requested != day ||
                    c.effective <= day || h.employer != c.from_employer || c.from_employer == c.to_employer ||
                    d.at("household") != c.household || d.at("from_employer") != c.from_employer ||
                    d.at("to_employer") != c.to_employer || integer(d.at("effective")) != c.effective ||
                    d.at("reason") != c.reason || integer(d.at("wage")) != c.wage ||
                    d.at("posting") != c.posting)
                    semantics_ok = false;
                if (c.to_employer.empty()) {
                    if (c.wage != 0 || !c.posting.empty() || c.reason.rfind("LAYOFF_", 0) != 0 ||
                        c.effective != add(day, integer(scenario.at("employment").at("notice_days"))))
                        semantics_ok = false;
                } else {
                    // A hire answers a live posting at its advertised wage, consumes one opening and
                    // starts tomorrow.
                    auto *p = posting_of(c.posting);
                    auto expected_reason = c.from_employer.empty() ? "HIRED"
                                           : h.unpaid_days >= h.job_preferences.unpaid_tolerance_days
                                               ? "QUIT_UNPAID"
                                               : "SWITCHED";
                    if (!p || p->firm != c.to_employer || p->openings <= 0 || p->wage != c.wage ||
                        c.effective != day + 1 || c.reason != expected_reason || p->firm == h.employer ||
                        (!h.employer.empty() && !p->accept_employed) ||
                        experience_days(reconstructed, h.id, day) < p->min_experience_days)
                        semantics_ok = false;
                    else {
                        for (const auto &r : reconstructed.employment)
                            if (r.household == h.id && r.employer == p->firm && r.end != -1 &&
                                r.end_reason.rfind("LAYOFF_", 0) == 0 &&
                                day - r.end < p->rehire_cooldown_days)
                                semantics_ok = false;
                        --p->openings;
                        p->unfilled_days = 0;
                        p->applications = add(p->applications, 1);
                        applied.insert(p->id);
                    }
                    inc(search_from_events, "applied", 1);
                    inc(search_from_events,
                        c.reason == "HIRED"         ? "hires"
                        : c.reason == "QUIT_UNPAID" ? "quits"
                                                    : "switches",
                        1);
                    recorded_applications[day][c.household] = {
                        {"outcome", c.reason == "HIRED"         ? "HIRE_SCHEDULED"
                                    : c.reason == "QUIT_UNPAID" ? "QUIT_SCHEDULED"
                                                                : "SWITCH_SCHEDULED"},
                        {"chosen", c.posting},
                        {"event", e.at("id")}};
                }
                pending_changes[c.id] = c;
                labor[c.household]["scheduled"] = {{"id", c.id},
                                                   {"from_employer", c.from_employer},
                                                   {"to_employer", c.to_employer},
                                                   {"effective", c.effective},
                                                   {"wage", c.wage},
                                                   {"posting", c.posting},
                                                   {"reason", c.reason}};
            } else if (type == "employment_changed") {
                auto c = d.at("change").get<EmploymentChange>();
                auto &h = household_of(c.household);
                if (!dynamic || phase != "employment" || !pending_changes.contains(c.id) ||
                    Json(pending_changes.at(c.id)) != Json(c) || c.effective != day ||
                    h.employer != c.from_employer || !changed_today.insert(c.household).second ||
                    d.at("household") != c.household || d.at("from_employer") != c.from_employer ||
                    d.at("to_employer") != c.to_employer || d.at("reason") != c.reason)
                    semantics_ok = false;
                pending_changes.erase(c.id);
                Json closed = nullptr;
                for (auto &r : reconstructed.employment)
                    if (r.household == c.household && r.end == -1) {
                        r.end = day;
                        r.end_reason = c.reason;
                        closed = r.id;
                    }
                h.employer = c.to_employer;
                h.unpaid_days = 0;
                Json opened = nullptr;
                if (!c.to_employer.empty()) {
                    EmploymentRecord r{"employment:" + std::to_string(employment_sequence++),
                                       c.household,
                                       c.to_employer,
                                       day,
                                       -1,
                                       c.wage,
                                       c.reason,
                                       ""};
                    reconstructed.employment.push_back(r);
                    opened = r.id;
                }
                if (d.at("closed_record") != closed || d.at("opened_record") != opened)
                    semantics_ok = false;
                if (!c.from_employer.empty())
                    ++separations;
                if (!c.to_employer.empty())
                    ++hires;
                labor[c.household]["change"] = {{"id", c.id},
                                                {"from_employer", c.from_employer},
                                                {"to_employer", c.to_employer},
                                                {"wage", c.wage},
                                                {"reason", c.reason}};
            } else
                semantics_ok = false;
            if (required != e.at("effects"))
                semantics_ok = false;
            for (auto &ef : e.at("effects")) {
                auto &a = account(reconstructed, ef.at("actor"));
                auto field = ef.at("field").get<std::string>(), p = ef.at("product").get<std::string>();
                auto n = integer(ef.at("delta"));
                if (field == "cash") {
                    a.cash = add(a.cash, n);
                    if (a.id.rfind("firm:", 0) == 0)
                        inc(flow, a.id, n);
                } else if (field == "reserved_cash")
                    a.reserved_cash = add(a.reserved_cash, n);
                else if (field == "stock")
                    a.stock[p] = add(stock(a.stock, p), n);
                else if (field == "reserved_stock")
                    a.reserved_stock[p] = add(stock(a.reserved_stock, p), n);
                else if (field == "labor")
                    a.labor = add(a.labor, n);
                else
                    ledger_ok = false;
            }
            for (auto &a : reconstructed.accounts) {
                if (a.cash < 0 || a.reserved_cash < 0 || a.reserved_cash > a.cash || a.labor < 0)
                    ledger_ok = false;
                for (auto &[p, q] : a.stock)
                    if (q < 0 || stock(a.reserved_stock, p) < 0 || stock(a.reserved_stock, p) > q)
                        ledger_ok = false;
            }
            advance();
        }
        if (next && integer(next->at("day")) < day)
            ledger_ok = false;
        for (auto &a : reconstructed.accounts) {
            if (a.labor != 0) // labor never survives the day
                ledger_ok = false;
            if (metric.at("inventories").at(a.id) != Json(a.stock))
                metrics_ok = false;
            auto key = a.id.rfind("firm:", 0) == 0 ? "firm_cash" : "household_cash";
            if (integer(metric.at(key).at(a.id)) != a.cash)
                metrics_ok = false;
        }
        for (auto &h : reconstructed.households)
            for (auto &[p, need] : h.needs)
                if (add(consumed_by[{h.id, p}], unmet_by[{h.id, p}]) != need)
                    semantics_ok = false;
        // Postings nobody answered today age by a day; demand observations roll into the window.
        for (auto &p : reconstructed.board)
            if (p.openings > 0 && !applied.contains(p.id))
                p.unfilled_days = add(p.unfilled_days, 1);
        for (auto &f : reconstructed.firms) {
            f.demand_history.push_back(add(sold[f.id], shortfall[f.id]));
            if (f.demand_history.size() > demand_window)
                f.demand_history.erase(f.demand_history.begin());
        }
        // Payroll streaks: a firm with workers that ran no payroll missed one; a paid worker resets.
        for (auto &f : reconstructed.firms) {
            Amount workers = 0, paid_workers = 0;
            for (auto &h : reconstructed.households)
                if (h.employer == f.id) {
                    ++workers;
                    if (paid_households.contains(h.id))
                        ++paid_workers;
                }
            if ((paid_workers != 0 && paid_workers != workers) ||
                (paid_workers > 0 && workers < f.labor_required) ||
                (inactive.contains(f.id) && paid_workers != 0) ||
                (!inactive.contains(f.id) && paid_workers != workers))
                semantics_ok = false;
            if (inactive.contains(f.id)) {
                if (inactive.at(f.id) !=
                    (workers < f.labor_required ? "INSUFFICIENT_LABOR" : "INSUFFICIENT_PAYROLL"))
                    semantics_ok = false;
                if (workers > 0)
                    f.payroll_missed_days = add(f.payroll_missed_days, 1);
                else
                    f.payroll_missed_days = 0;
            } else
                f.payroll_missed_days = 0;
        }
        Amount employed = 0;
        for (auto &h : reconstructed.households) {
            if (metric.at("employment").at(h.id) != h.employer)
                metrics_ok = false;
            if (!h.employer.empty()) {
                ++employed;
                h.unpaid_days = paid_households.contains(h.id) ? 0 : add(h.unpaid_days, 1);
            }
            // Per-person documentation must match the day's ledger: job held, work, pay, changes.
            auto &person = labor[h.id];
            person["employer"] = h.employer.empty() ? Json(nullptr) : Json(h.employer);
            person["status"] = h.employer.empty() ? "unemployed" : "employed";
            person["worked"] = paid_households.contains(h.id);
            person["wage_paid"] = paid_households.contains(h.id) ? paid_amount.at(h.id) : 0;
            person["idle_reason"] = !h.employer.empty() && !paid_households.contains(h.id)
                                        ? inactive.value(h.employer, Json(nullptr))
                                        : Json(nullptr);
            person["unpaid_days"] = h.unpaid_days;
            for (auto key : {"change", "scheduled", "cancelled"})
                if (!person.contains(key))
                    person[key] = nullptr;
            if (paid_households.contains(h.id) && h.employer.empty())
                labor_ok = false;
        }
        Json demand = Json::object();
        for (auto &f : reconstructed.firms)
            demand[f.id] = f.demand_history.back();
        // Per-person search outcomes come from decisions; set them aside for the decision scan.
        Json recorded_labor = metric.at("labor"), person_search = Json::object();
        for (auto it = recorded_labor.begin(); it != recorded_labor.end(); ++it)
            if (it.value().is_object()) {
                person_search[it.key()] = it.value().value("search", Json(nullptr));
                it.value().erase("search");
            }
        recorded_person_search[day] = person_search;
        recorded_search[day] = metric.value("job_search", Json(nullptr));
        if (!recorded_search[day].is_object())
            labor_ok = false;
        else
            for (auto key : {"applied", "hires", "switches", "quits"})
                if (recorded_search[day].value(key, Json(nullptr)) != search_from_events.at(key))
                    labor_ok = false;
        if (recorded_labor != labor || metric.at("inactive_firms") != inactive ||
            metric.at("board") != Json(reconstructed.board) ||
            metric.at("board_activity") != board_activity || metric.at("demand") != demand ||
            integer(metric.at("hires")) != hires || integer(metric.at("separations")) != separations ||
            integer(metric.at("employed")) != employed ||
            integer(metric.at("unemployed")) !=
                static_cast<Amount>(reconstructed.households.size()) - employed ||
            integer(metric.at("pending_changes")) != static_cast<Amount>(pending_changes.size()))
            labor_ok = false;
        if (metric.at("labor_units") != labor_units || metric.at("labor_flow") != labor_flow)
            labor_ok = false;
        if (metric.at("production") != production || metric.at("consumption") != consumption ||
            metric.at("unmet") != unmet || metric.at("firm_net_cash_flow") != flow ||
            metric.at("sales_cents") != sales || metric.at("sales_units") != units ||
            metric.at("realized_prices") != realized || integer(metric.at("wages")) != wages ||
            integer(metric.at("paid_workers")) != paid || integer(metric.at("failed_attempts")) != failed ||
            integer(metric.at("stockouts")) != stockouts)
            metrics_ok = false;
        for (auto key : {"production", "consumption", "unmet"})
            for (auto it = metric.at(key).begin(); it != metric.at(key).end(); ++it)
                inc(totals[key], it.key(), integer(it.value()));
        ++expected_day;
        ++metric_count;
    });
    if (next || sequence != final_state.next_event || expected_day != final_state.next_day)
        ledger_ok = false;
    check("ordered_complete_economic_ledger", ledger_ok);
    check("event_operation_semantics", semantics_ok);
    check("per_day_metrics_reconcile", metrics_ok);
    check("per_person_labor_records_reconcile", labor_ok);
    check("closing_accounts_reconcile", Json(reconstructed.accounts) == Json(final_state.accounts));
    check("closing_prices_reconcile", Json(reconstructed.firms) == Json(final_state.firms));
    {
        // Jobs, spells and promises at the close must be exactly what the ledger implies.
        bool employment_ok = initial_state.employment_mode == final_state.employment_mode &&
                             change_sequence == final_state.next_change &&
                             employment_sequence == final_state.next_employment &&
                             posting_sequence == final_state.next_posting &&
                             Json(reconstructed.employment) == Json(final_state.employment) &&
                             Json(reconstructed.board) == Json(final_state.board);
        std::vector<EmploymentChange> expected_changes, actual_changes = final_state.pending_changes;
        for (auto &[id, c] : pending_changes) {
            (void)id;
            expected_changes.push_back(c);
        }
        auto by_id = [](const EmploymentChange &a, const EmploymentChange &b) { return a.id < b.id; };
        std::sort(expected_changes.begin(), expected_changes.end(), by_id);
        std::sort(actual_changes.begin(), actual_changes.end(), by_id);
        if (Json(expected_changes) != Json(actual_changes) ||
            reconstructed.households.size() != final_state.households.size())
            employment_ok = false;
        for (std::size_t i = 0; employment_ok && i < reconstructed.households.size(); ++i)
            if (reconstructed.households[i].id != final_state.households[i].id ||
                reconstructed.households[i].employer != final_state.households[i].employer ||
                reconstructed.households[i].unpaid_days != final_state.households[i].unpaid_days)
                employment_ok = false;
        check("employment_records_and_pending_changes_reconcile", employment_ok);
    }
    std::vector<Order> expected_orders;
    for (auto &[id, o] : pending) {
        (void)id;
        expected_orders.push_back(o);
    }
    auto actual_orders = final_state.orders;
    auto sort_orders = [](std::vector<Order> &orders) {
        std::sort(orders.begin(), orders.end(), [](const Order &a, const Order &b) { return a.id < b.id; });
    };
    sort_orders(actual_orders);
    sort_orders(expected_orders);
    check("pending_orders_reconcile", orders_ok && Json(expected_orders) == Json(actual_orders));
    bool decisions_ok = true;
    std::uint64_t decision_sequence = initial_state.next_decision;
    std::ifstream linked_events(dir / "events.jsonl");
    std::optional<Json> linked;
    auto next_link = [&]() {
        std::string line;
        if (std::getline(linked_events, line))
            linked = Json::parse(line);
        else
            linked.reset();
    };
    next_link();
    Day decision_day = initial_state.next_day;
    std::map<Day, Json> search_from_decisions, person_search_from_decisions;
    scan_lines(dir / "decisions.jsonl", [&](const Json &d) {
        auto day = integer(d.at("day"));
        if (d.at("policy") == "job_search-2") {
            auto &day_totals = search_from_decisions[day];
            if (day_totals.is_null())
                day_totals = {{"searching", 0},
                              {"declined", 0},
                              {"ineligible", 0},
                              {"no_postings", 0},
                              {"applied", 0},
                              {"hires", 0},
                              {"switches", 0},
                              {"quits", 0},
                              {"declined_reasons", Json::object()},
                              {"ineligible_reasons", Json::object()}};
            inc(day_totals, "searching", 1);
            auto outcome = d.at("outcome").get<std::string>();
            auto actor = d.at("actor").get<std::string>();
            if (!dynamic || actor.rfind("household:", 0) != 0 ||
                (person_search_from_decisions[day].is_object() &&
                 person_search_from_decisions[day].contains(actor)))
                decisions_ok = false;
            if (outcome == "DECLINED")
                inc(day_totals, "declined", 1);
            else if (outcome == "INELIGIBLE")
                inc(day_totals, "ineligible", 1);
            else if (outcome == "NO_POSTINGS")
                inc(day_totals, "no_postings", 1);
            else if (outcome == "HIRE_SCHEDULED" || outcome == "SWITCH_SCHEDULED" ||
                     outcome == "QUIT_SCHEDULED") {
                inc(day_totals, "applied", 1);
                inc(day_totals,
                    outcome == "HIRE_SCHEDULED"     ? "hires"
                    : outcome == "SWITCH_SCHEDULED" ? "switches"
                                                    : "quits",
                    1);
                const auto &applications = recorded_applications[day];
                if (!applications.is_object() || !applications.contains(actor) ||
                    applications.at(actor).at("outcome") != outcome ||
                    applications.at(actor).at("chosen") != d.at("chosen") ||
                    d.at("event_ids") != Json::array({applications.at(actor).at("event")}))
                    decisions_ok = false;
            } else
                decisions_ok = false;
            Amount declined = 0, ineligible = 0;
            if (d.contains("exclusions"))
                for (auto &x : d.at("exclusions")) {
                    bool preference = x.at("stage") == "preference";
                    inc(day_totals[preference ? "declined_reasons" : "ineligible_reasons"], x.at("reason"),
                        1);
                    (preference ? declined : ineligible) += 1;
                }
            Json person = {{"outcome", outcome}, {"reason", d.at("reason")}, {"chosen", d.at("chosen")}};
            if (d.contains("exclusions")) {
                person["declined"] = declined;
                person["ineligible"] = ineligible;
            }
            person_search_from_decisions[day][d.at("actor").get<std::string>()] = person;
        }
        if (d.at("schema") != "decision-1" ||
            d.at("id") != "decision:" + std::to_string(decision_sequence++) || day < decision_day ||
            day >= final_state.next_day)
            decisions_ok = false;
        decision_day = day;
        bool actor = false;
        for (auto &h : initial_state.accounts)
            if (d.at("actor") == h.id)
                actor = true;
        if (!actor)
            decisions_ok = false;
        if (manifest.at("trace") == "audit") {
            if (!d.contains("knowledge") || !d.contains("alternatives") || !d.contains("exclusions") ||
                !d.at("omitted").empty())
                decisions_ok = false;
        } else if (d.at("omitted") != Json({"knowledge", "alternatives", "exclusions"}))
            decisions_ok = false;
        for (auto &event_id : d.at("event_ids")) {
            if (!event_id.is_string())
                throw Error(5, "invalid linked event ID");
            auto target = event_id.get<std::string>();
            if (target.rfind("event:", 0) != 0)
                throw Error(5, "invalid linked event ID");
            auto n = std::stoull(target.substr(6));
            while (linked && std::stoull(linked->at("id").get<std::string>().substr(6)) < n)
                next_link();
            if (!linked || linked->at("id") != target || linked->at("day") != day)
                decisions_ok = false;
            else if (d.at("kind") == "household") {
                auto type = linked->at("type").get<std::string>();
                if (type == "employment_scheduled") {
                    if (linked->at("data").at("household") != d.at("actor") ||
                        d.at("policy") != "job_search-2")
                        decisions_ok = false;
                } else if ((type != "trade" && type != "trade_rejected") ||
                           linked->at("data").at("buyer") != d.at("actor") ||
                           linked->at("data").at("product") != d.at("product"))
                    decisions_ok = false;
            } else {
                auto &data = linked->at("data");
                auto type = linked->at("type").get<std::string>();
                if (type == "employment_scheduled" || type == "employment_change_cancelled") {
                    if (data.at("from_employer") != d.at("actor") || d.at("policy") != "staffing_plan-1")
                        decisions_ok = false;
                } else if (type == "order_reserved") {
                    if (data.at("order").at("buyer") != d.at("actor") ||
                        std::find(d.at("order_ids").begin(), d.at("order_ids").end(),
                                  data.at("order").at("id")) == d.at("order_ids").end())
                        decisions_ok = false;
                } else if (type == "trade") {
                    if (data.at("buyer") != d.at("actor"))
                        decisions_ok = false;
                } else if (type == "wage") {
                    if (data.at("employer") != d.at("actor"))
                        decisions_ok = false;
                } else if (!data.contains("firm") || data.at("firm") != d.at("actor"))
                    decisions_ok = false;
            }
            if (d.at("outcome") == "PURCHASED" && (!linked || linked->at("type") != "trade"))
                decisions_ok = false;
        }
        if (d.at("outcome") == "PURCHASED" && d.at("event_ids").empty())
            decisions_ok = false;
    });
    if (decision_sequence != final_state.next_decision)
        decisions_ok = false;
    check("decision_sequence_and_event_links", decisions_ok);
    // Search totals and each person's search record must agree with the recorded decisions. Summary
    // traces omit exclusions, so reason breakdowns are only checked when the evidence is present.
    bool search_ok = true;
    for (auto &[day, recorded] : recorded_search) {
        if (!recorded.is_object()) {
            search_ok = false;
            continue;
        }
        auto expected = search_from_decisions.contains(day) ? search_from_decisions.at(day)
                                                            : Json{{"searching", 0},
                                                                   {"declined", 0},
                                                                   {"ineligible", 0},
                                                                   {"no_postings", 0},
                                                                   {"applied", 0},
                                                                   {"hires", 0},
                                                                   {"switches", 0},
                                                                   {"quits", 0},
                                                                   {"declined_reasons", Json::object()},
                                                                   {"ineligible_reasons", Json::object()}};
        for (auto key :
             {"searching", "declined", "ineligible", "no_postings", "applied", "hires", "switches", "quits"})
            if (recorded.value(key, Json(nullptr)) != expected.at(key))
                search_ok = false;
        if (manifest.at("trace") == "audit")
            for (auto key : {"declined_reasons", "ineligible_reasons"})
                if (recorded.value(key, Json(nullptr)) != expected.at(key))
                    search_ok = false;
        auto &persons = recorded_person_search[day];
        auto &decided = person_search_from_decisions[day];
        for (auto it = persons.begin(); it != persons.end(); ++it) {
            auto expected_person =
                decided.is_object() ? decided.value(it.key(), Json(nullptr)) : Json(nullptr);
            if (it.value().is_null() != expected_person.is_null())
                search_ok = false;
            else if (!it.value().is_null()) {
                for (auto key : {"outcome", "reason", "chosen"})
                    if (it.value().value(key, Json(nullptr)) != expected_person.at(key))
                        search_ok = false;
                if (manifest.at("trace") == "audit")
                    for (auto key : {"declined", "ineligible"})
                        if (it.value().value(key, Json(nullptr)) != expected_person.at(key))
                            search_ok = false;
            }
        }
    }
    check("job_search_metrics_reconcile", search_ok);
    bool replay_ok = true;
    Day replay_day = initial_state.next_day + 1;
    Json last_display;
    std::ifstream display_metrics(dir / "metrics.jsonl");
    scan_lines(dir / "replay.jsonl", [&](const Json &display) {
        if (display.at("schema") != "replay-1" || integer(display.at("next_day")) != replay_day ||
            replay_day > final_state.next_day)
            replay_ok = false;
        std::string metric_line;
        if (!std::getline(display_metrics, metric_line))
            replay_ok = false;
        else {
            auto metric = Json::parse(metric_line);
            auto accounts = display.at("accounts").get<std::vector<Account>>();
            if (accounts.size() != initial_state.accounts.size())
                replay_ok = false;
            for (auto &a : accounts) {
                auto cash_key = a.id.rfind("firm:", 0) == 0 ? "firm_cash" : "household_cash";
                if (metric.at("inventories").at(a.id) != Json(a.stock) ||
                    integer(metric.at(cash_key).at(a.id)) != a.cash)
                    replay_ok = false;
            }
        }
        ++replay_day;
        last_display = display;
    });
    if (replay_day != final_state.next_day + 1)
        replay_ok = false;
    if (final_state.next_day > initial_state.next_day &&
        (last_display.at("accounts") != Json(final_state.accounts) ||
         last_display.at("firms") != Json(final_state.firms)))
        replay_ok = false;
    check("display_export_boundaries_and_final_state", replay_ok);
    check("public_board_reconciles", reconstructed.public_board == final_state.public_board);
    Amount before = 0, after = 0;
    for (auto &a : initial_state.accounts)
        before = add(before, a.cash);
    for (auto &a : final_state.accounts)
        after = add(after, a.cash);
    check("closed_cash_conservation", before == after);
    bool passed = true;
    for (auto &c : checks)
        if (c.at("status") != "passed")
            passed = false;
    return {{"schema", "verification-1"},
            {"passed", passed},
            {"checks", checks},
            {"unavailable", {"independent_reexecution_of_household_beliefs", "empirical_economic_validity"}},
            {"events_scanned", event_count},
            {"periods_scanned", metric_count},
            {"total_cash", after},
            {"totals", totals}};
}
Json inspect(const fs::path &dir, const std::string &id) {
    auto j = read_json(dir / "final_state.json");
    auto s = j.at("state").get<State>();
    Json result = {
        {"schema", "inspection-1"},  {"boundary", s.next_day},      {"employment_mode", s.employment_mode},
        {"account", account(s, id)}, {"employment", Json::array()}, {"pending_changes", Json::array()}};
    for (auto &h : s.households)
        if (h.id == id)
            result["household"] = h;
    for (auto &f : s.firms)
        if (f.id == id)
            result["firm"] = f;
    for (auto &r : s.employment)
        if (r.household == id || r.employer == id)
            result["employment"].push_back(r);
    for (auto &c : s.pending_changes)
        if (c.household == id || c.from_employer == id || c.to_employer == id)
            result["pending_changes"].push_back(c);
    result["board"] = Json::array();
    for (auto &p : s.board)
        if (p.firm == id)
            result["board"].push_back(p);
    return result;
}
Json explain(const fs::path &dir, const std::string &id) {
    Json result;
    scan_lines(dir / "decisions.jsonl", [&](const Json &d) {
        if (d.at("id") == id)
            result = {{"schema", "explanation-1"}, {"decision", d}, {"events", Json::array()}};
    });
    if (result.is_null())
        throw Error(2, "unknown decision: " + id);
    scan_lines(dir / "events.jsonl", [&](const Json &e) {
        bool match = false;
        for (auto &event_id : result.at("decision").at("event_ids"))
            if (e.at("id") == event_id)
                match = true;
        for (auto &order_id : result.at("decision").at("order_ids"))
            if (e.at("data").contains("order")) {
                auto &order = e.at("data").at("order");
                if ((order.is_object() && order.at("id") == order_id) ||
                    (order.is_string() && order == order_id))
                    match = true;
            }
        if (match)
            result["events"].push_back(e);
    });
    return result;
}
Json compare(const fs::path &base, const fs::path &variant, Day from, Day until) {
    auto a = read_json(base / "manifest.json"), b = read_json(variant / "manifest.json");
    if (from < 0 || until <= from || from < integer(a.at("from_day")) || from < integer(b.at("from_day")) ||
        until > integer(a.at("until_day")) || until > integer(b.at("until_day")))
        throw Error(4, "comparison interval not shared");
    for (auto key : {"model", "origin", "seed", "scenario_digest"})
        if (a.at(key) != b.at(key))
            throw Error(4, std::string("incompatible comparison ") + key);
    if (a.at("status") != "complete" || b.at("status") != "complete")
        throw Error(4, "comparison requires completed runs");
    for (auto *m : {&a, &b})
        if (!m->at("intervention").is_null()) {
            auto type = m->at("intervention").at("type");
            if (type != "noop" && type != "public_price_board")
                throw Error(4, "unsupported paired intervention");
            if (integer(m->at("intervention").at("day")) > from)
                throw Error(4, "interval starts before intervention");
        }
    // Prove shared state at the branch boundary from parent metadata, or same initial state.
    auto initial_a = read_json(base / "initial_state.json"),
         initial_b = read_json(variant / "initial_state.json");
    const bool different_modes = a.at("employment") != b.at("employment");
    auto state_at = [&](const fs::path &dir, const Json &initial_record, Day day) -> std::string {
        auto state = initial_record.at("state").get<State>();
        if (state.next_day == day)
            return digest(Json(state));
        std::ostringstream p;
        p << "day-" << std::setfill('0') << std::setw(4) << day << ".json";
        auto cp = dir / "checkpoints" / p.str();
        if (!fs::exists(cp))
            throw Error(4, "comparison needs checkpoint at continuation origin");
        return load_checkpoint(cp).checkpoint_digest;
    };
    if (different_modes) {
        // Mode experiments share the entire scenario and day-zero endowment. Removing only the
        // treatment flag proves that no other initial condition changed. Continuations of different
        // modes have already diverged, so cannot establish this pairing on their own.
        auto state_a = initial_a.at("state"), state_b = initial_b.at("state");
        if (state_a.at("next_day") != 0 || state_b.at("next_day") != 0 || !a.at("parent").is_null() ||
            !b.at("parent").is_null() || !a.at("intervention").is_null() || !b.at("intervention").is_null())
            throw Error(4, "employment comparison requires unmodified runs from day zero");
        state_a.erase("employment_mode");
        state_b.erase("employment_mode");
        if (state_a != state_b)
            throw Error(4, "different initial conditions for employment comparison");
    } else if (initial_a.at("state").at("next_day") == initial_b.at("state").at("next_day")) {
        if (initial_a.at("digest") != initial_b.at("digest"))
            throw Error(4, "different initial state");
    } else {
        const auto &later = integer(a.at("from_day")) > integer(b.at("from_day")) ? a : b;
        const auto &earlier_dir = integer(a.at("from_day")) > integer(b.at("from_day")) ? variant : base;
        const auto &earlier_initial =
            integer(a.at("from_day")) > integer(b.at("from_day")) ? initial_b : initial_a;
        if (later.at("parent").is_null() ||
            state_at(earlier_dir, earlier_initial, integer(later.at("from_day"))) !=
                later.at("parent").at("checkpoint_digest").get<std::string>())
            throw Error(4, "continuation checkpoint does not match baseline");
    }
    if (!verify(base).at("passed").get<bool>() || !verify(variant).at("passed").get<bool>())
        throw Error(4, "comparison requires verified records");
    auto sum = [&](const fs::path &dir) {
        Json result = {{"production", Json::object()},
                       {"consumption", Json::object()},
                       {"unmet", Json::object()},
                       {"sales_cents", {{"retail", 0}, {"wholesale", 0}}},
                       {"wages", 0},
                       {"paid_workers", 0},
                       {"employed_person_days", 0},
                       {"unemployed_person_days", 0},
                       {"hires", 0},
                       {"separations", 0},
                       {"job_search", Json::object()},
                       {"labor_units", Json::object()},
                       {"periods", 0}};
        scan_lines(dir / "metrics.jsonl", [&](const Json &m) {
            auto day = integer(m.at("day"));
            if (day >= from && day < until) {
                for (auto key : {"production", "consumption", "unmet", "sales_cents", "labor_units"})
                    for (auto it = m.at(key).begin(); it != m.at(key).end(); ++it)
                        inc(result[key], it.key(), integer(it.value()));
                inc(result, "wages", integer(m.at("wages")));
                for (auto key : {"paid_workers", "hires", "separations"})
                    inc(result, key, integer(m.at(key)));
                inc(result, "employed_person_days", integer(m.at("employed")));
                inc(result, "unemployed_person_days", integer(m.at("unemployed")));
                for (auto key : {"searching", "applied", "hires", "switches", "quits", "declined",
                                 "ineligible", "no_postings"})
                    inc(result["job_search"], key, integer(m.at("job_search").at(key)));
                inc(result, "periods", 1);
            }
        });
        if (integer(result.at("periods")) != until - from)
            throw Error(4, "missing comparison periods");
        return result;
    };
    return {{"schema", "comparison-1"},
            {"model", model_version},
            {"from_day", from},
            {"employment", {{"baseline", a.at("employment")}, {"variant", b.at("employment")}}},
            {"comparison_kind", different_modes ? "employment_modes" : "shared_state"},
            {"until_day", until},
            {"baseline", sum(base)},
            {"variant", sum(variant)}};
}
} // namespace econ
