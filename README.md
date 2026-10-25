# Totoconomy

A C++20, CLI-only miniature economy whose decisions and transactions can be inspected. Stage 1 includes a closed accounting fixture, a synthetic 24-household town, limited price knowledge, delayed procurement, checkpoints and paired information experiments. An interactive terminal inspector is available; there is no graphical or video renderer yet.

Requires Linux, a C++20 GCC/Clang toolchain, CMake 3.20+, and a build tool. Tested with GCC 16.2.1 and CMake 4.4.4 on x86-64 Linux. JSON 3.11.3 is vendored with its MIT license. Builds, tests, and runs require no network, services, keys, graphics, or Python.

For the usual starting and testing procedures, use the Bash helper:

```sh
./scripts/dev.sh start          # Build Release and run the complete demo experiment
./scripts/dev.sh tui            # Build Release and browse saved simulations
./scripts/dev.sh tui runs/baseline  # Inspect a particular run
./scripts/dev.sh test           # Build and run all CTest gates in Debug and Release
./scripts/dev.sh test sanitize  # Build and test with ASan + UBSan (takes several minutes)
./scripts/dev.sh test all       # Debug, Release, and sanitizers
./scripts/dev.sh build          # Build Release only
./scripts/dev.sh --help
```

`start` runs the accounting fixture, the 60-day town, inspection/explanation, day-30 resume and both branches, verification, and paired comparisons. It creates a unique `runs/start-*` directory and prints its location; JSON command reports live in its `reports/` subdirectory. To choose the destination, use `./scripts/dev.sh start runs/my-experiment`; the directory must not already exist. The helper works from any working directory, with explicit relative output paths interpreted from that directory. Set `ECON_JOBS=4` to change the default two build jobs. Bash and standard Linux coreutils are required for the helper.

The inspector provides run selection, overview, entity balances/inventories, recorded decisions and events, day-boundary navigation, filters and exact-ID lookup. Use `1`–`5` or Tab to switch views, `[`/`]` to change day, Enter for full evidence, `?` for help, and `q` to quit. It requires an interactive terminal (60×18 minimum) and changes no run files. See [TUI controls and data limits](docs/TUI.md).

Equivalent individual commands:

```sh
cmake -S . -B build/debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build/debug --parallel 2
ctest --test-dir build/debug --output-on-failure
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release --parallel 2
ctest --test-dir build/release --output-on-failure

./build/release/econ --help
./build/release/econ --version
./build/release/econ validate --scenario scenarios/accounting_cycle.json --json
./build/release/econ run --scenario scenarios/accounting_cycle.json --seed 42 --until-day 30 --trace audit --out runs/accounting
./build/release/econ run --scenario scenarios/demo_town.json --seed 42 --until-day 60 --checkpoint-every-days 15 --trace audit --out runs/baseline
./build/release/econ inspect --run runs/baseline --entity household:7 --json
./build/release/econ explain --run runs/baseline --decision decision:1 --json
./build/release/econ resume --checkpoint runs/baseline/checkpoints/day-0030.json --until-day 60 --trace audit --out runs/resumed
./build/release/econ branch --checkpoint runs/baseline/checkpoints/day-0030.json --intervention scenarios/interventions/noop.json --until-day 60 --trace audit --out runs/noop
./build/release/econ branch --checkpoint runs/baseline/checkpoints/day-0030.json --intervention scenarios/interventions/public_price_board.json --until-day 60 --trace audit --out runs/price_board
./build/release/econ compare --baseline runs/baseline --variant runs/price_board --from-day 30 --until-day 60 --json
./build/release/econ verify --run runs/baseline --json
./build/release/econ benchmark --scenario scenarios/demo_town.json --seed 42 --until-day 365 --trace summary --out runs/benchmark --json
```

Run outputs must be new directories. To explicitly replace one run, add `--overwrite`; parent run directories and their descendants are protected during continuation. Tests use isolated output locations. `decision:1` and `household:7` are real demo records; the executable CLI acceptance test also reads a decision ID from the actual output.

`--until-day D` completes days `[0,D)`; day-0030 checkpoints resume **before day 30**. Continuations write only their own shared interval. A branch must advance beyond its intervention boundary. Noninteractive command results are JSON (except help); `tui` renders an interactive terminal interface. CLI errors are JSON on stderr. Exit codes: 0 success, 2 input/schema, 3 I/O, 4 incompatible checkpoint/comparison, 5 invariant/overflow, 6 internal failure. Unknown or duplicate flags are rejected.

Optional sanitizer build:

```sh
cmake -S . -B build/sanitize -DCMAKE_BUILD_TYPE=Debug -DECON_SANITIZE=ON
cmake --build build/sanitize --parallel 2
ctest --test-dir build/sanitize --output-on-failure
```

See the [executed results](docs/RESULTS.md), [model](docs/MODEL.md), [architecture and decisions](docs/ARCHITECTURE.md), [formats](docs/FORMATS.md), [determinism](docs/DETERMINISM.md), [verification](docs/VERIFICATION.md), [performance](docs/PERFORMANCE.md), and [roadmap/status](docs/ROADMAP.md). The original implementation plan is in [PLAN.md](docs/PLAN.md).

Licensed under the [MIT License](LICENSE). The vendored nlohmann/json library retains its [MIT license and copyright notice](third_party/nlohmann/LICENSE.MIT).
