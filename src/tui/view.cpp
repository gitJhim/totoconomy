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
    for (auto key : {"actor", "buyer", "seller", "employer", "household", "firm"})
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
static std::string event_label(const Json &e) {
    auto &d = e.at("data");
    std::string s = text(e, "id") + "  " + text(e, "type") + "  ";
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
    rebuild_rows();
}
void Inspector::set_boundary(Day day) {
    if (!data_)
        return;
    day = std::clamp(day, data_->first, data_->last);
    auto previous_entity =
        (view_ == View::Entities && selected_ < rows_.size()) ? rows_[selected_].id : std::string{};
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
                         std::to_string(run.until) + "  seed " + run.seed + "  " + run.trace;
            if (match(label))
                rows_.push_back({run.path.string(),
                                 label,
                                 {{"path", run.path.string()},
                                  {"status", run.status},
                                  {"from_day", run.from},
                                  {"until_day", run.until},
                                  {"trace", run.trace},
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
            lines.push_back("Employer: " + text(h, "employer"));
            lines.push_back("Needs: " + counts(h.at("needs")));
            lines.push_back(h.contains("knowledge")
                                ? "Knowledge: " + std::to_string(h.at("knowledge").size()) + " observations"
                                : "Knowledge: see daily decisions");
        } else if (row.record.contains("firm")) {
            auto &f = row.record.at("firm");
            lines.push_back("Role: " + text(f, "kind"));
            lines.push_back("Prices c/unit: " + counts(f.at("prices")));
            lines.push_back("Wage: " + text(f, "wage") + " c / worker");
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
    } else {
        auto &e = row.record;
        lines = {row.id, "Phase: " + text(e, "phase"), "Type: " + text(e, "type"),
                 "Effects: " + std::to_string(e.at("effects").size())};
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
    else
        show_json(view_ == View::Entities ? "Entity at boundary " + std::to_string(boundary_)
                                          : "Recorded event",
                  row.record);
}
void Inspector::submit_prompt() {
    auto kind = prompt_;
    prompt_.clear();
    auto input = input_;
    input_.clear();
    if (kind == "Filter") {
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
            else if (detail_)
                detail_ = false;
            else if (!filter_.empty() || !entity_.empty()) {
                filter_.clear();
                entity_.clear();
                selected_ = top_ = 0;
                rebuild_rows();
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
        if (key.size() == 1 && key[0] >= '1' && key[0] <= '5') {
            select_view(static_cast<View>(key[0] - '1'));
            return true;
        }
        if (key == "TAB" || key == "BACKTAB") {
            auto value = (static_cast<int>(view_) + (key == "TAB" ? 1 : 4)) % 5;
            select_view(static_cast<View>(value));
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
            } else if ((key == "d" || key == "e") && view_ == View::Entities && !rows_.empty()) {
                auto actor = rows_[selected_].id;
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
    f.put(0, w - 12, "READ ONLY", Ink::Bold);
    f.put(1, 1, data_ ? data_->directory.string() : "Run browser: " + fs::absolute(root_).string());
    f.put(2, 1, message_);
    const std::vector<std::string> tabs =
        w < 75 ? std::vector<std::string>{"1Runs", "2Town", "3Entities", "4Decisions", "5Events"}
               : std::vector<std::string>{"1 Runs", "2 Overview", "3 Entities", "4 Decisions", "5 Events"};
    int x = 1;
    for (std::size_t i = 0; i < tabs.size(); ++i) {
        f.put(3, x, " " + tabs[i] + " ",
              static_cast<int>(view_) == static_cast<int>(i) ? Ink::Selected : Ink::Normal);
        x += static_cast<int>(tabs[i].size()) + (w < 75 ? 2 : 3);
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
        f.put(5, 1, "Recorded runs  |  Enter open  / filter  R refresh", Ink::Bold);
        f.put(6, 1, "Completed simulations only. Create one with ./scripts/dev.sh start");
    }
    const int first_row = 8, bottom = h - 4, body = std::max(1, bottom - first_row);
    page_size_ = body;
    if (help_) {
        std::vector<std::string> lines = {
            "Keyboard controls",
            "",
            "1..5 / Tab / Shift-Tab    Switch views",
            "Up/Down or j/k           Move selection / scroll record",
            "Page Up/Down, Home/End   Navigate longer lists and records",
            "Enter                    Open a run or the complete selected record",
            "[ / ] or Left/Right      Previous / next recorded day boundary",
            "g                        Jump to a boundary in this run's interval",
            "0 / G                    Opening / final state",
            "/                        Filter the current list (case-insensitive)",
            "i                        Find exact household, firm, decision or event ID",
            "d / e on an entity       Show its decisions / events for this day",
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
        if (metric_) {
            auto &m = *metric_;
            lines.push_back("Day " + std::to_string(boundary_ - 1) + " | Paid workers: " +
                            text(m, "paid_workers") + " | Wages: " + number(integer(m.at("wages"))) + " c");
            lines.push_back("Produced: " + counts(m.at("production")));
            lines.push_back("Consumed: " + counts(m.at("consumption")));
            lines.push_back("Unmet:    " + counts(m.at("unmet")));
            lines.push_back("Sales c: " + counts(m.at("sales_cents")));
            lines.push_back("Failed checkouts: " + text(m, "failed_attempts") +
                            "  Stockouts: " + text(m, "stockouts"));
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
        f.put(h - 3, 1, "Overview: arrows / Page Up / Down scroll; 3 entities; 4 decisions; 5 events");
    } else {
        f.put(7, 1,
              (filter_.empty() ? "" : ("Filter: " + filter_ + "  ")) +
                  (entity_.empty() ? "" : ("Actor: " + entity_ + "  ")) + std::to_string(rows_.size()) +
                  " records",
              Ink::Bold);
        if (rows_.empty()) {
            f.put(first_row + 1, 2,
                  view_ == View::Runs ? "No runs found here. Create a run, then press R."
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
                      (view_ == View::Entities ? " | Enter record | d decisions / e events | / filter"
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
        f.put(h - 2, 1, "q quit  ? help  Tab view  arrows select  Enter inspect  o runs");
    f.put(h - 1, 1, "Recorded observations only. No policy execution or run files are changed.");
    return f;
}
} // namespace econ::tui
