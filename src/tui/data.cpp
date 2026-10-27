#include <algorithm>
#include <econ/tui.hpp>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
namespace econ::tui {
namespace fs = std::filesystem;
std::string clean(const std::string &text) {
    std::string result;
    for (unsigned char c : text) {
        if (c >= 32 && c < 127)
            result += static_cast<char>(c);
        else if (c < 128 || (c & 0xc0) != 0x80)
            result += '?';
    }
    return result;
}
std::string number(Amount value) {
    auto s = std::to_string(value);
    auto start = s.front() == '-' ? 1U : 0U;
    for (auto n = s.size(); n > start + 3; n -= 3)
        s.insert(n - 3, ",");
    return s;
}
static RunSummary summary(const fs::path &path) {
    RunSummary row;
    row.path = path;
    row.name = path.filename().string();
    try {
        auto m = read_json(path / "manifest.json");
        if (m.at("schema") != "manifest-1" || m.at("model") != model_version)
            throw Error(4, "unsupported run schema/model");
        row.status = m.at("status");
        row.trace = m.at("trace");
        row.employment = m.value("employment", std::string("fixed"));
        row.seed = m.at("seed").dump();
        row.from = integer(m.at("from_day"));
        row.until = integer(m.at("until_day"));
    } catch (const std::exception &e) {
        row.status = "unreadable";
        row.error = e.what();
    }
    return row;
}
std::vector<RunSummary> discover(const fs::path &root) {
    std::vector<RunSummary> rows;
    if (!fs::exists(root))
        return rows;
    if (!fs::is_directory(root))
        throw Error(3, "Run browser path is not a directory: " + root.string());
    if (fs::is_regular_file(root / "manifest.json"))
        rows.push_back(summary(root));
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied), end;
    for (; it != end; ++it) {
        if (it.depth() >= 5 && it->is_directory())
            it.disable_recursion_pending();
        if (it->path().filename() == "manifest.json" && it->is_regular_file())
            rows.push_back(summary(it->path().parent_path()));
    }
    // Capture metadata once: a running job may update its manifest during discovery.
    // Sorting must not re-read changing filesystem values inside its comparator.
    std::map<fs::path, fs::file_time_type> modified;
    for (auto &row : rows) {
        std::error_code ec;
        auto time = fs::last_write_time(row.path / "manifest.json", ec);
        modified[row.path] = ec ? fs::file_time_type::min() : time;
    }
    std::sort(rows.begin(), rows.end(), [&](const auto &a, const auto &b) {
        auto at = modified.at(a.path), bt = modified.at(b.path);
        return at == bt ? a.path.string() < b.path.string() : at > bt;
    });
    return rows;
}
LogIndex::LogIndex(const fs::path &path, const std::string &schema, const std::string &time_key, Day first,
                   Day last, bool single)
    : path_(path) {
    std::ifstream in(path);
    if (!in)
        throw Error(3, "Missing inspection artifact: " + path.string());
    std::string line;
    Day previous = first;
    std::size_t line_number = 0;
    while (true) {
        auto offset = in.tellg();
        if (!std::getline(in, line))
            break;
        ++line_number;
        try {
            auto j = Json::parse(line);
            if (j.at("schema") != schema)
                throw Error(4, "unsupported " + schema + " record");
            auto day = integer(j.at(time_key));
            if (day < first || day > last || (line_number > 1 && day < previous))
                throw Error(4, "out-of-order/out-of-interval records");
            previous = day;
            auto [it, inserted] = ranges_.try_emplace(day, Range{offset, 0});
            if (single && !inserted)
                throw Error(4, "duplicate day observation");
            ++it->second.count;
        } catch (const std::exception &e) {
            throw Error(4, path.string() + ":" + std::to_string(line_number) + ": " + e.what());
        }
    }
    if (in.bad())
        throw Error(3, "Read failed: " + path.string());
    if (single) {
        Day expected = first;
        for (auto &[day, range] : ranges_) {
            (void)range;
            if (day != expected)
                throw Error(4, "Missing day observation in " + path.string());
            ++expected;
        }
        if (expected != last + 1)
            throw Error(4, "Incomplete history in " + path.string());
    }
}
std::vector<Json> LogIndex::read(Day day) const {
    std::vector<Json> rows;
    auto found = ranges_.find(day);
    if (found == ranges_.end())
        return rows;
    std::ifstream in(path_);
    if (!in)
        throw Error(3, "Cannot read: " + path_.string());
    in.seekg(found->second.offset);
    std::string line;
    for (std::size_t n = 0; n < found->second.count; ++n) {
        if (!std::getline(in, line))
            throw Error(4, "History changed/truncated; reload the run");
        rows.push_back(Json::parse(line));
    }
    return rows;
}
std::optional<Json> LogIndex::find(const std::string &id) const {
    std::optional<Json> result;
    // One record at a time; no full-history allocation.
    std::ifstream in(path_);
    if (!in)
        throw Error(3, "Cannot read: " + path_.string());
    std::string line;
    while (std::getline(in, line)) {
        auto j = Json::parse(line);
        if (j.at("id") == id) {
            result = std::move(j);
            break;
        }
    }
    if (in.bad())
        throw Error(3, "Read failed: " + path_.string());
    return result;
}
static void account_bounds(const Json &snapshot) {
    std::set<std::string> ids;
    if (!snapshot.at("accounts").is_array())
        throw Error(4, "Recorded accounts must be an array");
    for (auto &a : snapshot.at("accounts")) {
        auto cash = integer(a.at("cash")), reserved = integer(a.at("reserved_cash"));
        if (!ids.insert(a.at("id").get<std::string>()).second || cash < 0 || reserved < 0 || reserved > cash)
            throw Error(4, "Invalid recorded cash/reservation bounds");
        if (!a.at("stock").is_object() || !a.at("reserved_stock").is_object() ||
            a.at("stock").size() != a.at("reserved_stock").size())
            throw Error(4, "Invalid recorded inventory maps");
        for (auto it = a.at("stock").begin(); it != a.at("stock").end(); ++it) {
            auto q = integer(it.value()), r = integer(a.at("reserved_stock").at(it.key()));
            if (q < 0 || r < 0 || r > q)
                throw Error(4, "Invalid recorded stock/reservation bounds");
        }
    }
}
RunData::RunData(const fs::path &path) : directory(fs::absolute(path)) {
    manifest = read_json(directory / "manifest.json");
    if (manifest.at("schema") != "manifest-1" || manifest.at("model") != model_version)
        throw Error(4, "Unsupported run schema/model");
    if (manifest.at("status") != "complete")
        throw Error(4, "Run is not complete. Finish the simulation before opening it.");
    scenario = read_json(directory / "scenario.normalized.json");
    if (scenario.at("schema") != "scenario-1")
        throw Error(4, "Unsupported scenario schema");
    initial = read_json(directory / "initial_state.json");
    final = read_json(directory / "final_state.json");
    for (auto *state : {&initial, &final})
        if (state->at("schema") != "state-1" || state->at("model") != model_version ||
            state->at("digest") != digest(state->at("state")))
            throw Error(4, "Invalid state schema/digest");
    account_bounds(initial.at("state"));
    account_bounds(final.at("state"));
    first = integer(manifest.at("from_day"));
    last = integer(manifest.at("until_day"));
    if (first < 0 || last < first || last > 10000000 ||
        integer(initial.at("state").at("next_day")) != first ||
        integer(final.at("state").at("next_day")) != last)
        throw Error(4, "Run/state interval mismatch");
    replay_ = LogIndex(directory / "replay.jsonl", "replay-1", "next_day", first + 1, last, true);
    metrics_ = LogIndex(directory / "metrics.jsonl", "metric-1", "day", first, last - 1, true);
    decisions_ = LogIndex(directory / "decisions.jsonl", "decision-1", "day", first, last - 1, false);
    events_ = LogIndex(directory / "events.jsonl", "event-1", "day", first, last - 1, false);
    if (fs::exists(directory / "verification.json")) {
        try {
            auto report = read_json(directory / "verification.json");
            if (!report.is_object() || report.value("schema", std::string{}) != "verification-1" ||
                !report.contains("passed") || !report.at("passed").is_boolean() ||
                !report.contains("checks") || !report.at("checks").is_array())
                throw Error(4, "Expected verification-1 with boolean passed and a checks array");
            saved_verification = std::move(report);
        } catch (const std::exception &e) {
            saved_verification_error = e.what();
        }
    }
}
static fs::path checkpoint_path(const fs::path &directory, Day boundary) {
    std::ostringstream name;
    name << "day-" << std::setfill('0') << std::setw(4) << boundary << ".json";
    return directory / "checkpoints" / name.str();
}
Json RunData::snapshot(Day boundary) const {
    if (boundary < first || boundary > last)
        throw Error(2, "Boundary is outside this run's interval");
    if (boundary == first)
        return initial.at("state");
    if (boundary == last)
        return final.at("state");
    auto cp = checkpoint_path(directory, boundary);
    if (fs::exists(cp)) {
        auto loaded = load_checkpoint(cp);
        if (loaded.origin != manifest.at("origin").get<std::string>() || loaded.state.next_day != boundary)
            throw Error(4, "Checkpoint does not match this run");
        return Json(loaded.state);
    }
    auto rows = replay_.read(boundary);
    if (rows.size() != 1)
        throw Error(4, "Missing display snapshot");
    account_bounds(rows.front());
    return rows.front();
}
std::string RunData::snapshot_source(Day boundary) const {
    if (boundary == first)
        return "initial_state.json";
    if (boundary == last)
        return "final_state.json";
    if (fs::exists(checkpoint_path(directory, boundary)))
        return "checkpoint (complete state)";
    return "replay.jsonl (display state; household knowledge not recorded here)";
}
std::optional<Json> RunData::metric(Day boundary) const {
    if (boundary == first)
        return {};
    auto rows = metrics_.read(boundary - 1);
    if (rows.size() != 1)
        throw Error(4, "Missing metric observation");
    return rows.front();
}
std::vector<Json> RunData::decisions(Day boundary) const {
    return boundary == first ? std::vector<Json>{} : decisions_.read(boundary - 1);
}
std::vector<Json> RunData::events(Day boundary) const {
    return boundary == first ? std::vector<Json>{} : events_.read(boundary - 1);
}
std::optional<Json> RunData::find_record(const std::string &id) const {
    if (id.rfind("decision:", 0) == 0)
        return decisions_.find(id);
    if (id.rfind("event:", 0) == 0)
        return events_.find(id);
    return {};
}
} // namespace econ::tui
