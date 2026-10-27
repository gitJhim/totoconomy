#pragma once
#include <econ/econ.hpp>
#include <memory>

namespace econ::tui {
struct RunSummary {
    std::filesystem::path path;
    std::string name, status, trace, seed, employment, error;
    Day from = 0, until = 0;
};
std::vector<RunSummary> discover(const std::filesystem::path &root);
class LogIndex {
  public:
    struct Range {
        std::streamoff offset = 0;
        std::size_t count = 0;
    };
    LogIndex() = default;
    LogIndex(const std::filesystem::path &path, const std::string &schema, const std::string &time_key,
             Day first, Day last, bool one_per_day);
    std::vector<Json> read(Day day) const;
    std::optional<Json> find(const std::string &id) const;

  private:
    std::filesystem::path path_;
    std::map<Day, Range> ranges_;
};
class RunData {
  public:
    explicit RunData(const std::filesystem::path &directory);
    std::filesystem::path directory;
    Json manifest, scenario, initial, final, saved_verification;
    std::string saved_verification_error;
    Day first = 0, last = 0;
    Json snapshot(Day boundary) const;
    std::string snapshot_source(Day boundary) const;
    std::optional<Json> metric(Day boundary) const;
    std::vector<Json> decisions(Day boundary) const;
    std::vector<Json> events(Day boundary) const;
    std::optional<Json> find_record(const std::string &id) const;

  private:
    LogIndex replay_, metrics_, decisions_, events_;
};
enum class View { Runs, Overview, Entities, Decisions, Events, Labor, Ledger };
inline constexpr int view_count = 7;
enum class Ink { Normal, Bold, Accent, Selected };
struct Cell {
    char text = ' ';
    Ink ink = Ink::Normal;
};
class Frame {
  public:
    Frame(int columns, int rows);
    int columns, rows;
    void put(int row, int column, const std::string &text, Ink ink = Ink::Normal, int limit = -1);
    void rule(int row);
    std::string plain() const;
    std::string ansi(bool color) const;

  private:
    std::vector<Cell> cells_;
};
struct Row {
    std::string id, label;
    Json record;
};
// Parameters for a simulation started from the inspector. Everything is plain text so the
// form can be edited field by field; launch() validates through the same core run() as the CLI.
struct LaunchForm {
    std::string scenario, seed = "42", days = "60", employment = "fixed", trace = "audit",
                          checkpoint_every = "15", name;
    bool name_edited = false;
};
class Inspector {
  public:
    explicit Inspector(std::filesystem::path root, std::optional<std::filesystem::path> run = {});
    Frame render(int columns, int rows);
    bool key(const std::string &key);
    // Text for an interim frame when this key will start slow work, or empty for none.
    std::string wait_message(const std::string &key) const;
    View view() const {
        return view_;
    }
    bool form_open() const {
        return form_;
    }
    const LaunchForm &form() const {
        return form_values_;
    }
    Day boundary() const {
        return boundary_;
    }
    const RunData *data() const {
        return data_.get();
    }

  private:
    std::filesystem::path root_;
    std::vector<RunSummary> runs_;
    std::unique_ptr<RunData> data_;
    View view_ = View::Runs;
    Day boundary_ = 0;
    Json snapshot_;
    std::optional<Json> metric_;
    std::vector<Json> decisions_, events_;
    std::vector<Row> rows_;
    std::size_t selected_ = 0, top_ = 0;
    int page_size_ = 10;
    std::string filter_, entity_, message_;
    bool help_ = false, detail_ = false;
    std::vector<std::string> detail_lines_;
    std::string detail_title_;
    std::size_t detail_scroll_ = 0, overview_scroll_ = 0;
    std::string prompt_, input_;
    std::string ledger_entity_; // Ledger drill-down: empty = all entities, else that entity's transactions
    bool form_ = false;
    std::size_t form_field_ = 0;
    LaunchForm form_values_;
    std::vector<std::filesystem::path> scenario_choices_;
    void open(const std::filesystem::path &directory);
    void open_form();
    void form_key(const std::string &key);
    void launch_form();
    std::vector<std::string> form_lines() const;
    void refresh_runs();
    void select_view(View view);
    void set_boundary(Day day);
    void rebuild_rows();
    void show_json(const std::string &title, const Json &record);
    void submit_prompt();
    void activate();
    Json entity_record(const std::string &id) const;
    Json day_flows(const std::string &id) const;
    Money opening_cash(const std::string &id) const;
    std::vector<std::string> preview(const Row &row) const;
};
struct Options {
    std::optional<std::filesystem::path> run;
    std::filesystem::path runs_directory = "runs";
    bool color = true;
};
int launch(const Options &options);
std::string clean(const std::string &text);
std::string number(Amount value);
} // namespace econ::tui
