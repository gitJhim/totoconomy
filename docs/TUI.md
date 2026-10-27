# Interactive simulation inspector

Build and open the run browser:

```sh
./scripts/dev.sh tui
```

Open a particular run (the helper builds Release first):

```sh
./scripts/dev.sh tui runs/baseline
```

The equivalent direct CLI commands are:

```sh
./build/release/econ tui
./build/release/econ tui --runs-dir runs
./build/release/econ tui --run runs/baseline
./build/release/econ tui --run runs/baseline --no-color
```

With no path, `econ tui` browses `./runs` relative to the current directory. The helper always browses this project's `runs/`; its explicit relative run path is interpreted from your current working directory. Run discovery includes bounded nested experiment folders and lists their statuses. Only completed runs can be opened. Press `n` in the run list to create a run, including when no runs exist. Choose a scenario, seed, duration and fixed/dynamic employment, then Start run. Left/Right or Space toggles Employment. New outputs go under the browsed runs directory; existing outputs cannot be overwritten. `R` refreshes the browser.

## Navigation

| Key | Action |
| --- | --- |
| `1`–`7`, Tab, Shift-Tab | Runs, Overview, Entities, Decisions, Events, Labor, Ledger |
| Up/Down or `j`/`k` | Select a row; scroll Overview, Help or a full record |
| Page Up/Down, Home/End | Page or jump through the current list/record |
| Enter | Open selected run or evidence; drill from Ledger into entity transactions and then event details |
| `[` / `]`, Left/Right or `h`/`l` | Previous / next recorded day boundary |
| `g` | Enter an exact day boundary within this run |
| `0` / `G` | Opening / final boundary |
| `/` | Case-insensitive filter of the current list (Overview opens Entities) |
| `i` | Find exact `household:N`, `firm:N`, `decision:N` or `event:N` |
| `n` in Runs | Open the new-run form |
| `d` / `e` on an entity or ledger row | Its decisions / events for the selected day |
| Esc | Back, cancel input, or clear text/actor filters |
| `m` | Inspect the actual manifest |
| `v` | Perform independent persisted-record verification now |
| `R` / `o` | Reload artifacts / return to run browser |
| `?` | Keyboard help |
| `q`, Ctrl-C or Ctrl-D | Quit and restore the terminal (Ctrl-C/D also work during input) |

Select a household or firm in Entities, then change the day to follow the same ID. In Decisions or Events, an actor filter stays active across day changes. Find-ID scans recorded logs and jumps to the record's day; it does not run a policy. Filtering applies to the selected day, not the entire history.

## Time and evidence

Boundary `D` means days before `D` are complete. The activity, decision and event views show **day `D-1`**, while entity balances show the state **at boundary `D`**. A run's opening boundary has no activity within that run. A resumed day-30 continuation starts at 30, not zero; inspect its parent to see earlier history.

The viewer uses initial/final states, an exact-boundary checkpoint when available, and otherwise the recorded display snapshot. The source is disclosed. A display snapshot does not contain household knowledge; entity details say so and direct you to the day's decisions or a checkpoint. Final household memories are never substituted into earlier snapshots.

Opening a decision shows its recorded evidence and **all matching resulting events in that recorded run**, including later deliveries. This detail view explicitly says “whole recorded run”; its linked events may be later than the selected boundary. Summary traces explicitly identify omitted candidate details. No narration or policy intention is invented.

Overview distinguishes a saved verification report from a fresh check. An absent or malformed optional report does not prevent inspection; `v` performs the actual verifier and shows its result without writing a replacement file. Saved PASSED is not treated as a new verification of the files on disk.

## Terminal and data contract

The inspector requires an interactive ANSI-compatible terminal on both stdin and stdout. It refuses pipes and `TERM=dumb`; use `inspect`/`explain`/`verify --json` for scripting. It supports 60×18 and larger (80×24 or 120×36 recommended), responds to resizing, and shows a resize message below the minimum. Extremely large terminal dimensions are bounded to 400×120. Character grids are ASCII; non-ASCII display labels are replaced safely, while full JSON uses Unicode escapes. This prevents artifact strings from sending terminal controls. Actual file paths/IDs retain their original values internally.

Styling inherits terminal foreground/background and monospaced font. Inverse selection and `>` indicate focus; labels, not color alone, communicate status. `--no-color` or a present `NO_COLOR` environment variable disables the cyan accent. No mouse, clipboard, graphical session, ncurses installation or online service is needed.

`econ_tui` is a separate reader/interface library linked by the CLI. It indexes byte offsets and record counts **by day** for replay, metrics, decisions and events; it keeps current snapshots and one day's parsed records, not the whole event history. Opening/indexing large runs and on-demand full-history ID lookup/verification may take time. It inspects completed runs and can launch a separate new run. Launching runs synchronously and displays a progress message. It cannot edit, advance, branch, or repair an existing run. See [employment and ledger views](employment.md).

## Interface direction and quality bar

The mode is Operate: choose a recorded experiment, move to a time boundary, find an actor or decision, and inspect the actual evidence. The interface extends the existing CLI vocabulary with terminal-native tabs, a persistent quit/help hint, a list/detail split on wide terminals, and a stacked preview on narrower ones. Numeric values remain exact cents/units. Overview, Help and full records wrap and scroll; long preview rows use explicit ellipses and Enter opens the complete record.

Required outcomes: keyboard access to every view and record; no skipped lines during paging; correct Home/End for wrapped records and Help; readable content at the supported minimum; an explicit empty/error state; no future-knowledge substitution; terminal restoration after normal or signaled exit; and no change to run artifacts. Real pseudo-terminal captures at 60×18, 80×24 and 120×36 support layout review; they do not establish contrast for every user's terminal palette.

## Verification

The `tui_acceptance` C++/CTest entry generates real small runs and tests indexed lookup, initial/checkpoint/display/final states, continuation boundaries, filters, actor following, full-record explanations, fresh verification, malformed/missing artifacts, optional-report errors, wrapping/paging, resize, quit/signal restoration, non-TTY rejection and byte-identical run artifacts after inspection. Its interactive checks use Linux pseudo-terminals (`pty.h`/libutil, supplied by the system libc development environment). The standard test command includes it:

```sh
./scripts/dev.sh test
ctest --test-dir build/debug -R tui_acceptance --output-on-failure
```

Executed for this increment: Debug **6/6** CTest entries passed; Release **6/6** passed; the focused ASan/UBSan **tui_acceptance** entry passed. The helper was also exercised from outside the repository with run paths containing spaces, the browser entry, `NO_COLOR`, terminal restoration and unchanged real baseline artifacts. Raw logs: [Debug](measurements/tui-debug-tests.txt), [Release](measurements/tui-release-tests.txt), [sanitizer TUI check](measurements/tui-sanitizer-tests.txt), and [build identity](measurements/tui-version.json).

The character-grid review at 60×18, 80×24 and 120×36 scored all five reported fixes resolved: optional-report recovery, End navigation, paging, minimum-width content access and persistent documentation. That verdict covers those fixes and captured terminal layouts; it does not claim palette contrast for every terminal theme.
