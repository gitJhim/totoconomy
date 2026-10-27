#include <algorithm>
#include <chrono>
#include <econ/tui.hpp>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <limits>
#include <poll.h>
#include <pty.h>
#include <regex>
#include <sstream>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
using namespace econ;
using namespace econ::tui;
namespace fs = std::filesystem;
#define REQUIRE(condition)                                                                                   \
    do {                                                                                                     \
        if (!(condition))                                                                                    \
            throw std::runtime_error(std::string(__FILE__) + ":" + std::to_string(__LINE__) +                \
                                     " " #condition);                                                        \
    } while (false)
static void fails(const std::function<void()> &fn) {
    bool failed = false;
    try {
        fn();
    } catch (const std::exception &) {
        failed = true;
    }
    REQUIRE(failed);
}
static Json tree(const fs::path &root) {
    Json j = Json::object();
    for (auto &f : fs::recursive_directory_iterator(root))
        if (f.is_regular_file()) {
            std::ifstream in(f.path(), std::ios::binary);
            std::string bytes{std::istreambuf_iterator<char>(in), {}};
            j[fs::relative(f.path(), root).string()] = digest(Json(bytes));
        }
    return j;
}
static void type(Inspector &app, const std::string &value) {
    for (char c : value)
        app.key(std::string(1, c));
    app.key("ENTER");
}
static void retype(Inspector &app, const std::string &value) {
    for (int n = 0; n < 200; ++n)
        app.key("BACKSPACE");
    type(app, value);
}
static void has(const Frame &frame, const std::string &needle) {
    REQUIRE(frame.plain().find(needle) != std::string::npos);
}
static void frame_bounds(const Frame &frame, int w, int h) {
    std::istringstream in(frame.plain());
    std::string line;
    int count = 0;
    while (std::getline(in, line)) {
        REQUIRE(line.size() <= static_cast<std::size_t>(w));
        for (unsigned char c : line)
            REQUIRE(c >= 32 && c < 127);
        ++count;
    }
    REQUIRE(count == h);
}
struct Pty {
    int fd = -1;
    pid_t child = -1;
    termios original{};
    std::string transcript;
    Pty(const std::string &exe, const fs::path &directory, int width, int height) {
        int gate[2];
        REQUIRE(pipe(gate) == 0);
        winsize size{};
        size.ws_col = static_cast<unsigned short>(width);
        size.ws_row = static_cast<unsigned short>(height);
        child = forkpty(&fd, nullptr, nullptr, &size);
        REQUIRE(child >= 0);
        if (child == 0) {
            close(gate[1]);
            char byte = 0;
            if (read(gate[0], &byte, 1) != 1)
                _exit(125);
            close(gate[0]);
            setenv("TERM", "xterm-256color", 1);
            execl(exe.c_str(), exe.c_str(), "tui", "--run", directory.c_str(), "--no-color",
                  static_cast<char *>(nullptr));
            _exit(126);
        }
        close(gate[0]);
        REQUIRE(tcgetattr(fd, &original) == 0);
        REQUIRE(write(gate[1], "x", 1) == 1);
        close(gate[1]);
        REQUIRE(fcntl(fd, F_SETFL, O_NONBLOCK) == 0);
    }
    ~Pty() {
        if (child > 0) {
            kill(child, SIGTERM);
            int status = 0;
            waitpid(child, &status, 0);
        }
        if (fd >= 0)
            close(fd);
    }
    std::string screen() const {
        auto pos = transcript.rfind("\x1b[H");
        auto s = pos == std::string::npos ? transcript : transcript.substr(pos + 3);
        s = std::regex_replace(s, std::regex("\x1b\\[[0-9;?]*[A-Za-z]"), "");
        s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());
        return s;
    }
    void read_available() {
        char bytes[8192];
        while (true) {
            auto n = read(fd, bytes, sizeof(bytes));
            if (n <= 0)
                break;
            transcript.append(bytes, static_cast<std::size_t>(n));
        }
    }
    void until(const std::string &wanted) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (std::chrono::steady_clock::now() < deadline) {
            pollfd p{fd, POLLIN, 0};
            poll(&p, 1, 25);
            read_available();
            auto s = screen();
            if (s.find(wanted) != std::string::npos &&
                (s.find("Recorded observations only.") != std::string::npos ||
                 s.find("Terminal too small.") != std::string::npos))
                return;
        }
        throw std::runtime_error("PTY screen timed out waiting for " + wanted + "\n" + screen());
    }
    void send(const std::string &keys, const std::string &wanted) {
        read_available();
        transcript.clear();
        REQUIRE(write(fd, keys.data(), keys.size()) == static_cast<ssize_t>(keys.size()));
        until(wanted);
    }
    void resize(int w, int h) {
        winsize size{};
        size.ws_col = static_cast<unsigned short>(w);
        size.ws_row = static_cast<unsigned short>(h);
        read_available();
        transcript.clear();
        REQUIRE(ioctl(fd, TIOCSWINSZ, &size) == 0);
    }
    void finish(bool signal = false, int expected = 0, bool quit = true) {
        if (signal)
            kill(child, SIGTERM);
        else if (quit)
            REQUIRE(write(fd, "q", 1) == 1);
        int status = 0;
        bool exited = false;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline) {
            read_available();
            auto n = waitpid(child, &status, WNOHANG);
            if (n == child) {
                exited = true;
                break;
            }
            pollfd p{fd, POLLIN, 0};
            poll(&p, 1, 25);
        }
        REQUIRE(exited);
        child = -1;
        REQUIRE(WIFEXITED(status) && WEXITSTATUS(status) == expected);
        read_available();
        REQUIRE(transcript.find("\x1b[?1049l") != std::string::npos);
        termios restored{};
        REQUIRE(tcgetattr(fd, &restored) == 0);
        REQUIRE(restored.c_lflag == original.c_lflag);
        REQUIRE(restored.c_iflag == original.c_iflag);
    }
};
static void noninteractive(const std::string &exe, const fs::path &root) {
    int pipefd[2];
    REQUIRE(pipe(pipefd) == 0);
    auto child = fork();
    REQUIRE(child >= 0);
    if (child == 0) {
        close(pipefd[0]);
        auto null = open("/dev/null", O_RDONLY);
        dup2(null, STDIN_FILENO);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(null);
        close(pipefd[1]);
        execl(exe.c_str(), exe.c_str(), "tui", "--run", root.c_str(), static_cast<char *>(nullptr));
        _exit(126);
    }
    close(pipefd[1]);
    std::string output;
    char data[1024];
    ssize_t n;
    while ((n = read(pipefd[0], data, sizeof(data))) > 0)
        output.append(data, static_cast<std::size_t>(n));
    close(pipefd[0]);
    int status = 0;
    waitpid(child, &status, 0);
    REQUIRE(WIFEXITED(status) && WEXITSTATUS(status) == 2);
    REQUIRE(output.find("interactive terminal") != std::string::npos);
    REQUIRE(output.find('\x1b') == std::string::npos);
}
int main(int argc, char **argv) {
    auto root = fs::temp_directory_path() / ("econ-tui-tests-" + std::to_string(getpid()));
    try {
        REQUIRE(argc == 2);
        REQUIRE(!fs::exists(root));
        fs::create_directories(root);
        auto config = read_json(fs::path(ECON_SOURCE) / "scenarios/demo_town.json");
        RunOptions opt;
        opt.out = root / "town";
        opt.until = 6;
        opt.checkpoint_every = 3;
        run(config, 42, opt);
        opt.out = root / "resumed";
        opt.loaded = load_checkpoint(root / "town/checkpoints/day-0003.json");
        run(config, 42, opt);
        opt.loaded.reset();
        opt.out = root / "summary";
        opt.trace = "summary";
        run(config, 42, opt);
        opt.out = root / "empty";
        opt.until = 0;
        run(config, 42, opt);
        auto labor_config = read_json(fs::path(ECON_SOURCE) / "scenarios/labor_market.json");
        RunOptions dynamic_opt;
        dynamic_opt.out = root / "dynamic";
        dynamic_opt.until = 15;
        dynamic_opt.employment = "dynamic";
        run(labor_config, 7, dynamic_opt);
        auto long_manifest = read_json(root / "empty/manifest.json");
        long_manifest["zz_tui_test_note"] = std::string(2000, 'X') + "END_OF_WRAPPED_NOTE";
        write_json(root / "empty/manifest.json", long_manifest);
        auto before = tree(root);
        RunData data(root / "town");
        REQUIRE(data.first == 0 && data.last == 6);
        REQUIRE(!data.metric(0));
        REQUIRE(data.events(0).empty());
        REQUIRE(data.decisions(0).empty());
        REQUIRE(!data.snapshot(1).at("households")[0].contains("knowledge"));
        REQUIRE(data.snapshot(3).at("households")[0].contains("knowledge"));
        REQUIRE(data.snapshot(6).at("households")[0].contains("knowledge"));
        REQUIRE(data.snapshot_source(1).find("display state") != std::string::npos);
        REQUIRE(data.find_record("decision:1")->at("actor") == "firm:1");
        REQUIRE(!data.find_record("decision:9999999"));
        fails([&] { data.snapshot(7); });
        auto runs = discover(root);
        REQUIRE(runs.size() == 5);
        for (auto &r : runs)
            REQUIRE(r.employment == (r.path == root / "dynamic" ? "dynamic" : "fixed"));
        REQUIRE(discover(root / "missing").empty());
        Inspector browser(root);
        has(browser.render(120, 36), "Recorded runs");
        REQUIRE(browser.key("ENTER"));
        REQUIRE(browser.data() != nullptr);
        REQUIRE(browser.key("o"));
        browser.key("/");
        type(browser, "NO_SUCH_RUN");
        has(browser.render(80, 24), "No runs found");
        browser.key("ESC");
        has(browser.render(80, 24), "records");
        Inspector app(root, root / "town");
        has(app.render(120, 36), "312,000");
        REQUIRE(app.boundary() == 6);
        app.key("0");
        REQUIRE(app.boundary() == 0);
        app.key("3");
        app.key("i");
        type(app, "household:7");
        app.key("]");
        REQUIRE(app.boundary() == 1);
        app.key("ENTER");
        has(app.render(120, 36), "household:7");
        has(app.render(120, 36), "account");
        app.key("END");
        has(app.render(120, 36), "knowledge_note");
        app.key("ESC");
        app.key("d");
        has(app.render(120, 36), "Actor: household:7");
        app.key("ESC");
        app.key("4");
        app.key("/");
        type(app, "FINANCED_PAYROLL");
        has(app.render(120, 36), "4 records");
        app.key("ESC");
        app.key("i");
        type(app, "decision:1");
        REQUIRE(app.boundary() == 1);
        app.key("ENTER");
        has(app.render(120, 36), "whole recorded run");
        app.key("END");
        app.key("ESC");
        app.key("i");
        type(app, "event:1");
        REQUIRE(app.view() == View::Events);
        has(app.render(120, 36), "event:1");
        app.key("g");
        type(app, "999");
        has(app.render(120, 36), "Enter a boundary between");
        app.key("G");
        REQUIRE(app.boundary() == 6);
        app.key("2");
        app.key("END");
        has(app.render(80, 24), "Saved verification: PASSED");
        app.key("v");
        has(app.render(120, 36), "performed now");
        app.key("ESC");
        app.key("?");
        app.key("END");
        app.key("ESC");
        for (auto [w, h] : {std::pair{60, 18}, std::pair{80, 24}, std::pair{120, 36}, std::pair{40, 10}}) {
            auto frame = app.render(w, h);
            frame_bounds(frame, w - 1, h);
            if (w < 60)
                has(frame, "Terminal too small");
        }
        Inspector summary(root, root / "summary");
        summary.key("4");
        has(summary.render(120, 36), "candidate detail omitted");
        Inspector continuation(root, root / "resumed");
        continuation.key("0");
        REQUIRE(continuation.boundary() == 3);
        has(continuation.render(120, 36), "No days completed");
        Inspector empty(root, root / "empty");
        has(empty.render(120, 36), "No days completed");
        REQUIRE(clean("name\x1b[2J\n\t") == "name?[2J??");
        REQUIRE(number(std::numeric_limits<Amount>::min()) == "-9,223,372,036,854,775,808");
        Frame safe(80, 24);
        safe.put(1, 1, "hello\x1b[2J");
        REQUIRE(safe.plain().find('\x1b') == std::string::npos);
        REQUIRE(safe.ansi(false).find(";36m") == std::string::npos);
        noninteractive(argv[1], root / "town");
        {
            Pty terminal(argv[1], root / "town", 120, 36);
            terminal.until("Boundary 6 / 6");
            terminal.send("3", "Cash:");
            terminal.send("i", "Find ID:");
            terminal.send("household:7\r", "household:7");
            terminal.send("0", "Boundary 0 / 6");
            terminal.send("\x1b[C", "Boundary 1 / 6");
            terminal.send("d", "Actor: household:7");
            terminal.send("\r", "whole recorded run");
            terminal.send("\x1b", "records");
            terminal.send("2", "Overview");
            terminal.send("6", "Labor");
            terminal.send("\x1b[C", "Boundary 2 / 6");
            terminal.resize(80, 24);
            terminal.until("Boundary 2 / 6");
            terminal.finish();
        }
        {
            Pty terminal(argv[1], root / "dynamic", 120, 36);
            terminal.until("Employment mode: dynamic");
            terminal.send("6", "paid 1,000 c");
            terminal.send("g", "Boundary:");
            terminal.send("3\r", "Boundary 3 / 15");
            terminal.send("/", "Filter:");
            terminal.send("posting:1\r", "Filter: posting:1");
            terminal.send("j", "vacancy board entry");
            terminal.send("o", "Recorded runs");
            terminal.send("n", "NEW RUN");
            terminal.send("jjj ", "fixed  [dynamic]");
            terminal.send("\x1b", "form closed");
            terminal.send("2", "Employment mode: dynamic");
            terminal.send("7", "Ledger: town > entities");
            terminal.send("\r", "> transactions");
            terminal.send("\x1b", "Ledger: town > entities");
            terminal.finish();
        }
        {
            Pty terminal(argv[1], root / "town", 80, 24);
            terminal.until("Boundary 6 / 6");
            terminal.finish(true);
        }
        {
            Pty failed_open(argv[1], root / "missing-run", 80, 24);
            failed_open.finish(false, 3, false);
        }
        // Labor view: per-person records of job, work, pay and changes for the selected day.
        Inspector labor(root, root / "dynamic");
        has(labor.render(120, 36), "Employment mode: dynamic");
        labor.key("g");
        type(labor, "4"); // day 3: three hires and a quit, one offer declined on reservation wage
        auto overview = labor.render(120, 36);
        has(overview, "Job search: 12 searched | 4 applied (3 hires, 0 switches, 1 quits) | 1 declined");
        has(overview, "acceptance 80%");
        has(overview, "Declined offers by reason: BELOW_RESERVATION_WAGE=1");
        labor.key("6");
        for (int n = 0; n < 3; ++n)
            labor.key("DOWN"); // household:12, who turned down an offer below its reservation wage
        auto searched = labor.render(120, 36);
        has(searched, "household:12  (none)  idle  search: declined");
        has(searched, "Search today: DECLINED (NO_PREFERABLE_OFFER)");
        labor.key("3");
        labor.key("/");
        type(labor, "household:12\"");
        has(labor.render(120, 36), "Job preferences: reservation 1,050 c | gain >= 10% and");
        labor.key("ESC");
        labor.key("6");
        REQUIRE(labor.view() == View::Labor);
        has(labor.render(120, 36), "12 records");
        labor.key("g");
        type(labor, "3"); // boundary 3 shows day 2: layoffs scheduled, firm:2's posting raised to 1,100
        auto day2 = labor.render(120, 36);
        has(day2, "household:1  firm:1  paid 1,000 c  scheduled ->(none");
        has(day2, "Scheduled today: firm:1 -> (none) effective day 3 (LAYO");
        has(day2, "Pending at boundary: firm:1 -> (none) on day 3");
        has(day2, "posting:1  firm:2  3 open at 1,100 c  unfilled 0 d"); // one seat taken by a switcher
        labor.key("/");
        type(labor, "posting:1");
        labor.key("DOWN"); // the switcher who answered posting:1 matches too; the posting row is second
        auto posting = labor.render(120, 36);
        has(posting, "Filter: posting:1  2 records");
        has(posting, "vacancy board entry of firm:2");
        has(posting, "applications so far 1");
        has(posting, "Requires: experience >= 2 days");
        has(posting, "Posted because: HIRE_INSUFFICIENT_HISTORY");
        labor.key("e");
        REQUIRE(labor.view() == View::Events);
        auto adjusted = labor.render(120, 36);
        has(adjusted, "posting_adjusted  firm:2  posting:1  wage");
        has(adjusted, "Offer: 1000 -> 1100");
        has(adjusted, "Reason: UNFILLED");
        labor.key("ESC");
        labor.key("6");
        labor.key("g");
        type(labor, "4"); // day 3: the laid-off workers take firm:2's raised offer
        labor.key("/");
        type(labor, "HIRED");
        has(labor.render(120, 36), "3 records");
        has(labor.render(120, 36), "at 1,100 c via posting:1");
        labor.key("ENTER");
        auto record = labor.render(120, 36);
        has(record, "Labor record (job, work, payroll, changes)");
        has(record, "\"scheduled\"");
        labor.key("ESC");
        labor.key("ESC");
        labor.key("d");
        REQUIRE(labor.view() == View::Decisions);
        has(labor.render(120, 36), "job_search-2");
        labor.key("6");
        labor.key("0");
        has(labor.render(120, 36), "opening state");
        labor.key("3");
        labor.key("/");
        type(labor, "household:1\"");
        auto entity = labor.render(120, 36);
        has(entity, "unpaid streak");
        labor.key("ESC");
        labor.key("/");
        type(labor, "firm:2");
        auto firm_entity = labor.render(120, 36);
        has(firm_entity, "Positions: 6 | labor required 3 | output/worker 2");
        has(firm_entity, "Observed demand (units/day");
        has(firm_entity, "Offers: 900..1300 step 100");
        labor.key("5");
        labor.key("g");
        type(labor, "4");
        labor.key("/");
        type(labor, "employment_changed");
        auto employment_event = labor.render(120, 36);
        has(employment_event, "In force from this day's employment phase");
        labor.key("ESC");
        labor.key("TAB");
        REQUIRE(labor.view() == View::Labor);
        labor.key("TAB");
        REQUIRE(labor.view() == View::Ledger);
        labor.key("TAB");
        REQUIRE(labor.view() == View::Runs);
        // Ledger levels: every entity's day, then one entity's transactions with a running balance,
        // then the full record; Esc climbs back one level at a time.
        Inspector ledger(root, root / "dynamic");
        ledger.key("7");
        REQUIRE(ledger.view() == View::Ledger);
        ledger.key("g");
        type(ledger, "4");
        auto town = ledger.render(130, 36);
        has(town, "Ledger: town > entities  16 records");
        has(town, "firm:2  cash 79,000 (-3,100)  wages -3,100  labor 3 used");
        has(town, "Cash in 2,100 c | out 0 c | net +2,100 c | opening 7,600 c");
        ledger.key("DOWN");
        has(ledger.render(130, 36), "Payroll: 3,100 c for 3 worker-days | used in production 3");
        ledger.key("ENTER");
        auto transactions = ledger.render(130, 36);
        has(transactions, "Ledger: town > firm:2 > transactions");
        has(transactions, "wage  cash -1,000  labor +1  -> 81,100 c");
        has(transactions, "production  labor -3  food +6  -> 79,000 c");
        has(transactions, "order_reserved wholesale  reserved food +6");
        has(transactions, "Esc: back to all entities");
        ledger.key("END");
        ledger.key("ENTER");
        has(ledger.render(130, 36), "Recorded transaction (firm:2)");
        ledger.key("ESC");
        has(ledger.render(130, 36), "Ledger: town > firm:2 > transactions");
        ledger.key("ESC");
        has(ledger.render(130, 36), "Ledger: town > entities");
        has(ledger.render(130, 36), "> firm:2  cash"); // the entity we came from stays selected
        ledger.key("/");
        type(ledger, "household:5");
        ledger.key("ENTER");
        has(ledger.render(130, 36), "Ledger: town > household:5 > transactions");
        has(ledger.render(130, 36), "wage  cash +1,000  -> ");
        ledger.key("ESC");
        ledger.key("ESC");
        ledger.key("e");
        REQUIRE(ledger.view() == View::Events);
        ledger.key("7");
        ledger.key("0");
        has(ledger.render(130, 36), "opening state, no day completed");
        ledger.key("ENTER");
        has(ledger.render(130, 36), "No day completed at the opening boundary");
        ledger.key("2");
        ledger.key("G");
        auto circuit = ledger.render(130, 36);
        has(circuit, "worker-days bought with payroll");
        has(circuit, "Goods: made ");
        has(circuit, "Cash: wages ");
        // New-run form: the employment mode is chosen in the inspector and launched through run().
        Inspector creator(root);
        REQUIRE(!creator.form_open());
        creator.key("n");
        REQUIRE(creator.form_open());
        auto form = creator.render(120, 36);
        has(form, "NEW RUN");
        has(form, "[fixed]  dynamic");
        for (auto [width, height] : {std::pair{60, 18}, std::pair{80, 24}, std::pair{120, 36}}) {
            creator.key("HOME");
            for (auto label : {"Scenario", "Seed", "Days", "Employment", "Trace", "Checkpoint every",
                               "Run name", "Start run"}) {
                auto focused = creator.render(width, height);
                has(focused, std::string("> ") + label);
                has(focused, "Esc cancel");
                creator.key("DOWN");
            }
        }
        creator.key("HOME");
        creator.key("ENTER");
        retype(creator, (fs::path(ECON_SOURCE) / "scenarios/accounting_cycle.json").string());
        creator.key("DOWN");
        creator.key("ENTER");
        retype(creator, "3");
        creator.key("DOWN");
        creator.key("ENTER");
        retype(creator, "4");
        creator.key("DOWN");
        creator.key(" ");
        REQUIRE(creator.form().employment == "dynamic");
        has(creator.render(120, 36), "fixed  [dynamic]");
        REQUIRE(creator.form().name == "accounting_cycle-dynamic-seed3-4d");
        creator.key("DOWN");
        creator.key("DOWN");
        creator.key("ENTER");
        retype(creator, "0");
        creator.key("END");
        REQUIRE(!creator.wait_message("ENTER").empty());
        REQUIRE(creator.wait_message("ENTER").find("dynamic") != std::string::npos);
        auto before_launch = tree(root);
        creator.key("ENTER");
        REQUIRE(!creator.form_open());
        REQUIRE(creator.data() != nullptr && creator.view() == View::Overview);
        has(creator.render(120, 36), "Run complete with dynamic employment");
        auto created = read_json(root / "accounting_cycle-dynamic-seed3-4d/manifest.json");
        REQUIRE(created.at("employment") == "dynamic" && created.at("seed") == 3 &&
                created.at("until_day") == 4);
        for (auto &[file, hash] : before_launch.items())
            REQUIRE(tree(root).at(file) == hash); // existing runs untouched
        creator.key("o");
        has(creator.render(120, 36), "accounting_cycle-dynamic-seed3-4d");
        creator.key("n");
        creator.key("END");
        creator.key("ENTER"); // same name again: refused before anything is written
        REQUIRE(creator.form_open());
        has(creator.render(120, 36), "Output already exists");
        creator.key("UP");
        creator.key("ENTER");
        retype(creator, "bad/name");
        creator.key("END");
        creator.key("ENTER");
        has(creator.render(120, 36), "single directory name");
        creator.key("ESC");
        REQUIRE(!creator.form_open());
        fs::remove_all(root / "accounting_cycle-dynamic-seed3-4d");
        REQUIRE(before == tree(root));
        // Minimum-size text is recoverable through wrapping and exact page traversal.
        Inspector help_page(root, root / "town"), help_lines(root, root / "town");
        help_page.key("?");
        help_lines.key("?");
        help_page.render(60, 18);
        help_lines.render(60, 18);
        help_page.key("PAGEDOWN");
        for (int n = 0; n < 6; ++n)
            help_lines.key("DOWN");
        REQUIRE(help_page.render(60, 18).plain() == help_lines.render(60, 18).plain());
        Inspector fresh_help(root, root / "town");
        fresh_help.key("?");
        fresh_help.key("END");
        has(fresh_help.render(60, 18), "q / Ctrl-C / Ctrl-D");
        Inspector long_record(root, root / "empty");
        long_record.key("m");
        long_record.render(60, 18);
        long_record.key("END");
        has(long_record.render(60, 18), "END_OF_WRAPPED_NOTE");
        Inspector narrow(root, root / "town");
        has(narrow.render(60, 18), "q quit");
        has(narrow.render(60, 18), "5Events");
        has(narrow.render(60, 18), "7Ledger");
        narrow.key("/");
        REQUIRE(!narrow.key("CTRL_C"));
        // The optional saved report must not prevent browsing or a fresh read-only verification.
        auto original_report = read_json(root / "empty/verification.json");
        for (auto bad : std::vector<Json>{
                 Json::object(),
                 Json{{"schema", "verification-1"}, {"passed", "true"}, {"checks", Json::array()}}}) {
            write_json(root / "empty/verification.json", bad);
            auto bad_before = tree(root);
            Inspector invalid_report(root, root / "empty");
            has(invalid_report.render(120, 36), "Saved verification: INVALID");
            invalid_report.key("v");
            has(invalid_report.render(120, 36), "performed now");
            REQUIRE(bad_before == tree(root));
        }
        {
            std::ofstream out(root / "empty/verification.json");
            out << "{truncated";
        }
        Inspector broken_report(root, root / "empty");
        has(broken_report.render(120, 36), "Saved verification: INVALID");
        write_json(root / "empty/verification.json", original_report);
        REQUIRE(before == tree(root));
        // Valid JSON with impossible monetary bounds must fail before rendering arithmetic.
        auto replay_path = root / "town/replay.jsonl";
        std::ifstream replay_input(replay_path);
        std::string replay_bytes{std::istreambuf_iterator<char>(replay_input), {}};
        auto first_newline = replay_bytes.find('\n');
        auto bad_account = Json::parse(replay_bytes.substr(0, first_newline));
        bad_account["accounts"][0]["cash"] = std::numeric_limits<Amount>::min();
        bad_account["accounts"][0]["reserved_cash"] = std::numeric_limits<Amount>::max();
        {
            std::ofstream out(replay_path);
            out << bad_account.dump() << replay_bytes.substr(first_newline);
        }
        RunData bad_bounds(root / "town");
        fails([&] { bad_bounds.snapshot(1); });
        {
            std::ofstream out(replay_path);
            out << replay_bytes;
        }
        // Corrupt/missing history never turns into a plausible empty timeline.
        auto corrupt = read_json(root / "empty/manifest.json");
        corrupt["model"] = "unknown";
        write_json(root / "empty/manifest.json", corrupt);
        fails([&] { RunData invalid(root / "empty"); });
        {
            std::ofstream out(root / "summary/replay.jsonl");
            out << "{truncated";
        }
        fails([&] { RunData invalid(root / "summary"); });
        fs::remove(root / "resumed/decisions.jsonl");
        fails([&] { RunData invalid(root / "resumed"); });
        fs::remove_all(root);
        std::cout
            << "PASS TUI indexed history, historical knowledge, navigation, filters, evidence, verification, "
               "labor records, new-run form with employment choice, resize, terminal restoration, signals, "
               "non-TTY rejection and read-only artifacts\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "FAIL " << e.what() << " (fixtures retained in " << root << ")\n";
        return 1;
    }
}
