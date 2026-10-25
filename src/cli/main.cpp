#include <charconv>
#include <cstdlib>
#include <econ/econ.hpp>
#include <econ/tui.hpp>
#include <iostream>
#include <set>
namespace {
using namespace econ;
void help() {
    std::cout << R"(econ 0.1.0 — inspectable cash-only economy (C++20)
Usage: econ COMMAND [options]
  tui       [--run DIR | --runs-dir DIR] [--no-color]
  validate  --scenario FILE [--json]
  run       --scenario FILE --seed N --until-day D --out DIR
  benchmark --scenario FILE --seed N --until-day D --out DIR [--json]
  resume    --checkpoint FILE --until-day D --out DIR
  branch    --checkpoint FILE --intervention FILE --until-day D --out DIR
  verify    --run DIR [--json]
  inspect   --run DIR --entity ID [--json]
  explain   --run DIR --decision ID [--json]
  compare   --baseline DIR --variant DIR --from-day D --until-day D [--json]
Execution options: --trace audit|summary --checkpoint-every-days N --overwrite
tui is interactive; other command results are JSON. Diagnostics go to stderr.
Day D is exclusive. tui defaults to browsing ./runs and changes no run files.
Exit codes: 0 success, 2 input/schema, 3 I/O, 4 incompatible persistence/comparison,
            5 accounting invariant/overflow, 6 internal error.
)";
}
Amount parse_number(const std::string &s) {
    Amount n = 0;
    auto [p, err] = std::from_chars(s.data(), s.data() + s.size(), n);
    if (err != std::errc{} || p != s.data() + s.size() || n < 0)
        throw Error(2, "invalid nonnegative integer: " + s);
    return n;
}
} // namespace
int main(int argc, char **argv) {
    using namespace econ;
    try {
        if (argc == 1)
            throw Error(2, "command required; use --help");
        std::string cmd = argv[1];
        if ((cmd == "--help" || cmd == "--version") && argc == 2) {
            if (cmd == "--help")
                help();
            else
                std::cout << "{\"version\":\"0.1.0\",\"model\":\"econ-1\",\"build\":\"" << ECON_BUILD
                          << "\"}\n";
            return 0;
        }
        std::map<std::string, std::set<std::string>> allowed = {
            {"tui", {"run", "runs-dir", "no-color"}},
            {"validate", {"scenario", "json"}},
            {"run", {"scenario", "seed", "until-day", "out", "trace", "checkpoint-every-days", "overwrite"}},
            {"benchmark",
             {"scenario", "seed", "until-day", "out", "trace", "checkpoint-every-days", "overwrite", "json"}},
            {"resume", {"checkpoint", "until-day", "out", "trace", "checkpoint-every-days", "overwrite"}},
            {"branch",
             {"checkpoint", "intervention", "until-day", "out", "trace", "checkpoint-every-days",
              "overwrite"}},
            {"verify", {"run", "json"}},
            {"inspect", {"run", "entity", "json"}},
            {"explain", {"run", "decision", "json"}},
            {"compare", {"baseline", "variant", "from-day", "until-day", "json"}}};
        if (!allowed.contains(cmd))
            throw Error(2, "unknown command: " + cmd);
        std::map<std::string, std::string> flags;
        for (int i = 2; i < argc; ++i) {
            std::string flag = argv[i];
            if (flag.rfind("--", 0) != 0)
                throw Error(2, "expected flag: " + flag);
            flag = flag.substr(2);
            if (!allowed.at(cmd).contains(flag) || flags.contains(flag))
                throw Error(2, "unknown/duplicate flag: " + flag);
            if (flag == "json" || flag == "overwrite" || flag == "no-color")
                flags[flag] = "true";
            else {
                if (i + 1 >= argc || std::string(argv[i + 1]).rfind("--", 0) == 0)
                    throw Error(2, "missing value: " + flag);
                flags[flag] = argv[++i];
            }
        }
        auto need = [&](const std::string &k) -> std::string {
            if (!flags.contains(k) || flags.at(k).empty())
                throw Error(2, "missing --" + k);
            return flags.at(k);
        };
        auto number = [&](const std::string &k, Amount fallback) {
            return flags.contains(k) ? parse_number(flags.at(k)) : fallback;
        };
        if (cmd == "tui") {
            if (flags.contains("run") && flags.contains("runs-dir"))
                throw Error(2, "Use either --run or --runs-dir for tui");
            tui::Options options;
            if (flags.contains("run"))
                options.run = need("run");
            if (flags.contains("runs-dir"))
                options.runs_directory = need("runs-dir");
            options.color = !flags.contains("no-color") && std::getenv("NO_COLOR") == nullptr;
            return tui::launch(options);
        }
        Json result;
        if (cmd == "validate") {
            auto j = normalize(read_json(need("scenario")));
            result = {{"valid", true},
                      {"schema", j.at("schema")},
                      {"digest", digest(j)},
                      {"households", j.at("households").size()},
                      {"firms", j.at("firms").size()}};
        } else if (cmd == "verify") {
            result = verify(need("run"));
            std::cout << result.dump() << '\n';
            return result.at("passed").get<bool>() ? 0 : 5;
        } else if (cmd == "inspect")
            result = inspect(need("run"), need("entity"));
        else if (cmd == "explain")
            result = explain(need("run"), need("decision"));
        else if (cmd == "compare")
            result = compare(need("baseline"), need("variant"), parse_number(need("from-day")),
                             parse_number(need("until-day")));
        else {
            RunOptions opt;
            opt.out = need("out");
            opt.until = parse_number(need("until-day"));
            opt.checkpoint_every = number("checkpoint-every-days", 0);
            opt.trace =
                flags.contains("trace") ? flags.at("trace") : (cmd == "benchmark" ? "summary" : "audit");
            opt.overwrite = flags.contains("overwrite");
            Json scenario;
            std::uint64_t seed = 0;
            if (cmd == "resume" || cmd == "branch") {
                opt.loaded = load_checkpoint(need("checkpoint"));
                scenario = opt.loaded->scenario;
                seed = opt.loaded->state.seed;
                if (cmd == "branch")
                    opt.intervention = read_json(need("intervention"));
            } else {
                scenario = read_json(need("scenario"));
                seed = static_cast<std::uint64_t>(parse_number(need("seed")));
            }
            result = run(scenario, seed, opt);
        }
        std::cout << result.dump() << '\n';
        return 0;
    } catch (const Error &e) {
        std::cerr << Json{{"error", e.what()}, {"exit_code", e.code}}.dump() << '\n';
        return e.code;
    } catch (const Json::exception &e) {
        std::cerr << Json{{"error", e.what()}, {"exit_code", 2}}.dump() << '\n';
        return 2;
    } catch (const std::filesystem::filesystem_error &e) {
        std::cerr << Json{{"error", e.what()}, {"exit_code", 3}}.dump() << '\n';
        return 3;
    } catch (const std::exception &e) {
        std::cerr << Json{{"error", e.what()}, {"exit_code", 6}}.dump() << '\n';
        return 6;
    }
}
