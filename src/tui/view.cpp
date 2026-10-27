#include <algorithm>
#include <cctype>
#include <charconv>
#include <econ/tui.hpp>
#include <limits>
#include <sstream>
namespace econ::tui {
namespace fs = std::filesystem;
Frame::Frame(int w, int h)
    : columns(std::clamp(w, 1, 399)), rows(std::clamp(h, 1, 120)),
      cells_(static_cast<std::size_t>(columns * rows)) {}
void Frame::put(int row, int col, const std::string &raw, Ink ink, int limit) {
    if (row < 0 || row >= rows || col < 0 || col >= columns)
        return;
    auto text = clean(raw);
    int room = std::min(columns - col, limit < 0 ? columns : limit);
    if (room <= 0)
        return;
    if (text.size() > static_cast<std::size_t>(room)) {
        text.resize(static_cast<std::size_t>(room));
        if (room >= 3)
            text.replace(text.size() - 3, 3, "...");
    }
    for (std::size_t i = 0; i < text.size(); ++i)
        cells_[static_cast<std::size_t>(row * columns + col) + i] = {text[i], ink};
}
void Frame::rule(int row) {
    put(row, 0, std::string(static_cast<std::size_t>(columns), '-'));
}
std::string Frame::plain() const {
    std::string out;
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < columns; ++c)
            out += cells_[static_cast<std::size_t>(r * columns + c)].text;
        if (r + 1 < rows)
            out += '\n';
    }
    return out;
}
std::string Frame::ansi(bool color) const {
    std::string out = "\x1b[H";
    Ink current = Ink::Normal;
    for (int r = 0; r < rows; ++r) {
        out += "\x1b[0m";
        current = Ink::Normal;
        for (int c = 0; c < columns; ++c) {
            auto cell = cells_[static_cast<std::size_t>(r * columns + c)];
            if (cell.ink != current) {
                switch (cell.ink) {
                case Ink::Normal:
                    out += "\x1b[0m";
                    break;
                case Ink::Bold:
                    out += "\x1b[0;1m";
                    break;
                case Ink::Accent:
                    out += color ? "\x1b[0;1;36m" : "\x1b[0;1m";
                    break;
                case Ink::Selected:
                    out += "\x1b[0;7m";
                    break;
                }
                current = cell.ink;
            }
            out += cell.text;
        }
        out += "\x1b[0m\x1b[K";
        if (r + 1 < rows)
            out += "\r\n";
    }
    return out;
}
static std::string text(const Json &j, const std::string &key, const std::string &fallback = "-") {
    if (!j.contains(key) || j.at(key).is_null())
        return fallback;
    return j.at(key).is_string() ? j.at(key).get<std::string>() : j.at(key).dump();
}
static std::string lower(std::string s) {
    for (auto &c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
static std::string counts(const Json &j) {
    std::string out;
    for (auto it = j.begin(); it != j.end(); ++it) {
        if (!out.empty())
            out += "  ";
        out += it.key() + "=" + number(integer(it.value()));
    }
    return out.empty() ? "none" : out;
}
static std::vector<std::string> wrap_lines(const std::vector<std::string> &lines, int width) {
    std::vector<std::string> result;
    auto columns = static_cast<std::size_t>(std::max(1, width));
    for (auto &raw : lines) {
        auto line = clean(raw);
        if (line.empty()) {
            result.emplace_back();
            continue;
        }
        while (line.size() > columns) {
            auto split = line.rfind(' ', columns);
            if (split == std::string::npos || split < columns / 2)
                split = columns;
            result.push_back(line.substr(0, split));
            line.erase(0, split);
            if (!line.empty() && line[0] == ' ')
                line.erase(0, 1);
        }
        if (!line.empty())
            result.push_back(std::move(line));
    }
    return result;
}
static bool actor_matches(const Json &event, const std::string &actor) {
    if (actor.empty())
        return true;
    auto &d = event.at("data");
    for (auto key : {"actor", "buyer", "seller", "employer", "household", "firm", "from_employer",
                     "to_employer", "supplier"})
        if (d.contains(key) && d.at(key) == actor)
            return true;
    if (d.contains("order") && d.at("order").is_object()) {
        auto &o = d.at("order");
        if (o.at("buyer") == actor || o.at("seller") == actor)
            return true;
    }
    for (auto &e : event.at("effects"))
        if (e.at("actor") == actor)
            return true;
    return false;
}
static std::string employer_name(const Json &j, const std::string &key) {
    auto value = text(j, key, "");
    return value.empty() ? "(none)" : value;
}
static std::string event_label(const Json &e) {
    auto &d = e.at("data");
    std::string s = text(e, "id") + "  " + text(e, "type") + "  ";
    if (d.contains("from_employer") && d.contains("to_employer")) {
        s += text(d, "household") + "  " + employer_name(d, "from_employer") + " -> " +
             employer_name(d, "to_employer");
        if (d.contains("effective"))
            s += "  effective day " + text(d, "effective");
        s += "  " + text(d, "reason");
        return s;
    }
    if (e.at("type") == "posting_opened")
        return s + text(d, "firm") + "  " + text(d.at("posting"), "id") + "  " +
               text(d.at("posting"), "openings") + " open at " + number(integer(d.at("posting").at("wage"))) +
               " c  " + text(d, "reason");
    if (e.at("type") == "posting_adjusted")
        return s + text(d, "firm") + "  " + text(d, "posting") + "  " + text(d, "field") + " " +
               text(d, "old") + " -> " + text(d, "new") + "  " + text(d, "reason");
    if (e.at("type") == "posting_closed")
        return s + text(d, "firm") + "  " + text(d, "posting") + "  " + text(d, "reason");
    if (d.contains("buyer"))
        s += text(d, "buyer") + " -> " + text(d, "seller");
    else if (d.contains("household"))
        s += text(d, "household");
    else if (d.contains("firm"))
        s += text(d, "firm");
    else if (d.contains("order") && d.at("order").is_object())
        s += text(d.at("order"), "id") + " " + text(d.at("order"), "buyer");
    if (d.contains("product"))
        s += "  " + text(d, "product");
    if (d.contains("quantity"))
        s += " x" + text(d, "quantity");
    if (d.contains("reason"))
        s += "  " + text(d, "reason");
    return s;
}
// What one recorded event did to one entity, from its effects: cash, labor and stock deltas.
static Json entity_deltas(const Json &event, const std::string &id) {
    Json deltas = {{"cash", 0},
                   {"reserved_cash", 0},
                   {"labor", 0},
                   {"stock", Json::object()},
                   {"reserved_stock", Json::object()}};
    for (auto &ef : event.at("effects")) {
        if (ef.at("actor") != id)
            continue;
        auto field = text(ef, "field");
        auto delta = integer(ef.at("delta"));
        if (field == "stock" || field == "reserved_stock")
            deltas[field][text(ef, "product")] =
                add(deltas[field].value(text(ef, "product"), Amount{0}), delta);
        else
            deltas[field] = add(deltas.value(field, Amount{0}), delta);
    }
    return deltas;
}
static std::string signed_number(Amount value) {
    return (value > 0 ? "+" : "") + number(value);
}
static std::string stock_changes(const Json &stock) {
    std::string out;
    for (auto it = stock.begin(); it != stock.end(); ++it) {
        if (integer(it.value()) == 0)
            continue;
        if (!out.empty())
            out += " ";
        out += it.key() + " " + signed_number(integer(it.value()));
    }
    return out;
}
static void add_units(Json &target, const std::string &product, Amount q) {
    target[product] = add(target.value(product, Amount{0}), q);
}
// Everything that happened to an entity on the selected day, aggregated from the events.
Json Inspector::day_flows(const std::string &id) const {
    Json f = {{"id", id},
              {"cash_in", 0},
              {"cash_out", 0},
              {"wages_paid", 0},
              {"wages_received", 0},
              {"sales", 0},
              {"purchases", 0},
              {"labor_delivered", 0},
              {"labor_used", 0},
              {"labor_released", 0},
              {"produced", Json::object()},
              {"sold", Json::object()},
              {"bought", Json::object()},
              {"consumed", Json::object()},
              {"unmet", Json::object()},
              {"transactions", 0}};
    auto inc = [&](const std::string &key, Amount n) { f[key] = add(integer(f.at(key)), n); };
    for (auto &e : events_) {
        if (!actor_matches(e, id))
            continue;
        inc("transactions", 1);
        auto type = text(e, "type");
        auto &d = e.at("data");
        auto deltas = entity_deltas(e, id);
        auto cash = integer(deltas.at("cash"));
        inc(cash >= 0 ? "cash_in" : "cash_out", cash >= 0 ? cash : -cash);
        if (type == "wage")
            inc(d.at("employer") == id ? "wages_paid" : "wages_received", integer(d.at("amount")));
        if (type == "trade") {
            if (d.at("seller") == id) {
                inc("sales", integer(d.at("amount")));
                add_units(f["sold"], text(d, "product"), integer(d.at("quantity")));
            } else if (d.at("buyer") == id) {
                inc("purchases", integer(d.at("amount")));
                add_units(f["bought"], text(d, "product"), integer(d.at("quantity")));
            }
        }
        if (type == "production" && d.at("firm") == id)
            add_units(f["produced"], text(d, "product"), integer(d.at("quantity")));
        if (type == "consumption" && d.at("household") == id)
            add_units(f["consumed"], text(d, "product"), integer(d.at("quantity")));
        if (type == "unmet" && d.at("household") == id)
            add_units(f["unmet"], text(d, "product"), integer(d.at("quantity")));
        auto labor = integer(deltas.at("labor"));
        if (type == "wage" && labor > 0)
            inc("labor_delivered", labor);
        if (type == "production" && labor < 0)
            inc("labor_used", -labor);
        if (type == "labor_released" && labor < 0)
            inc("labor_released", -labor);
    }
    return f;
}
// Cash at the start of the selected day: the boundary balance minus the day's recorded changes.
Money Inspector::opening_cash(const std::string &id) const {
    Money closing = 0;
    for (auto &a : snapshot_.at("accounts"))
        if (a.at("id") == id)
            closing = integer(a.at("cash"));
    for (auto &e : events_)
        closing -= integer(entity_deltas(e, id).at("cash"));
    return closing;
}
Inspector::Inspector(fs::path root, std::optional<fs::path> run) : root_(std::move(root)) {
    refresh_runs();
    if (run)
        open(*run);
}
void Inspector::refresh_runs() {
    auto selected_path =
        (view_ == View::Runs && selected_ < rows_.size()) ? rows_[selected_].id : std::string{};
    runs_ = discover(root_);
    selected_ = top_ = 0;
    rebuild_rows();
    for (std::size_t i = 0; i < rows_.size(); ++i)
        if (rows_[i].id == selected_path)
            selected_ = i;
}
void Inspector::open(const fs::path &path) {
    auto next = std::make_unique<RunData>(path);
    data_ = std::move(next);
    view_ = View::Overview;
    filter_.clear();
    entity_.clear();
    selected_ = top_ = 0;
    detail_ = help_ = false;
    set_boundary(data_->last);
    message_ = data_->saved_verification_error.empty()
                   ? "Opened recorded run. Press ? for controls; v to verify persisted records."
                   : "Saved verification is invalid. Press v for a fresh read-only check.";
}
void Inspector::select_view(View v) {
    if (v != View::Runs && !data_) {
        message_ = "Open a run first: select one and press Enter.";
        return;
    }
    view_ = v;
    selected_ = top_ = 0;
    detail_ = help_ = false;
    filter_.clear();
    entity_.clear();
    ledger_entity_.clear();
    rebuild_rows();
}
void Inspector::set_boundary(Day day) {
    if (!data_)
        return;
    day = std::clamp(day, data_->first, data_->last);
    auto previous_entity = ((view_ == View::Entities || (view_ == View::Ledger && ledger_entity_.empty())) &&
                            selected_ < rows_.size())
                               ? rows_[selected_].id
                               : std::string{};
    auto next_snapshot = data_->snapshot(day);
    auto next_metric = data_->metric(day);
    auto next_decisions = data_->decisions(day);
    auto next_events = data_->events(day);
    boundary_ = day;
    snapshot_ = std::move(next_snapshot);
    metric_ = std::move(next_metric);
    decisions_ = std::move(next_decisions);
    events_ = std::move(next_events);
    selected_ = top_ = 0;
    detail_ = false;
    overview_scroll_ = 0;
    rebuild_rows();
    if (!previous_entity.empty())
        for (std::size_t i = 0; i < rows_.size(); ++i)
            if (rows_[i].id == previous_entity)
                selected_ = i;
}
Json Inspector::entity_record(const std::string &id) const {
    Json result = {{"boundary", boundary_}, {"source", data_->snapshot_source(boundary_)}};
    for (auto &a : snapshot_.at("accounts"))
        if (a.at("id") == id)
            result["account"] = a;
    for (auto &h : snapshot_.at("households"))
        if (h.at("id") == id) {
            result["household"] = h;
            if (!h.contains("knowledge"))
                result["knowledge_note"] =
                    "This display snapshot does not contain household knowledge. Inspect this day's "
                    "decisions, or a checkpoint boundary. Final-state knowledge is not substituted.";
        }
    for (auto &f : snapshot_.at("firms"))
        if (f.at("id") == id)
            result["firm"] = f;
    return result;
}
void Inspector::rebuild_rows() {
    rows_.clear();
    auto match = [&](const std::string &s) {
        return filter_.empty() || lower(s).find(lower(filter_)) != std::string::npos;
    };
    if (view_ == View::Runs) {
        for (auto &run : runs_) {
            auto relative = run.path.lexically_relative(root_).string();
            if (relative.empty() || relative == ".")
                relative = run.path.filename().string();
            auto label = relative + "  [" + run.status + "]  " + std::to_string(run.from) + ".." +
                         std::to_string(run.until) + "  seed " + run.seed + "  " + run.trace + "  " +
                         run.employment;
            if (match(label))
                rows_.push_back({run.path.string(),
                                 label,
                                 {{"path", run.path.string()},
                                  {"status", run.status},
                                  {"from_day", run.from},
                                  {"until_day", run.until},
                                  {"trace", run.trace},
                                  {"employment", run.employment},
                                  {"seed", run.seed},
                                  {"error", run.error}}});
        }
    } else if (data_ && view_ == View::Entities) {
        for (auto &a : snapshot_.at("accounts")) {
            auto id = text(a, "id");
            auto kind = id.rfind("household:", 0) == 0 ? "household" : "firm";
            auto record = entity_record(id);
            auto label = id + "  " + kind + "  " + number(integer(a.at("cash"))) + " c";
            if (match(record.dump()))
                rows_.push_back({id, label, record});
        }
    } else if (data_ && view_ == View::Decisions) {
        for (auto &d : decisions_) {
            if (!entity_.empty() && d.at("actor") != entity_)
                continue;
            auto label = text(d, "id") + "  " + text(d, "actor") + "  " + text(d, "outcome") + "  " +
                         text(d, "product", "");
            if (match(d.dump()))
                rows_.push_back({text(d, "id"), label, d});
        }
    } else if (data_ && view_ == View::Events) {
        for (auto &e : events_)
            if (actor_matches(e, entity_) && match(e.dump()))
                rows_.push_back({text(e, "id"), event_label(e), e});
    } else if (data_ && view_ == View::Ledger && !ledger_entity_.empty()) {
        // Level 2: one entity's transactions for the day, with its running cash balance.
        auto balance = opening_cash(ledger_entity_);
        for (auto &e : events_) {
            if (!actor_matches(e, ledger_entity_))
                continue;
            auto deltas = entity_deltas(e, ledger_entity_);
            auto cash = integer(deltas.at("cash")), labor = integer(deltas.at("labor"));
            balance = add(balance, cash);
            auto &d = e.at("data");
            std::string what = text(e, "type");
            if (what == "trade" || what == "trade_rejected" || what == "order_reserved")
                what += " " + (d.contains("stage") ? text(d, "stage") : "");
            std::string label = text(e, "id") + "  " + what;
            if (cash)
                label += "  cash " + signed_number(cash);
            if (integer(deltas.at("reserved_cash")))
                label += "  reserved " + signed_number(integer(deltas.at("reserved_cash")));
            if (labor)
                label += "  labor " + signed_number(labor) +
                         (what == "labor_released" ? " (" + text(d, "use") + ")" : "");
            auto goods = stock_changes(deltas.at("stock"));
            if (!goods.empty())
                label += "  " + goods;
            auto reserved = stock_changes(deltas.at("reserved_stock"));
            if (!reserved.empty())
                label += "  reserved " + reserved;
            if (what == "unmet")
                label += "  " + text(d, "product") + " short " + text(d, "quantity");
            if (d.contains("reason"))
                label += "  " + text(d, "reason");
            label += "  -> " + number(balance) + " c";
            Json record = e;
            record["ledger"] = {{"entity", ledger_entity_}, {"deltas", deltas}, {"cash_after", balance}};
            if (match(label) || match(e.dump()))
                rows_.push_back({text(e, "id"), label, record});
        }
    } else if (data_ && view_ == View::Ledger) {
        // Level 1: every entity's day in one line; Enter drills into its transactions.
        for (auto &a : snapshot_.at("accounts")) {
            auto id = text(a, "id");
            auto f = day_flows(id);
            if (boundary_ == data_->first) {
                if (match(id))
                    rows_.push_back({id, id + "  opening state, no day completed", f});
                continue;
            }
            auto net = integer(f.at("cash_in")) - integer(f.at("cash_out"));
            auto label = id + "  cash " + number(integer(a.at("cash"))) + " (" + signed_number(net) + ")";
            if (id.rfind("firm:", 0) == 0) {
                if (integer(f.at("wages_paid")))
                    label += "  wages -" + number(integer(f.at("wages_paid")));
                if (integer(f.at("sales")))
                    label += "  sales +" + number(integer(f.at("sales")));
                if (integer(f.at("purchases")))
                    label += "  bought -" + number(integer(f.at("purchases")));
                if (integer(f.at("labor_delivered")))
                    label += "  labor " + text(f, "labor_delivered") + " used " + text(f, "labor_used");
                if (!f.at("produced").empty())
                    label += "  made " + counts(f.at("produced"));
                if (!f.at("sold").empty())
                    label += "  sold " + counts(f.at("sold"));
            } else {
                if (integer(f.at("wages_received")))
                    label += "  wage +" + number(integer(f.at("wages_received")));
                if (integer(f.at("purchases")))
                    label += "  spent -" + number(integer(f.at("purchases")));
                if (!f.at("bought").empty())
                    label += "  bought " + counts(f.at("bought"));
                if (!f.at("consumed").empty())
                    label += "  ate " + counts(f.at("consumed"));
                if (!f.at("unmet").empty())
                    label += "  short " + counts(f.at("unmet"));
            }
            if (match(label))
                rows_.push_back({id, label, f});
        }
    } else if (data_ && view_ == View::Labor) {
        // One row per person for the completed day: job held, work done, pay received, changes.
        if (metric_ && metric_->contains("labor")) {
            for (auto it = metric_->at("labor").begin(); it != metric_->at("labor").end(); ++it) {
                auto id = it.key();
                if (!entity_.empty() && id != entity_)
                    continue;
                Json person = it.value();
                person["household"] = id;
                person["day"] = boundary_ - 1;
                auto label =
                    id + "  " + employer_name(person, "employer") + "  " +
                    (person.value("worked", false) ? "paid " + number(integer(person.at("wage_paid"))) + " c"
                                                   : "idle");
                if (!person.at("change").is_null())
                    label += "  changed " + employer_name(person.at("change"), "from_employer") + "->" +
                             employer_name(person.at("change"), "to_employer");
                if (!person.at("scheduled").is_null())
                    label += "  scheduled ->" + employer_name(person.at("scheduled"), "to_employer") + " d" +
                             text(person.at("scheduled"), "effective");
                if (person.contains("cancelled") && !person.at("cancelled").is_null())
                    label += "  layoff cancelled";
                if (person.contains("search") && person.at("search").is_object() &&
                    person.at("search").at("chosen").is_null())
                    label += "  search: " + lower(text(person.at("search"), "outcome"));
                if (match(person.dump()))
                    rows_.push_back({id, label, person});
            }
            if (metric_->contains("board"))
                for (auto &p : metric_->at("board")) {
                    auto label = text(p, "id") + "  " + text(p, "firm") + "  " + text(p, "openings") +
                                 " open at " + number(integer(p.at("wage"))) + " c  unfilled " +
                                 text(p, "unfilled_days") + " d  " + text(p, "applications", "0") +
                                 " applications";
                    if (match(p.dump()))
                        rows_.push_back({text(p, "id"), label, p});
                }
        } else
            for (auto &h : snapshot_.at("households")) {
                auto id = text(h, "id");
                if (!entity_.empty() && id != entity_)
                    continue;
                Json person = {
                    {"household", id},
                    {"employer", text(h, "employer", "").empty() ? Json(nullptr) : h.at("employer")},
                    {"status", text(h, "employer", "").empty() ? "unemployed" : "employed"},
                    {"note", "Opening state: no day completed yet, so no work or payroll."}};
                if (match(person.dump()))
                    rows_.push_back(
                        {id, id + "  " + employer_name(person, "employer") + "  opening state", person});
            }
    }
    if (rows_.empty())
        selected_ = top_ = 0;
    else
        selected_ = std::min(selected_, rows_.size() - 1);
}
std::vector<std::string> Inspector::preview(const Row &row) const {
    std::vector<std::string> lines;
    if (view_ == View::Runs) {
        auto &j = row.record;
        lines = {text(j, "path"),
                 "Status: " + text(j, "status"),
                 "Days: [" + text(j, "from_day") + ", " + text(j, "until_day") + ")",
                 "Seed: " + text(j, "seed"),
                 "Trace: " + text(j, "trace"),
                 "Employment: " + text(j, "employment"),
                 "",
                 text(j, "error", "")};
    } else if (view_ == View::Entities) {
        auto &a = row.record.at("account");
        lines = {row.id,
                 "Cash: " + number(integer(a.at("cash"))) + " c",
                 "Reserved: " + number(integer(a.at("reserved_cash"))) + " c",
                 "Available: " + number(integer(a.at("cash")) - integer(a.at("reserved_cash"))) + " c",
                 "Stock: " + counts(a.at("stock")),
                 "Reserved goods: " + counts(a.at("reserved_stock"))};
        if (row.record.contains("household")) {
            auto &h = row.record.at("household");
            lines.push_back(
                "Employer: " + employer_name(h, "employer") +
                (h.contains("unpaid_days") ? " | unpaid streak " + text(h, "unpaid_days") + " d" : ""));
            if (snapshot_.contains("pending_changes"))
                for (auto &c : snapshot_.at("pending_changes"))
                    if (c.at("household") == row.id)
                        lines.push_back("Pending: " + employer_name(c, "from_employer") + " -> " +
                                        employer_name(c, "to_employer") + " on day " + text(c, "effective") +
                                        " (" + text(c, "reason") + ")");
            if (h.contains("job_preferences")) {
                auto &jp = h.at("job_preferences");
                lines.push_back("Job preferences: reservation " + number(integer(jp.at("reservation_wage"))) +
                                " c | gain >= " + text(jp, "min_wage_gain_percent") + "% and " +
                                number(integer(jp.at("min_wage_gain"))) +
                                " c | tenure >= " + text(jp, "min_tenure_days") + " d | " +
                                (jp.at("search_while_employed").get<bool>() ? "searches while employed"
                                                                            : "stays put while paid") +
                                " | unpaid tolerance " + text(jp, "unpaid_tolerance_days") + " d");
            }
            lines.push_back("Needs: " + counts(h.at("needs")));
            lines.push_back(h.contains("knowledge")
                                ? "Knowledge: " + std::to_string(h.at("knowledge").size()) + " observations"
                                : "Knowledge: see daily decisions");
        } else if (row.record.contains("firm")) {
            auto &f = row.record.at("firm");
            lines.push_back("Role: " + text(f, "kind"));
            lines.push_back("Prices c/unit: " + counts(f.at("prices")));
            lines.push_back("Wage: " + text(f, "wage") + " c / worker");
            lines.push_back("Positions: " + text(f, "positions") + " | labor required " +
                            text(f, "labor_required") + " | output/worker " + text(f, "output_per_worker") +
                            " | missed payrolls " + text(f, "payroll_missed_days"));
            if (f.contains("demand_history"))
                lines.push_back("Observed demand (units/day, oldest first): " +
                                (f.at("demand_history").empty() ? std::string("none yet")
                                                                : f.at("demand_history").dump()));
            if (f.contains("hiring")) {
                auto &hp = f.at("hiring");
                lines.push_back("Offers: " + text(hp, "wage_min") + ".." + text(hp, "wage_max") + " step " +
                                text(hp, "wage_step") + " | raise after " +
                                text(hp, "raise_after_unfilled_days") + " unfilled d | needs " +
                                text(hp, "min_experience_days") + " d experience");
            }
            if (snapshot_.contains("board"))
                for (auto &p : snapshot_.at("board"))
                    if (p.at("firm") == row.id)
                        lines.push_back("Posting " + text(p, "id") + ": " + text(p, "openings") +
                                        " open at " + number(integer(p.at("wage"))) + " c (" +
                                        text(p, "reason") + ")");
            lines.push_back("Price rule: " + text(f, "price_policy"));
        }
        lines.push_back("");
        lines.push_back("d decisions / e events for this actor");
    } else if (view_ == View::Decisions) {
        auto &d = row.record;
        lines = {row.id,
                 "Actor: " + text(d, "actor"),
                 "Policy: " + text(d, "policy"),
                 "Outcome: " + text(d, "outcome"),
                 "Reason: " + text(d, "reason"),
                 "Chosen: " + text(d, "chosen", "no action"),
                 "Product: " + text(d, "product"),
                 "Linked events: " + std::to_string(d.at("event_ids").size())};
        if (d.contains("omitted") && !d.at("omitted").empty())
            lines.push_back("Summary trace: candidate detail omitted");
        else
            lines.push_back("Audit evidence available");
        lines.push_back("Enter: evidence + linked events (whole run)");
    } else if (view_ == View::Ledger && !ledger_entity_.empty()) {
        auto &e = row.record;
        auto &l = e.at("ledger");
        lines = {row.id + "  " + text(e, "phase") + " / " + text(e, "type"), "Entity: " + text(l, "entity"),
                 "Cash: " + signed_number(integer(l.at("deltas").at("cash"))) + " c -> " +
                     number(integer(l.at("cash_after"))) + " c after this transaction",
                 "Labor: " + signed_number(integer(l.at("deltas").at("labor"))) + " worker-days",
                 "Goods: " + (stock_changes(l.at("deltas").at("stock")).empty()
                                  ? "none"
                                  : stock_changes(l.at("deltas").at("stock")))};
        auto &d = e.at("data");
        if (d.contains("buyer"))
            lines.push_back("Counterparty: " +
                            (d.at("buyer") == text(l, "entity") ? text(d, "seller") : text(d, "buyer")) +
                            " | " + text(d, "quantity") + " " + text(d, "product") + " at " +
                            text(d, "price") + " c");
        if (d.contains("employer") && d.contains("household"))
            lines.push_back("Counterparty: " + (d.at("employer") == text(l, "entity") ? text(d, "household")
                                                                                      : text(d, "employer")));
        if (d.contains("reason"))
            lines.push_back("Reason: " + text(d, "reason"));
        lines.push_back("");
        lines.push_back("Enter: full recorded event | Esc: back to all entities");
    } else if (view_ == View::Ledger) {
        auto &f = row.record;
        auto net = integer(f.at("cash_in")) - integer(f.at("cash_out"));
        lines = {row.id + "  day " + std::to_string(boundary_ - 1),
                 "Cash in " + number(integer(f.at("cash_in"))) + " c | out " +
                     number(integer(f.at("cash_out"))) + " c | net " + signed_number(net) + " c | opening " +
                     number(opening_cash(row.id)) + " c"};
        if (row.id.rfind("firm:", 0) == 0) {
            lines.push_back("Payroll: " + number(integer(f.at("wages_paid"))) + " c for " +
                            text(f, "labor_delivered") + " worker-days | used in production " +
                            text(f, "labor_used") + " | released " + text(f, "labor_released"));
            lines.push_back("Produced: " + counts(f.at("produced")));
            lines.push_back("Sold: " + counts(f.at("sold")) + " for " + number(integer(f.at("sales"))) +
                            " c");
            lines.push_back("Bought: " + counts(f.at("bought")) + " for " +
                            number(integer(f.at("purchases"))) + " c");
        } else {
            lines.push_back("Wages received: " + number(integer(f.at("wages_received"))) + " c");
            lines.push_back("Bought: " + counts(f.at("bought")) + " for " +
                            number(integer(f.at("purchases"))) + " c");
            lines.push_back("Consumed: " + counts(f.at("consumed")) + " | unmet: " + counts(f.at("unmet")));
        }
        lines.push_back("Transactions: " + text(f, "transactions"));
        lines.push_back("");
        lines.push_back("Enter: this entity's transactions | d decisions / e events");
    } else if (view_ == View::Labor && row.id.rfind("posting:", 0) == 0) {
        auto &p = row.record;
        lines = {row.id + "  vacancy board entry of " + text(p, "firm"),
                 "Openings: " + text(p, "openings") + " at " + number(integer(p.at("wage"))) + " c / day",
                 "Unfilled for " + text(p, "unfilled_days") + " days | posted day " + text(p, "posted") +
                     " | applications so far " + text(p, "applications", "0"),
                 "Requires: experience >= " + text(p, "min_experience_days") + " days; " +
                     (p.at("accept_employed").get<bool>() ? "employed applicants accepted"
                                                          : "unemployed applicants only") +
                     "; rehire cooldown " + text(p, "rehire_cooldown_days") + " days",
                 "Posted because: " + text(p, "reason"),
                 "",
                 "Enter: full record | e events for this firm"};
    } else if (view_ == View::Labor) {
        auto &p = row.record;
        lines = {row.id + (p.contains("day") ? "  day " + text(p, "day") : ""),
                 "Status: " + text(p, "status") +
                     (p.at("employer").is_null() ? "" : " at " + text(p, "employer"))};
        if (p.contains("worked")) {
            lines.push_back(std::string("Worked: ") + (p.at("worked").get<bool>() ? "yes" : "no") +
                            (p.at("idle_reason").is_null() ? "" : " (" + text(p, "idle_reason") + ")"));
            lines.push_back("Wage paid: " + number(integer(p.at("wage_paid"))) + " c");
            lines.push_back("Unpaid streak: " + text(p, "unpaid_days") + " days");
            lines.push_back(p.at("change").is_null()
                                ? "Change today: none"
                                : "Change today: " + employer_name(p.at("change"), "from_employer") + " -> " +
                                      employer_name(p.at("change"), "to_employer") + " (" +
                                      text(p.at("change"), "reason") + ")");
            lines.push_back(p.at("scheduled").is_null()
                                ? "Scheduled today: none"
                                : "Scheduled today: " + employer_name(p.at("scheduled"), "from_employer") +
                                      " -> " + employer_name(p.at("scheduled"), "to_employer") +
                                      " effective day " + text(p.at("scheduled"), "effective") + " (" +
                                      text(p.at("scheduled"), "reason") + ")");
            if (!p.at("scheduled").is_null() &&
                !p.at("scheduled").at("to_employer").get<std::string>().empty())
                lines.push_back("  at " + number(integer(p.at("scheduled").at("wage"))) + " c via " +
                                text(p.at("scheduled"), "posting"));
            if (p.contains("cancelled") && !p.at("cancelled").is_null())
                lines.push_back("Cancelled today: layoff " + text(p.at("cancelled"), "id") + " (" +
                                text(p.at("cancelled"), "reason") + ")");
            if (p.contains("search") && p.at("search").is_object()) {
                auto &sr = p.at("search");
                lines.push_back("Search today: " + text(sr, "outcome") + " (" + text(sr, "reason") + ")" +
                                (sr.at("chosen").is_null() ? "" : " -> " + text(sr, "chosen")) +
                                "; declined " + text(sr, "declined", "?") + ", ineligible " +
                                text(sr, "ineligible", "?"));
            } else
                lines.push_back("Search today: did not read the board");
        } else
            lines.push_back(text(p, "note"));
        if (snapshot_.contains("pending_changes"))
            for (auto &c : snapshot_.at("pending_changes"))
                if (c.at("household") == row.id)
                    lines.push_back("Pending at boundary: " + employer_name(c, "from_employer") + " -> " +
                                    employer_name(c, "to_employer") + " on day " + text(c, "effective"));
        lines.push_back("");
        lines.push_back("Enter: full record | d decisions / e events for this person");
    } else {
        auto &e = row.record;
        lines = {row.id, "Phase: " + text(e, "phase"), "Type: " + text(e, "type"),
                 "Effects: " + std::to_string(e.at("effects").size())};
        auto &d = e.at("data");
        if (d.contains("from_employer") && d.contains("to_employer")) {
            lines.push_back("Household: " + text(d, "household"));
            lines.push_back("Job: " + employer_name(d, "from_employer") + " -> " +
                            employer_name(d, "to_employer"));
            if (d.contains("effective"))
                lines.push_back("Effective day: " + text(d, "effective") + " (scheduled, not yet in force)");
            else
                lines.push_back("In force from this day's employment phase, before work and payroll");
            lines.push_back("Reason: " + text(d, "reason"));
        } else if (text(e, "type") == "posting_opened") {
            auto &p = d.at("posting");
            lines.push_back("Posting " + text(p, "id") + " by " + text(d, "firm") + ": " +
                            text(p, "openings") + " open at " + number(integer(p.at("wage"))) + " c");
            lines.push_back("Requires experience >= " + text(p, "min_experience_days") + " d; " +
                            (p.at("accept_employed").get<bool>() ? "employed may apply" : "unemployed only") +
                            "; rehire cooldown " + text(p, "rehire_cooldown_days") + " d");
            lines.push_back("Reason: " + text(d, "reason"));
        } else if (text(e, "type") == "posting_adjusted") {
            lines.push_back("Posting " + text(d, "posting") + " by " + text(d, "firm"));
            lines.push_back((text(d, "field") == "wage" ? "Offer: " : "Openings: ") + text(d, "old") +
                            " -> " + text(d, "new"));
            lines.push_back("Reason: " + text(d, "reason"));
        } else if (text(e, "type") == "posting_closed") {
            lines.push_back("Posting " + text(d, "posting") + " by " + text(d, "firm") + " withdrawn");
            lines.push_back("Reason: " + text(d, "reason"));
        } else if (text(e, "type") == "supply_shortfall")
            lines.push_back(text(d, "supplier") + " could supply only " + text(d, "supplied") + " of " +
                            text(d, "desired") + " " + text(d, "product") + " wanted by " + text(d, "firm"));
        for (auto &ef : e.at("effects"))
            lines.push_back(text(ef, "actor") + " " + text(ef, "field") + " " + text(ef, "product", "") +
                            " " + number(integer(ef.at("delta"))));
        lines.push_back("Enter: full recorded event");
    }
    return lines;
}
void Inspector::show_json(const std::string &title, const Json &j) {
    detail_title_ = title;
    detail_lines_.clear();
    std::istringstream in(j.dump(2, ' ', true));
    std::string line;
    while (std::getline(in, line))
        detail_lines_.push_back(line);
    detail_scroll_ = 0;
    detail_ = true;
    help_ = false;
}
void Inspector::activate() {
    if (rows_.empty()) {
        message_ = "Nothing selected. Clear the filter with Esc or choose another day.";
        return;
    }
    auto &row = rows_[selected_];
    if (view_ == View::Runs)
        open(row.id);
    else if (view_ == View::Decisions)
        show_json("Decision evidence + linked events (whole recorded run)",
                  explain(data_->directory, row.id));
    else if (view_ == View::Ledger && ledger_entity_.empty()) {
        if (boundary_ == data_->first) {
            message_ = "No day completed at the opening boundary; press ] for the first recorded day.";
            return;
        }
        ledger_entity_ = row.id;
        selected_ = top_ = 0;
        rebuild_rows();
    } else if (view_ == View::Ledger)
        show_json("Recorded transaction (" + ledger_entity_ + ")", row.record);
    else
        show_json(view_ == View::Entities ? "Entity at boundary " + std::to_string(boundary_)
                  : view_ == View::Labor  ? "Labor record (job, work, payroll, changes) for one day"
                                          : "Recorded event",
                  row.record);
}
void Inspector::submit_prompt() {
    auto kind = prompt_;
    prompt_.clear();
    auto input = input_;
    input_.clear();
    if (form_) {
        auto &v = form_values_;
        if (kind == "Scenario")
            v.scenario = input;
        else if (kind == "Seed")
            v.seed = input;
        else if (kind == "Days")
            v.days = input;
        else if (kind == "Checkpoint every")
            v.checkpoint_every = input;
        else if (kind == "Run name") {
            v.name = input;
            v.name_edited = !input.empty();
        }
        if (!v.name_edited)
            v.name = fs::path(v.scenario).stem().string() + "-" + v.employment + "-seed" + v.seed + "-" +
                     v.days + "d";
        message_.clear();
    } else if (kind == "Filter") {
        filter_ = input;
        selected_ = top_ = 0;
        rebuild_rows();
        message_.clear();
    } else if (kind == "Boundary") {
        Day day = 0;
        auto [end, error] = std::from_chars(input.data(), input.data() + input.size(), day);
        if (error != std::errc{} || end != input.data() + input.size() || day < data_->first ||
            day > data_->last) {
            message_ = "Enter a boundary between " + std::to_string(data_->first) + " and " +
                       std::to_string(data_->last) + ".";
            return;
        }
        set_boundary(day);
        message_ = "Moved to recorded boundary " + std::to_string(day) + ".";
    } else if (kind == "Find ID") {
        if (input.rfind("household:", 0) == 0 || input.rfind("firm:", 0) == 0) {
            select_view(View::Entities);
            bool found = false;
            for (std::size_t i = 0; i < rows_.size(); ++i)
                if (rows_[i].id == input) {
                    selected_ = i;
                    found = true;
                    break;
                }
            if (!found)
                message_ = "No recorded entity with ID " + input;
        } else {
            auto record = data_->find_record(input);
            if (!record) {
                message_ = "No recorded ID " + input + ". Use household:N, firm:N, decision:N, or event:N.";
                return;
            }
            set_boundary(integer(record->at("day")) + 1);
            select_view(input.rfind("decision:", 0) == 0 ? View::Decisions : View::Events);
            for (std::size_t i = 0; i < rows_.size(); ++i)
                if (rows_[i].id == input) {
                    selected_ = i;
                    break;
                }
            message_ = "Found " + input + " on day " + text(*record, "day") + ".";
        }
    }
}
bool Inspector::key(const std::string &key) {
    try {
        if (key == "CTRL_C" || key == "CTRL_D")
            return false;
        if (!prompt_.empty()) {
            if (key == "ESC") {
                prompt_.clear();
                input_.clear();
                message_ = "Cancelled.";
            } else if (key == "ENTER")
                submit_prompt();
            else if (key == "BACKSPACE") {
                if (!input_.empty())
                    input_.pop_back();
            } else if (key.size() == 1 && key[0] >= 32 && key[0] < 127 && input_.size() < 160)
                input_ += key;
            return true;
        }
        if (key == "q" || key == "CTRL_C" || key == "CTRL_D")
            return false;
        if (key == "ESC") {
            if (help_)
                help_ = false;
            else if (form_) {
                form_ = false;
                message_ = "New-run form closed; nothing was written.";
            } else if (detail_)
                detail_ = false;
            else if (!filter_.empty() || !entity_.empty()) {
                filter_.clear();
                entity_.clear();
                selected_ = top_ = 0;
                rebuild_rows();
            } else if (!ledger_entity_.empty()) {
                auto previous = ledger_entity_;
                ledger_entity_.clear();
                selected_ = top_ = 0;
                rebuild_rows();
                for (std::size_t i = 0; i < rows_.size(); ++i)
                    if (rows_[i].id == previous)
                        selected_ = i;
            }
            return true;
        }
        if (key == "?") {
            help_ = !help_;
            detail_ = false;
            detail_scroll_ = 0;
            return true;
        }
        if (help_ && key == "ENTER") {
            help_ = false;
            return true;
        }
        if (detail_ || help_) {
            if (key == "DOWN" || key == "j")
                detail_scroll_ += detail_scroll_ < std::numeric_limits<std::size_t>::max() ? 1 : 0;
            else if (key == "UP" || key == "k") {
                if (detail_scroll_)
                    --detail_scroll_;
            } else if (key == "PAGEDOWN")
                detail_scroll_ = std::min(detail_scroll_, std::numeric_limits<std::size_t>::max() -
                                                              static_cast<std::size_t>(page_size_)) +
                                 static_cast<std::size_t>(page_size_);
            else if (key == "PAGEUP")
                detail_scroll_ = detail_scroll_ > static_cast<std::size_t>(page_size_)
                                     ? detail_scroll_ - static_cast<std::size_t>(page_size_)
                                     : 0;
            else if (key == "HOME")
                detail_scroll_ = 0;
            else if (key == "END")
                detail_scroll_ = std::numeric_limits<std::size_t>::max();
            return true;
        }
        if (form_ && !help_) {
            form_key(key);
            return true;
        }
        if (key.size() == 1 && key[0] >= '1' && key[0] < '1' + view_count) {
            select_view(static_cast<View>(key[0] - '1'));
            return true;
        }
        if (key == "TAB" || key == "BACKTAB") {
            auto value = (static_cast<int>(view_) + (key == "TAB" ? 1 : view_count - 1)) % view_count;
            select_view(static_cast<View>(value));
            return true;
        }
        if (key == "n" && view_ == View::Runs) {
            open_form();
            return true;
        }
        if (key == "o") {
            select_view(View::Runs);
            return true;
        }
        if (key == "R") {
            if (view_ == View::Runs) {
                refresh_runs();
                message_ = "Run list refreshed.";
            } else if (data_) {
                auto day = boundary_;
                auto view = view_;
                auto path = data_->directory;
                open(path);
                view_ = view;
                set_boundary(std::clamp(day, data_->first, data_->last));
                message_ = "Reloaded recorded artifacts.";
            }
            return true;
        }
        if (key == "/") {
            if (view_ == View::Overview)
                select_view(View::Entities);
            prompt_ = "Filter";
            input_ = filter_;
            return true;
        }
        if (data_ && view_ != View::Runs) {
            if (key == "[" || key == "LEFT" || key == "h")
                set_boundary(boundary_ > data_->first ? boundary_ - 1 : boundary_);
            else if (key == "]" || key == "RIGHT" || key == "l")
                set_boundary(boundary_ < data_->last ? boundary_ + 1 : boundary_);
            else if (key == "G")
                set_boundary(data_->last);
            else if (key == "0")
                set_boundary(data_->first);
            else if (key == "g") {
                prompt_ = "Boundary";
                input_.clear();
            } else if (key == "i") {
                prompt_ = "Find ID";
                input_.clear();
            } else if (key == "m")
                show_json("Recorded manifest", data_->manifest);
            else if (key == "v") {
                auto report = verify(data_->directory);
                message_ = report.at("passed").get<bool>()
                               ? "Independent verification PASSED (read-only)."
                               : "Independent verification FAILED; inspect the results.";
                show_json("Independent verification (performed now; nothing written)", report);
            } else if ((key == "d" || key == "e") &&
                       (view_ == View::Entities || view_ == View::Labor ||
                        (view_ == View::Ledger && ledger_entity_.empty())) &&
                       !rows_.empty()) {
                auto actor = rows_[selected_].id;
                if (actor.rfind("posting:", 0) == 0)
                    actor = text(rows_[selected_].record, "firm");
                select_view(key == "d" ? View::Decisions : View::Events);
                entity_ = actor;
                rebuild_rows();
            }
        }
        if (key == "ENTER" && view_ != View::Overview)
            activate();
        else if (view_ == View::Overview) {
            if (key == "DOWN" || key == "j")
                ++overview_scroll_;
            else if (key == "UP" || key == "k") {
                if (overview_scroll_)
                    --overview_scroll_;
            } else if (key == "PAGEDOWN")
                overview_scroll_ += static_cast<std::size_t>(page_size_);
            else if (key == "PAGEUP")
                overview_scroll_ = overview_scroll_ > static_cast<std::size_t>(page_size_)
                                       ? overview_scroll_ - static_cast<std::size_t>(page_size_)
                                       : 0;
            else if (key == "HOME")
                overview_scroll_ = 0;
            else if (key == "END")
                overview_scroll_ = 100000;
        } else if (!rows_.empty()) {
            if (key == "DOWN" || key == "j")
                selected_ = std::min(selected_ + 1, rows_.size() - 1);
            else if (key == "UP" || key == "k") {
                if (selected_)
                    --selected_;
            } else if (key == "PAGEDOWN")
                selected_ = std::min(selected_ + static_cast<std::size_t>(page_size_), rows_.size() - 1);
            else if (key == "PAGEUP")
                selected_ = selected_ > static_cast<std::size_t>(page_size_)
                                ? selected_ - static_cast<std::size_t>(page_size_)
                                : 0;
            else if (key == "HOME")
                selected_ = 0;
            else if (key == "END")
                selected_ = rows_.size() - 1;
        }
    } catch (const std::exception &e) {
        message_ = "Cannot complete action: " + clean(e.what()) + ". R reloads; o opens run list.";
    }
    return true;
}
void Inspector::open_form() {
    form_ = true;
    form_field_ = 0;
    help_ = detail_ = false;
    scenario_choices_.clear();
    // Scenarios usually live beside the runs directory (the project layout) or in the working directory.
    for (auto base : {fs::absolute(root_).parent_path() / "scenarios", fs::current_path() / "scenarios"}) {
        std::error_code ec;
        if (!fs::is_directory(base, ec))
            continue;
        for (auto &entry : fs::directory_iterator(base, ec))
            if (entry.is_regular_file() && entry.path().extension() == ".json")
                scenario_choices_.push_back(entry.path());
        if (!scenario_choices_.empty())
            break;
    }
    std::sort(scenario_choices_.begin(), scenario_choices_.end());
    auto &v = form_values_;
    if (v.scenario.empty()) {
        v.scenario = "scenarios/demo_town.json";
        if (!scenario_choices_.empty()) {
            v.scenario = scenario_choices_.front().string();
            for (auto &choice : scenario_choices_)
                if (choice.filename() == "demo_town.json")
                    v.scenario = choice.string();
        }
    }
    if (!v.name_edited)
        v.name =
            fs::path(v.scenario).stem().string() + "-" + v.employment + "-seed" + v.seed + "-" + v.days + "d";
    message_ = "New run: choose fixed or dynamic employment, then start. Output: " +
               (fs::absolute(root_) / v.name).string();
}
std::vector<std::string> Inspector::form_lines() const {
    auto &v = form_values_;
    const std::vector<std::pair<std::string, std::string>> fields = {
        {"Scenario",
         v.scenario + (scenario_choices_.empty() ? "" : "   (Left/Right cycles found scenarios)")},
        {"Seed", v.seed},
        {"Days", v.days + "   (exclusive final day)"},
        {"Employment", v.employment == "fixed"
                           ? "[fixed]  dynamic   (fixed: scenario jobs never change)"
                           : " fixed  [dynamic]  (dynamic: staffing targets, vacancy board, hires, quits)"},
        {"Trace", v.trace == "audit" ? "[audit]  summary" : " audit  [summary]"},
        {"Checkpoint every", v.checkpoint_every + " days   (0 = none)"},
        {"Run name", v.name},
        {"Start run", "writes " + (fs::absolute(root_) / v.name).string()}};
    std::vector<std::string> lines = {"New simulation run (existing runs are never touched)"};
    for (std::size_t i = 0; i < fields.size(); ++i) {
        auto label = fields[i].first;
        label.resize(18, ' ');
        lines.push_back((i == form_field_ ? "> " : "  ") + label + fields[i].second);
    }
    lines.push_back("");
    lines.push_back("Employment is recorded in the run manifest and every checkpoint; resume and branch");
    lines.push_back("inherit it. The same choice is available on the command line: --employment MODE.");
    return lines;
}
void Inspector::form_key(const std::string &key) {
    constexpr std::size_t fields = 8, start = 7;
    auto &v = form_values_;
    auto regenerate = [&] {
        if (!v.name_edited)
            v.name = fs::path(v.scenario).stem().string() + "-" + v.employment + "-seed" + v.seed + "-" +
                     v.days + "d";
    };
    if (key == "DOWN" || key == "j" || key == "TAB")
        form_field_ = (form_field_ + 1) % fields;
    else if (key == "UP" || key == "k" || key == "BACKTAB")
        form_field_ = (form_field_ + fields - 1) % fields;
    else if (key == "HOME")
        form_field_ = 0;
    else if (key == "END")
        form_field_ = start;
    else if (key == "LEFT" || key == "RIGHT" || key == " " || key == "h" || key == "l") {
        if (form_field_ == 3)
            v.employment = v.employment == "fixed" ? "dynamic" : "fixed";
        else if (form_field_ == 4)
            v.trace = v.trace == "audit" ? "summary" : "audit";
        else if (form_field_ == 0 && !scenario_choices_.empty()) {
            std::size_t current = 0;
            for (std::size_t i = 0; i < scenario_choices_.size(); ++i)
                if (scenario_choices_[i].string() == v.scenario)
                    current = i;
            auto n = scenario_choices_.size();
            current = (key == "LEFT" || key == "h") ? (current + n - 1) % n : (current + 1) % n;
            v.scenario = scenario_choices_[current].string();
        }
        regenerate();
    } else if (key == "ENTER") {
        if (form_field_ == start)
            launch_form();
        else if (form_field_ == 3 || form_field_ == 4)
            form_key("RIGHT");
        else {
            const char *labels[] = {"Scenario", "Seed", "Days", "", "", "Checkpoint every", "Run name"};
            const std::string *values[] = {&v.scenario,         &v.seed, &v.days, nullptr, nullptr,
                                           &v.checkpoint_every, &v.name};
            prompt_ = labels[form_field_];
            input_ = *values[form_field_];
        }
    }
}
void Inspector::launch_form() {
    auto &v = form_values_;
    auto parse = [](const std::string &label, const std::string &value) {
        Amount n = 0;
        auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), n);
        if (error != std::errc{} || end != value.data() + value.size() || n < 0)
            throw Error(2, label + " must be a nonnegative integer");
        return n;
    };
    if (v.name.empty() || v.name.find('/') != std::string::npos || v.name == "." || v.name == "..")
        throw Error(2, "Run name must be a single directory name");
    RunOptions options;
    options.out = fs::absolute(root_) / v.name;
    options.until = parse("Days", v.days);
    options.checkpoint_every = parse("Checkpoint every", v.checkpoint_every);
    options.trace = v.trace;
    options.employment = v.employment;
    if (fs::exists(options.out))
        throw Error(3, "Output already exists: " + options.out.string() + " (change the run name)");
    auto seed = static_cast<std::uint64_t>(parse("Seed", v.seed));
    auto scenario = read_json(v.scenario);
    auto result = run(scenario, seed, options);
    form_ = false;
    refresh_runs();
    open(options.out);
    message_ =
        "Run complete with " + v.employment + " employment: " + options.out.string() +
        (result.at("verification").at("passed").get<bool>() ? " (verified)" : " (verification FAILED)");
}
std::string Inspector::wait_message(const std::string &key) const {
    if (prompt_.empty() && form_ && key == "ENTER" && form_field_ == 7)
        return "Running " + form_values_.employment + "-employment simulation for " + form_values_.days +
               " days... (the terminal is restored when it finishes)";
    if (!form_ && (key == "ENTER" || key == "v" || key == "R"))
        return "Reading recorded artifacts...";
    return "";
}
Frame Inspector::render(int width, int height) {
    Frame f(std::max(1, width - 1), height);
    if (width < 60 || height < 18) {
        f.put(0, 0, "TOTOCONOMY / Simulation inspector", Ink::Bold);
        f.put(2, 0, "Terminal too small. Resize to at least 60 x 18.");
        f.put(4, 0, "Ctrl-C quits | current " + std::to_string(width) + " x " + std::to_string(height));
        return f;
    }
    auto w = f.columns;
    auto h = f.rows;
    f.put(0, 1, "TOTOCONOMY / Simulation inspector", Ink::Accent);
    f.put(0, w - 12, form_ ? "NEW RUN" : "READ ONLY", Ink::Bold);
    f.put(1, 1, data_ ? data_->directory.string() : "Run browser: " + fs::absolute(root_).string());
    f.put(2, 1, message_);
    const bool compact = w < 100;
    const std::vector<std::string> tabs =
        compact ? std::vector<std::string>{"1Runs", "2Town", "3Ents", "4Decs", "5Events", "6Labor", "7Ledger"}
                : std::vector<std::string>{"1 Runs",   "2 Overview", "3 Entities", "4 Decisions",
                                           "5 Events", "6 Labor",    "7 Ledger"};
    int x = 1;
    for (std::size_t i = 0; i < tabs.size(); ++i) {
        f.put(3, x, " " + tabs[i] + " ",
              static_cast<int>(view_) == static_cast<int>(i) ? Ink::Selected : Ink::Normal);
        x += static_cast<int>(tabs[i].size()) + (compact ? 2 : 3);
    }
    f.rule(4);
    if (data_ && view_ != View::Runs) {
        f.put(
            5, 1,
            "Boundary " + std::to_string(boundary_) + " / " + std::to_string(data_->last) + "  " +
                (boundary_ == data_->first ? "opening state" : "after day " + std::to_string(boundary_ - 1)),
            Ink::Bold);
        f.put(6, 1, "[ / ] day   g jump   0 opening   G final   i find ID   m manifest   v verify");
    } else {
        f.put(5, 1, "Recorded runs  |  Enter open  / filter  R refresh  n new run", Ink::Bold);
        f.put(6, 1, "Completed simulations only. n starts one here; ./scripts/dev.sh start runs the demo");
    }
    const int first_row = 8, bottom = h - 4, body = std::max(1, bottom - first_row);
    page_size_ = body;
    if (form_ && !help_) {
        auto lines = wrap_lines(form_lines(), w - 3);
        auto focus = std::find_if(lines.begin(), lines.end(),
                                  [](const auto &line) { return line.rfind("> ", 0) == 0; });
        auto selected_line = static_cast<std::size_t>(focus - lines.begin());
        auto first = selected_line < static_cast<std::size_t>(body)
                         ? 0
                         : selected_line - static_cast<std::size_t>(body / 2);
        for (int r = 0; r < body && first + static_cast<std::size_t>(r) < lines.size(); ++r) {
            auto &line = lines[first + static_cast<std::size_t>(r)];
            bool current = line.rfind("> ", 0) == 0;
            f.put(first_row + r, 1, line, r == 0 ? Ink::Bold : current ? Ink::Selected : Ink::Normal);
        }
        f.put(h - 3, 1,
              compact ? "Arrows field/toggle | Enter edit/start | Esc cancel"
                      : "Up/Down field | Enter edit or start | Left/Right/Space toggle | Esc cancel");
    } else if (help_) {
        std::vector<std::string> lines = {
            "Keyboard controls",
            "",
            "1..7 / Tab / Shift-Tab    Switch views",
            "Up/Down or j/k           Move selection / scroll record",
            "Page Up/Down, Home/End   Navigate longer lists and records",
            "Enter                    Open a run or the complete selected record",
            "[ / ] or Left/Right      Previous / next recorded day boundary",
            "g                        Jump to a boundary in this run's interval",
            "0 / G                    Opening / final state",
            "/                        Filter the current list (case-insensitive)",
            "i                        Find exact household, firm, decision or event ID",
            "d / e on an entity       Show its decisions / events for this day",
            "6 Labor                  Per-person day: job held, worked, wage paid, changes, search",
            "7 Ledger                 Levels: every entity's day -> Enter: its transactions -> Enter: record",
            "Esc in the Ledger        Back up one level",
            "n in the run list        New run form: scenario, seed, days, fixed/dynamic employment",
            "v                        Independently verify persisted records now",
            "R / o                    Reload / choose another run",
            "Esc                      Back, cancel prompt, or clear filters",
            "q / Ctrl-C / Ctrl-D      Quit and restore the terminal"};
        lines = wrap_lines(lines, w - 3);
        auto max_scroll =
            lines.size() > static_cast<std::size_t>(body) ? lines.size() - static_cast<std::size_t>(body) : 0;
        detail_scroll_ = std::min(detail_scroll_, max_scroll);
        for (int i = 0; i < body && detail_scroll_ + static_cast<std::size_t>(i) < lines.size(); ++i)
            f.put(first_row + i, 1, lines[detail_scroll_ + static_cast<std::size_t>(i)],
                  i == 0 ? Ink::Bold : Ink::Normal);
        f.put(h - 3, 1, "Help: arrows / Page Up / Down scroll; Esc closes");
    } else if (detail_) {
        f.put(7, 1, detail_title_, Ink::Bold);
        auto wrapped = wrap_lines(detail_lines_, w - 3);
        auto max_scroll = wrapped.size() > static_cast<std::size_t>(body)
                              ? wrapped.size() - static_cast<std::size_t>(body)
                              : 0;
        detail_scroll_ = std::min(detail_scroll_, max_scroll);
        for (int r = 0; r < body && detail_scroll_ + static_cast<std::size_t>(r) < wrapped.size(); ++r)
            f.put(first_row + r, 1, wrapped[detail_scroll_ + static_cast<std::size_t>(r)]);
        f.put(h - 3, 1,
              "Record lines " + std::to_string(wrapped.empty() ? 0 : detail_scroll_ + 1) + "-" +
                  std::to_string(std::min(wrapped.size(), detail_scroll_ + static_cast<std::size_t>(body))) +
                  " / " + std::to_string(wrapped.size()) + "  |  arrows scroll, Esc back");
    } else if (view_ == View::Overview && data_) {
        auto &accounts = snapshot_.at("accounts");
        Amount total = 0, households = 0, firms = 0;
        Json inventories = Json::object();
        std::size_t hc = 0, fc = 0;
        for (auto &a : accounts) {
            auto cash = integer(a.at("cash"));
            total = add(total, cash);
            if (text(a, "id").rfind("household:", 0) == 0) {
                households = add(households, cash);
                ++hc;
            } else {
                firms = add(firms, cash);
                ++fc;
            }
            for (auto it = a.at("stock").begin(); it != a.at("stock").end(); ++it)
                inventories[it.key()] = add(inventories.value(it.key(), Amount{0}), integer(it.value()));
        }
        std::vector<std::string> lines = {text(data_->scenario, "name"),
                                          std::to_string(hc) + " households / " + std::to_string(fc) +
                                              " firms | seed " + text(data_->manifest, "seed") + " | " +
                                              text(data_->manifest, "trace") + " | " +
                                              text(data_->manifest, "model"),
                                          "Cash c: total " + number(total) + " | households " +
                                              number(households) + " | firms " + number(firms),
                                          "Stock units (reserved included): " + counts(inventories)};
        lines.push_back("Employment mode: " + text(data_->manifest, "employment") +
                        (metric_
                             ? " | employed " + text(*metric_, "employed") + " | unemployed " +
                                   text(*metric_, "unemployed") + " | hires " + text(*metric_, "hires") +
                                   " | separations " + text(*metric_, "separations") + " | pending " +
                                   text(*metric_, "pending_changes") + " | board " +
                                   std::to_string(metric_->value("board", Json::array()).size()) + " postings"
                             : ""));
        if (metric_ && metric_->contains("job_search")) {
            auto &js = metric_->at("job_search");
            auto applied = integer(js.value("applied", Json(0))),
                 declined = integer(js.value("declined", Json(0)));
            auto offered = applied + declined;
            lines.push_back("Job search: " + text(js, "searching") + " searched | " +
                            std::to_string(applied) + " applied (" + text(js, "hires") + " hires, " +
                            text(js, "switches") + " switches, " + text(js, "quits") + " quits) | " +
                            std::to_string(declined) + " declined | " + text(js, "ineligible") +
                            " ineligible | " + text(js, "no_postings") + " saw no postings | acceptance " +
                            (offered ? std::to_string(applied * 100 / offered) + "%" : "n/a"));
            if (!js.value("declined_reasons", Json::object()).empty())
                lines.push_back("Declined offers by reason: " + counts(js.at("declined_reasons")));
            if (!js.value("ineligible_reasons", Json::object()).empty())
                lines.push_back("Ineligible offers by reason: " + counts(js.at("ineligible_reasons")));
        }
        if (metric_) {
            auto &m = *metric_;
            lines.push_back("Day " + std::to_string(boundary_ - 1) + " | Paid workers: " +
                            text(m, "paid_workers") + " | Wages: " + number(integer(m.at("wages"))) + " c");
            // The day's circuit, level by level: payroll buys labor, labor makes goods, goods move
            // through wholesale and retail into consumption.
            if (m.contains("labor_units")) {
                auto &lu = m.at("labor_units");
                lines.push_back("Labor: " + text(lu, "delivered") + " worker-days bought with payroll | " +
                                text(lu, "used") + " used in production | " + text(lu, "idle") + " idle | " +
                                text(lu, "retail_service") + " retail service");
            }
            lines.push_back("Goods: made " + counts(m.at("production")) + " | wholesale " +
                            counts(m.at("sales_units").at("wholesale")) + " | retail " +
                            counts(m.at("sales_units").at("retail")) + " | consumed " +
                            counts(m.at("consumption")) + " | unmet " + counts(m.at("unmet")));
            lines.push_back("Cash: wages " + number(integer(m.at("wages"))) + " c -> households | retail " +
                            number(integer(m.at("sales_cents").at("retail"))) + " c -> shops | wholesale " +
                            number(integer(m.at("sales_cents").at("wholesale"))) + " c -> producers");
            lines.push_back("Failed checkouts: " + text(m, "failed_attempts") + "  Stockouts: " +
                            text(m, "stockouts") + "  (7 Ledger drills into any entity's day)");
        } else
            lines.push_back("No days completed within this run yet; activity is not available.");
        auto &saved = data_->saved_verification;
        if (!data_->saved_verification_error.empty()) {
            lines.push_back("Saved verification: INVALID; v reruns checks without writing.");
            lines.push_back("Report error: " + data_->saved_verification_error);
        } else
            lines.push_back(saved.is_null() ? "Saved verification: absent; v verifies now."
                                            : std::string("Saved verification: ") +
                                                  (saved.at("passed").get<bool>() ? "PASSED" : "FAILED") +
                                                  " (saved; v reruns checks)");
        lines.push_back("State source: " + data_->snapshot_source(boundary_));
        if (!data_->manifest.at("parent").is_null())
            lines.push_back("Continuation: earlier days remain in parent run.");
        lines = wrap_lines(lines, w - 3);
        auto max_scroll =
            lines.size() > static_cast<std::size_t>(body) ? lines.size() - static_cast<std::size_t>(body) : 0;
        overview_scroll_ = std::min(overview_scroll_, max_scroll);
        for (int r = 0; r < body && overview_scroll_ + static_cast<std::size_t>(r) < lines.size(); ++r)
            f.put(first_row + r, 1, lines[overview_scroll_ + static_cast<std::size_t>(r)],
                  r == 0 ? Ink::Bold : Ink::Normal);
        f.put(h - 3, 1,
              "Overview: arrows scroll; 3 entities; 4 decisions; 5 events; 6 labor; 7 ledger (drill down)");
    } else {
        f.put(7, 1,
              (view_ == View::Ledger
                   ? (ledger_entity_.empty() ? "Ledger: town > entities  "
                                             : "Ledger: town > " + ledger_entity_ + " > transactions  ")
                   : "") +
                  (filter_.empty() ? "" : ("Filter: " + filter_ + "  ")) +
                  (entity_.empty() ? "" : ("Actor: " + entity_ + "  ")) + std::to_string(rows_.size()) +
                  " records",
              Ink::Bold);
        if (rows_.empty()) {
            f.put(first_row + 1, 2,
                  view_ == View::Runs ? "No runs found here. Press n to create a run."
                                      : "No matching records for this boundary.");
            f.put(first_row + 3, 2, "Esc clears filters. [ / ] changes the recorded day.");
        } else {
            bool split = w >= 100;
            int list_width = split ? w / 2 : w;
            int list_rows = split ? body : std::max(3, body - 6);
            if (selected_ < top_)
                top_ = selected_;
            if (selected_ >= top_ + static_cast<std::size_t>(list_rows))
                top_ = selected_ - static_cast<std::size_t>(list_rows) + 1;
            page_size_ = list_rows;
            for (int r = 0; r < list_rows && top_ + static_cast<std::size_t>(r) < rows_.size(); ++r) {
                auto n = top_ + static_cast<std::size_t>(r);
                f.put(first_row + r, 1, (n == selected_ ? "> " : "  ") + rows_[n].label,
                      n == selected_ ? Ink::Selected : Ink::Normal, list_width - 2);
            }
            auto detail = preview(rows_[selected_]);
            int detail_x = split ? list_width + 2 : 2;
            int detail_y = split ? first_row : first_row + list_rows + 1;
            int available = split ? body : std::max(0, bottom - detail_y);
            if (split)
                for (int r = first_row; r < bottom; ++r)
                    f.put(r, list_width, "|");
            for (int r = 0; r < available && r < static_cast<int>(detail.size()); ++r)
                f.put(detail_y + r, detail_x, detail[static_cast<std::size_t>(r)],
                      r == 0 ? Ink::Bold : Ink::Normal);
            f.put(h - 3, 1,
                  std::to_string(selected_ + 1) + " / " + std::to_string(rows_.size()) +
                      (view_ == View::Entities || view_ == View::Labor ||
                               (view_ == View::Ledger && ledger_entity_.empty())
                           ? " | Enter record | d decisions / e events | / filter"
                       : view_ == View::Ledger ? " | Enter full record | Esc up one level | / filter"
                                               : " | Enter record / evidence | / filter | Esc clear"));
        }
    }
    f.rule(h - 4);
    if (!prompt_.empty()) {
        auto prefix = prompt_ + ": ";
        auto value = input_;
        auto available = static_cast<std::size_t>(std::max(4, w - static_cast<int>(prefix.size()) - 22));
        if (value.size() > available)
            value = "..." + value.substr(value.size() - available + 3);
        f.put(h - 2, 1, prefix + value + "_ (Enter / Esc)", Ink::Selected);
    } else
        f.put(h - 2, 1, "q quit  ? help  Tab view  arrows select  Enter inspect  o runs  n new run");
    f.put(h - 1, 1, "Recorded observations only. Existing runs are never modified; n writes a new run.");
    return f;
}
} // namespace econ::tui
