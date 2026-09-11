# Koi Architecture Stage 3 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Extract adaptive move-ordering tables and pure pruning/reduction/extension decisions behind private seams without changing Koi search behavior.

**Architecture:** `SearchOrderingTables` owns per-context killer/history/counter/continuation state while `SearchMoveOrdering` remains the ranking façade. `SearchPolicy` owns only scalar decision formulas and returns explicit decision records; `SearchContext` applies those records to the existing recursion and statistics.

**Tech Stack:** Windows x64 MSVC C++26, CMake 3.31 named modules, CTest, the existing Koi search/rules interfaces, and the fixed-depth benchmark profile tooling.

**Spec:** `docs/superpowers/specs/2026-09-11-koi-architecture-stage3-design.md` and Stage 3 of `docs/superpowers/specs/2026-09-11-koi-architecture-rework-design.md`.

## Global Constraints

- Preserve all public Koi and UCI contracts.
- Preserve current ordering priorities, history update equations, pruning/reduction/extension formulas, diagnostics, and fallback policy.
- Keep `SearchOrderingTables` and `SearchPolicy` private to `src/koi/detail`.
- Do not allocate, lock, or dispatch virtually in the normal recursive policy path.
- Follow RED -> GREEN -> refactor and record the focused failure before production implementation.
- Keep `Goal.txt`; Stage 3 is not completion of the complete architecture objective.

---

### Task 1: Add real RED tests for ordering-table and policy seams

**Files:**
- Create: `tests/unit/search/search_policy_tests.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: the intended private `koi::detail::SearchOrderingTables` and `koi::detail::SearchPolicy` interfaces below.
- Produces: a focused executable that fails for missing production seams, not for test setup.

Required test-facing API:

```cpp
namespace koi::detail {

class SearchOrderingTables {
public:
    void clear() noexcept;
    [[nodiscard]] bool is_killer(Move move, int ply) const noexcept;
    [[nodiscard]] int quiet_history_score(
        Color side, Move move, std::optional<Move> previous_move = std::nullopt) const noexcept;
    [[nodiscard]] bool is_proven_counter_move(
        Color side, Move previous_move, Move move) const noexcept;
    void record_quiet_cutoff(Color side, Move move, int ply, int depth,
                             std::optional<Move> previous_move = std::nullopt) noexcept;
    void record_quiet_fail(Color side, Move move, int ply, int depth,
                           std::optional<Move> previous_move = std::nullopt) noexcept;
};

struct NullMoveDecision { bool eligible; int reduction; };
struct LateMoveDecision {
    bool candidate;
    bool high_history_exclusion;
    int reduction;
    bool reduced;
};
enum class QuiescenceCapturePrune { none, static_exchange, delta };

class SearchPolicy {
public:
    [[nodiscard]] static NullMoveDecision null_move(
        int depth, int alpha, int beta, bool checked, bool allowed) noexcept;
    [[nodiscard]] static bool check_extension(
        bool checked, int depth, bool short_timed_root,
        int extensions_remaining) noexcept;
    [[nodiscard]] static LateMoveDecision late_move(
        int depth, int move_number, int full_child_depth, int history_score,
        bool root_pawn_move, bool checked, bool gives_check, bool capture,
        bool promotion, bool tt_move, bool killer, bool reducible_quiet,
        bool quiet_forcing) noexcept;
    [[nodiscard]] static bool quiet_futility(
        bool phase_rich_quiet_position, bool capture, bool gives_check,
        bool promotion, int move_number, int static_eval, int depth,
        int alpha) noexcept;
    [[nodiscard]] static QuiescenceCapturePrune quiescence_capture(
        bool checked, bool capture, bool gives_check, bool promotion,
        int see_score, int captured_piece_value, int best, int alpha) noexcept;
};

}
```

- [ ] **Step 1: Write tests for policy boundaries and table reset/lookup.**

The tests must assert: null move rejects checked/wide/depth-too-shallow nodes
and uses reduction 2/3 at depths 5/6; check extension rejects short timed or
exhausted paths; LMR excludes captures/checks/killer/high-history moves and
computes the existing depth/history reduction; futility and qsearch classify
their exact boundary cases; table cutoff raises a killer/history/counter and
`clear()` removes it.

- [ ] **Step 2: Add `search_policy_tests` to CMake and CTest.**
- [ ] **Step 3: Build the target before adding production headers.**

```powershell
cmake --build build/release --config Release --target search_policy_tests
ctest --test-dir build/release -C Release -R '^search_policy_tests$' --output-on-failure
```

Expected: compilation fails because the two private interfaces do not exist.

- [ ] **Step 4: Commit the RED tests.**

```powershell
git add CMakeLists.txt tests/unit/search/search_policy_tests.cpp
git commit -m "test: specify ordering and search policy seams"
```

### Task 2: Extract adaptive ordering-table ownership

**Files:**
- Create: `src/koi/detail/search_ordering_tables.hpp`
- Create: `src/koi/detail/search_ordering_tables.cpp`
- Modify: `src/koi/detail/search_ordering.hpp`
- Modify: `src/koi/search_ordering.cpp`
- Modify: `CMakeLists.txt`
- Test: `tests/unit/search/search_policy_tests.cpp` and existing `search_ordering_tests`

**Interfaces:**
- Consumes: the current fixed-size killer/history/counter/continuation arrays and update equations.
- Produces: one `SearchOrderingTables` member owned by `SearchMoveOrdering`; no external consumer sees its storage.

- [ ] **Step 1: Move the adaptive arrays and their update helpers into `SearchOrderingTables`.**
- [ ] **Step 2: Delegate `SearchMoveOrdering::clear`, killer/history accessors, and record methods to the table owner.**
- [ ] **Step 3: Replace direct array access in `priority` with table-owner queries while retaining all numeric priorities.**
- [ ] **Step 4: Run `search_policy_tests` and `search_ordering_tests`; correct implementation until both pass.**

```powershell
cmake --build build/release --config Release --target search_policy_tests search_ordering_tests
ctest --test-dir build/release -C Release -R 'search_policy_tests|search_ordering_tests' --output-on-failure
```

- [ ] **Step 5: Commit the table extraction.**

```powershell
git add CMakeLists.txt src/koi/detail/search_ordering_tables.hpp src/koi/detail/search_ordering_tables.cpp src/koi/detail/search_ordering.hpp src/koi/search_ordering.cpp tests/unit/search/search_policy_tests.cpp
git commit -m "refactor: isolate search ordering tables"
```

### Task 3: Add and integrate pure search-policy decisions

**Files:**
- Create: `src/koi/detail/search_policy.hpp`
- Modify: `src/koi/detail/search_context.hpp`
- Modify: `src/koi/detail/search_constants.hpp`
- Test: `tests/unit/search/search_policy_tests.cpp`, `search_architecture_tests`, `koi_search_tests`

**Interfaces:**
- Consumes: the current scalar conditions in `quiescence` and `negamax`.
- Produces: pure `SearchPolicy` decision records used by the context; no policy function calls `GameState` or mutates statistics.

- [ ] **Step 1: Implement the tested policy functions inline using the current constants and exact formulas.**
- [ ] **Step 2: Replace inline null-move, check-extension, LMR, futility, and qsearch capture gates in `SearchContext` with policy decisions.**
- [ ] **Step 3: Preserve the existing statistics increments at the context call sites.**
- [ ] **Step 4: Run the focused policy, architecture, search, ordering, perft, and strength gates.**

```powershell
cmake --build build/release --config Release --target search_policy_tests search_architecture_tests koi_search_tests search_ordering_tests perft_tests koi_strength_tests
ctest --test-dir build/release -C Release -R 'search_policy_tests|search_architecture_tests|koi_search_tests|search_ordering_tests|perft_tests|koi_strength_tests' --output-on-failure
```

The known short-clock oracle sensitivity must be reported verbatim if it
reappears; fixed-depth and deterministic profile changes require investigation.

- [ ] **Step 5: Commit the policy seam.**

```powershell
git add src/koi/detail/search_policy.hpp src/koi/detail/search_context.hpp src/koi/detail/search_constants.hpp tests/unit/search/search_policy_tests.cpp
git commit -m "refactor: isolate search policy decisions"
```

### Task 4: Verify and document Stage 3

**Files:**
- Modify: `README.md`
- Modify: `docs/superpowers/specs/2026-09-11-koi-architecture-rework-design.md`
- Modify: `docs/superpowers/specs/2026-09-11-koi-architecture-stage3-design.md` only if implementation evidence requires a correction
- Create: `docs/superpowers/verification/2026-09-11-koi-architecture-stage3.md`

- [ ] **Step 1: Run focused Release gates and `git diff --check`/public-boundary scan.**
- [ ] **Step 2: Run fresh cold/warm 64-position profiles and compare every PV, score, best move, node, and qnode row with Stage 2.**
- [ ] **Step 3: Run full Release CTest and preserve timed limitations exactly.**
- [ ] **Step 4: Update the architecture documentation with the actual ownership and explicit remaining Stage 4–6 work.**
- [ ] **Step 5: Write the verification record and commit documentation.**

```powershell
git add README.md docs/superpowers/specs/2026-09-11-koi-architecture-rework-design.md docs/superpowers/verification/2026-09-11-koi-architecture-stage3.md
git commit -m "docs: record stage three policy architecture boundary"
```
