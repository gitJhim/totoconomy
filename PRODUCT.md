# Product

<!-- impeccable:product-schema 1 -->

## Platform

Linux terminal. The user explicitly requested an interactive TUI for this existing C++ CLI; this is not a browser or mobile surface.

## Users and Purpose

The user builds and inspects miniature economic simulations. The foundation brief establishes an investigation workflow: inspect an outcome, retrieve actual decision evidence, restore a checkpoint, change one declared condition and compare recorded results.

## Stack and Operating Context

Existing C++20/CMake/CTest engine and CLI, with vendored JSON and offline Linux builds. The TUI consumes existing completed run directories. Its operating context is an ANSI-compatible terminal and keyboard, including 80-column terminals; it preserves the terminal's font and foreground/background palette.

## Capabilities and Constraints

Authoritative economic state and policy execution belong to the simulation core. Run artifacts include initial/final states, complete checkpoints, periodic display states, decisions, events, metrics and verification reports. Display snapshots contain less household knowledge than full states; unavailable knowledge must be disclosed rather than filled from later state. Earlier history for a continuation remains in its parent run.

The TUI inspects completed runs and can launch new simulations with fixed or dynamic employment. It must leave existing run files unchanged, preserve noninteractive JSON commands, and restore terminal settings on exit. No new graphical framework, service or network dependency is required.

## Product Principles

- Show recorded facts and their time boundaries.
- Expose why a policy acted using its recorded inputs and resulting events.
- Distinguish saved verification from checks performed now.
- Preserve reproducibility and read-only inspection.

## Evidence on Hand

The repository includes accounting and synthetic-town scenarios, executable acceptance tests, and recorded baseline/resume/branch runs. Economic results are synthetic model outcomes, not empirical predictions. The user has not requested a new visual brand or a web interface.
