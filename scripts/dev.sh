#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_JOBS="${ECON_JOBS:-2}"

usage() {
    cat <<'EOF'
Usage: ./scripts/dev.sh COMMAND [OPTION]

  build [debug|release|sanitize]  Configure and build (default: release).
  test                          Build and test Debug and Release.
  test debug|release|sanitize    Build and test one configuration.
  test all                      Build and test all three configurations.
  tui [RUN_DIRECTORY]           Build Release and open the interactive inspector.
  start [OUTPUT_DIRECTORY]      Build Release and run the full demo experiment.
  help                          Show this help.

start runs accounting, the 60-day town, day-30 resume/no-op/price-board
continuations, the labor-market scenario with fixed and dynamic employment,
inspection, explanation, comparison, and verification.
It preserves existing outputs. By default, it creates a unique directory
under the project's runs/; an explicit output directory must not exist.
Relative output paths are resolved from your current working directory.

Works from any directory. Set ECON_JOBS to change build parallelism:
  ECON_JOBS=4 ./scripts/dev.sh test
EOF
}

fail() {
    printf 'Error: %s\n' "$*" >&2
    exit 2
}

progress() {
    printf '\n==> %s\n' "$*" >&2
}

build_config() {
    local config="$1" build_type sanitizer=OFF
    case "$config" in
        debug) build_type=Debug ;;
        release) build_type=Release ;;
        sanitize) build_type=Debug; sanitizer=ON ;;
        *) fail "Unknown configuration: $config" ;;
    esac
    progress "Building $config"
    cmake -S "$PROJECT_ROOT" -B "$PROJECT_ROOT/build/$config" \
        -DCMAKE_BUILD_TYPE="$build_type" -DECON_SANITIZE="$sanitizer"
    cmake --build "$PROJECT_ROOT/build/$config" --parallel "$BUILD_JOBS"
}

test_config() {
    local config="$1"
    build_config "$config"
    progress "Testing $config"
    ctest --test-dir "$PROJECT_ROOT/build/$config" --output-on-failure
}

start_demo() {
    local output="${1:-}" econ="$PROJECT_ROOT/build/release/econ"
    local reports checkpoint
    if [[ -n "$output" ]]; then
        [[ "$output" != -* ]] || fail "Unknown option: $output; use --help"
        [[ ! -e "$output" && ! -L "$output" ]] || fail "Output already exists: $output"
        output="$(realpath -m -- "$output")"
        [[ ! -e "$output" && ! -L "$output" ]] || fail "Output already exists: $output"
    fi
    build_config release
    if [[ -z "$output" ]]; then
        mkdir -p -- "$PROJECT_ROOT/runs"
        output="$(mktemp -d "$PROJECT_ROOT/runs/start-XXXXXXXX")"
    else
        mkdir -p -- "$(dirname -- "$output")"
        # Do not reuse a directory another process created while we built.
        mkdir -- "$output"
    fi
    reports="$output/reports"
    mkdir -- "$reports"
    printf '\nExperiment output: %s\n' "$output"

    progress "Validating scenarios and running the accounting cycle"
    "$econ" validate --scenario "$PROJECT_ROOT/scenarios/accounting_cycle.json" --json > "$reports/validate-accounting.json"
    "$econ" validate --scenario "$PROJECT_ROOT/scenarios/demo_town.json" --json > "$reports/validate-town.json"
    "$econ" run --scenario "$PROJECT_ROOT/scenarios/accounting_cycle.json" --seed 42 \
        --until-day 30 --trace audit --out "$output/accounting" > "$reports/accounting.json"

    progress "Running the town for 60 days"
    "$econ" run --scenario "$PROJECT_ROOT/scenarios/demo_town.json" --seed 42 \
        --until-day 60 --checkpoint-every-days 15 --trace audit \
        --out "$output/baseline" > "$reports/baseline.json"
    "$econ" inspect --run "$output/baseline" --entity household:7 --json > "$reports/household-7.json"
    "$econ" explain --run "$output/baseline" --decision decision:1 --json > "$reports/decision-1.json"

    checkpoint="$output/baseline/checkpoints/day-0030.json"
    progress "Resuming and branching from day 30"
    "$econ" resume --checkpoint "$checkpoint" --until-day 60 --trace audit \
        --out "$output/resumed" > "$reports/resumed.json"
    "$econ" branch --checkpoint "$checkpoint" \
        --intervention "$PROJECT_ROOT/scenarios/interventions/noop.json" \
        --until-day 60 --trace audit --out "$output/noop" > "$reports/noop.json"
    "$econ" branch --checkpoint "$checkpoint" \
        --intervention "$PROJECT_ROOT/scenarios/interventions/public_price_board.json" \
        --until-day 60 --trace audit --out "$output/price_board" > "$reports/price-board.json"

    progress "Running the labor market with fixed and dynamic employment"
    "$econ" validate --scenario "$PROJECT_ROOT/scenarios/labor_market.json" --json > "$reports/validate-labor.json"
    "$econ" run --scenario "$PROJECT_ROOT/scenarios/labor_market.json" --seed 7 \
        --until-day 40 --checkpoint-every-days 10 --trace audit --employment fixed \
        --out "$output/labor_fixed" > "$reports/labor-fixed.json"
    "$econ" run --scenario "$PROJECT_ROOT/scenarios/labor_market.json" --seed 7 \
        --until-day 40 --checkpoint-every-days 10 --trace audit --employment dynamic \
        --out "$output/labor_dynamic" > "$reports/labor-dynamic.json"
    "$econ" inspect --run "$output/labor_dynamic" --entity household:1 --json > "$reports/labor-household-1.json"
    "$econ" verify --run "$output/labor_dynamic" --json > "$reports/verification-labor-dynamic.json"
    "$econ" compare --baseline "$output/labor_fixed" --variant "$output/labor_dynamic" \
        --from-day 0 --until-day 40 --json > "$reports/labor-comparison.json"

    progress "Verifying and comparing continuations"
    "$econ" verify --run "$output/baseline" --json > "$reports/verification.json"
    "$econ" compare --baseline "$output/baseline" --variant "$output/resumed" \
        --from-day 30 --until-day 60 --json > "$reports/compare-resumed.json"
    "$econ" compare --baseline "$output/baseline" --variant "$output/noop" \
        --from-day 30 --until-day 60 --json > "$reports/compare-noop.json"
    "$econ" compare --baseline "$output/baseline" --variant "$output/price_board" \
        --from-day 30 --until-day 60 --json > "$reports/compare-price-board.json"

    printf '\nDemo complete.\nRun artifacts: %s\nJSON reports: %s\n' "$output" "$reports"
    printf 'Inspect a household:\n  %q inspect --run %q --entity household:7 --json\n' "$econ" "$output/baseline"
    printf 'Browse the dynamic-employment run (6 Labor view):\n  %q tui --run %q\n' "$econ" "$output/labor_dynamic"
}

command="${1:-help}"
if (( $# > 0 )); then shift; fi
case "$command" in
    help|--help|-h)
        (( $# == 0 )) || fail "help takes no arguments"
        usage
        ;;
    build|test|start|tui)
        (( $# <= 1 )) || fail "Too many arguments; use --help"
        [[ "$BUILD_JOBS" =~ ^[1-9][0-9]*$ ]] || fail "ECON_JOBS must be a positive integer"
        case "$command" in
            build) build_config "${1:-release}" ;;
            test)
                case "${1:-standard}" in
                    standard) configs=(debug release) ;;
                    all) configs=(debug release sanitize) ;;
                    debug|release|sanitize) configs=("$1") ;;
                    *) fail "Unknown test configuration: $1" ;;
                esac
                for config in "${configs[@]}"; do test_config "$config"; done
                ;;
            start) start_demo "${1:-}" ;;
            tui)
                if (( $# > 0 )); then
                    [[ "$1" != -* ]] || fail "Unknown option: $1; use --help"
                    run_directory="$(realpath -m -- "$1")"
                fi
                build_config release
                if (( $# > 0 )); then
                    exec "$PROJECT_ROOT/build/release/econ" tui --run "$run_directory"
                else
                    exec "$PROJECT_ROOT/build/release/econ" tui --runs-dir "$PROJECT_ROOT/runs"
                fi
                ;;
        esac
        ;;
    *) fail "Unknown command: $command; use --help" ;;
esac
