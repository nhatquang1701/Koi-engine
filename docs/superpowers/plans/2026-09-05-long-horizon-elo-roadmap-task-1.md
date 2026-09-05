# Koi Engine Task 1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Complete and verify the existing Task 1 implementation for UCI compatibility controls and hidden diagnostics without regressing Lucas/UCI behavior.

**Architecture:** Preserve the current controller-owned option state and snapshot it into `SearchOptions` at search start. Keep timing policy in `TimeManager`, WDL formatting and hidden diagnostic routing in the controller, and use the existing tests/process harness as the behavioral boundary.

**Tech Stack:** C++26, CMake, Visual Studio x64 Debug/Release, CTest, PowerShell process tests, and the vendored chess library.

**Spec:** `.superpowers/sdd/2026-09-05-long-horizon-elo-roadmap/task-1-brief.md`

## Global Constraints

- Work directly in `C:\Users\ntATh\AI test\Koi engine`.
- Preserve Lucas/UCI, book, deterministic `Threads=1`, and clean stdout behavior.
- Use no new third-party dependency.
- Do not dispatch subagents; use the Terra execution path only.
- Public option output appends the five Task 1 options after existing options.
- Explicit depth, nodes, and infinite searches remain untimed; option changes stop and join active searches.

### Task 1: Audit existing RED tests and implementation

**Files:**
- Inspect and, only when needed, modify `src/koi/search_types.hpp`, `src/koi/time_manager.hpp`, `src/koi/time_manager.cpp`, `src/koi/search_service.cpp`, `src/koi/uci_controller.hpp`, and `src/koi/uci_controller.cpp`.
- Inspect and, only when needed, modify `tests/koi_search_tests.cpp`, `tests/uci_controller_tests.cpp`, and `tests/uci_process_test.ps1`.

**Interfaces:**
- Consumes the existing search service, controller, timing, and process-test interfaces.
- Produces the completed Task 1 behavior and evidence needed by the final report.

- [x] Record the current diff and identify every Task 1 requirement covered by existing tests.
- [x] Run the focused Debug tests before any production edit and capture the expected RED failures for newly added behavior.
- [x] Trace multi-word `setoption` parsing, option snapshotting, cancellation/join, timing order, WDL/mate conversion, and debug rotation/locking.
- [x] For each real defect, add or adjust a behavior test, run it RED, apply the smallest production fix, and run the focused tests GREEN.
- [x] Keep sound existing code unchanged and preserve unrelated worktree changes.

### Task 2: Fresh build and full verification

**Files:**
- Modify `.superpowers/sdd/2026-09-05-long-horizon-elo-roadmap/task-1-report.md` with commands, outputs, audit findings, and final verification evidence.

**Interfaces:**
- Consumes the verified source and test tree from Task 1.
- Produces Debug and Release configured/built with the Visual Studio x64 environment, full CTest results, self-review findings, and a reproducible commit.

- [x] Configure and build fresh Debug and Release trees with the Visual Studio x64 environment.
- [x] Run focused Debug tests and the full Debug and Release CTest suites, including process tests.
- [x] Run a self-review against every brief requirement and inspect the final diff for accidental changes.
- [x] Write the detailed report before committing.
- [x] Commit the implementation and report with a focused subject, then record the commit and verification output. Commit: `a10cd42` before final bookkeeping amend.
