# Koi Architecture Stage 2 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Extract search-session lifecycle, fixed-capacity per-ply state, worker search context, and deterministic root coordination behind the existing `SearchService` and `SearchHandle` contracts without changing search formulas or externally observable behavior.

**Architecture:** `SearchSession` becomes the sole owner of one request's immutable root/limits/options snapshot, cancellation state, worker thread, and exactly-once completion claim. `SearchStack` owns fixed-capacity per-ply frames and principal-variation storage; `SearchContext` owns one stack plus one worker's mutable search state. `RootCoordinator` owns root-line records, deterministic ranking, and the root iteration coordination seams while the existing root-parallel policy is migrated incrementally.

**Tech Stack:** Windows x64 MSVC C++26, CMake 3.31 named modules, CTest, existing Koi native rules/evaluator/transposition-table interfaces, and the repository's custom executable test style.

**Spec:** `docs/superpowers/specs/2026-09-11-koi-architecture-rework-design.md`, especially “Stage 2 — Search session and stack decomposition” and the verification contract.

## Global Constraints

- Preserve the public `SearchService`, `SearchHandle`, `SearchLimits`, `SearchOptions`, `SearchInfo`, and `SearchResult` contracts.
- Preserve the native `Position` as the authoritative rules state and keep mirror checks at the existing explicit search boundary.
- Preserve current search formulas, root fallback behavior, UCI output ownership, cancellation semantics, and `Threads=1` deterministic node/PV/score behavior.
- Do not allocate on the normal recursive search path; fixed-capacity stack and PV storage must be contiguous and bounded.
- Do not expose `chess.hpp`, `SearchContext`, worker implementation, TT entries, or NNUE representation through public headers or modules.
- Do not delete `Goal.txt`; Stage 2 is not completion of the complete architecture goal.
- Use the Visual Studio developer environment for builds because ordinary PowerShell does not provide the required MSVC SDK variables.
- Every production change follows RED → GREEN → refactor, with the focused test failure observed before implementation.

---

### Task 1: Establish Stage 2 structural regression tests

**Files:**
- Create: `tests/unit/search/search_architecture_tests.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: the private headers introduced by Tasks 2–4: `koi/detail/search_stack.hpp`, `koi/detail/root_coordinator.hpp`, and `koi/detail/search_session.hpp`.
- Produces: executable `search_architecture_tests`, which proves fixed-capacity stack behavior, deterministic root ranking, immutable session snapshots, cancellation observation, and exactly-once completion publication.

- [ ] **Step 1: Add the test target declaration and test source with the intended API.**

The test must contain these real assertions:

```cpp
static void test_stack_is_fixed_capacity_and_restores_frames() {
    koi::detail::SearchStack stack;
    require(stack.capacity() == koi::detail::SearchStack::kCapacity,
            "search stack capacity must be explicit");
    require(stack.capacity() >= 64, "search stack must cover the configured search depth");
    auto& frame = stack.frame(7);
    frame.current_move = require_move("e2e4");
    frame.move_count = 3;
    stack.reset();
    require(stack.frame(7).current_move.is_no_move() && stack.frame(7).move_count == 0,
            "reset must clear transient per-ply state");
}

static void test_root_ranking_is_score_then_stable_index() {
    std::vector<koi::detail::RootLine> lines(3);
    lines[0].completed = true;
    lines[0].score = 20;
    lines[0].stable_index = 2;
    lines[1].completed = true;
    lines[1].score = 20;
    lines[1].stable_index = 0;
    lines[2].completed = true;
    lines[2].score = 30;
    lines[2].stable_index = 1;
    const auto ranked = koi::detail::RootCoordinator::rank(lines);
    require(ranked == std::vector<std::size_t>{2, 1, 0},
            "root ranking must be deterministic for equal scores");
}

static void test_session_snapshot_and_completion_are_single_owner_operations() {
    koi::GameState root = koi::GameState::startpos();
    koi::SearchLimits limits;
    limits.depth = 2;
    koi::SearchOptions options;
    options.generation = 41;
    auto session = std::make_shared<koi::detail::SearchSession>(
        std::move(root), limits, options);
    require(session->generation() == 41, "session must own the request generation");
    require(session->root().position_key() != 0, "session must own the root snapshot");
    int completions = 0;
    koi::SearchEventSink sink;
    sink.on_complete = [&completions](const koi::SearchResult&) { ++completions; };
    koi::SearchResult result;
    session->publish_completion(sink, result);
    session->publish_completion(sink, result);
    require(completions == 1, "session must publish completion exactly once");
}
```

The file must use the repository's `require`/`TestCase` pattern and must not mock the session, stack, or coordinator.

- [ ] **Step 2: Add `search_architecture_tests` to `koi_core` linkage and CTest.**

```cmake
add_executable(search_architecture_tests
    tests/unit/search/search_architecture_tests.cpp
)
target_link_libraries(search_architecture_tests PRIVATE koi_core)
add_test(NAME search_architecture_tests COMMAND search_architecture_tests)
```

- [ ] **Step 3: Build and run the new target before creating production headers.**

Run from a VS x64 developer prompt:

```powershell
cmake --build build/release --config Release --target search_architecture_tests
ctest --test-dir build/release -C Release -R '^search_architecture_tests$' --output-on-failure
```

Expected: compilation fails because the three new private interfaces do not exist. If the test passes or fails for a syntax/setup error, correct the test until the failure is specifically missing production behavior.

- [ ] **Step 4: Commit the RED test and CMake target.**

```powershell
git add CMakeLists.txt tests/unit/search/search_architecture_tests.cpp
git commit -m "test: specify stage two search ownership boundaries"
```

### Task 2: Implement the fixed-capacity `SearchStack`

**Files:**
- Create: `src/koi/detail/search_stack.hpp`
- Modify: `CMakeLists.txt` only if a translation unit is needed; the first implementation is header-only to keep frame access inline.
- Modify: `src/koi/search_service.cpp` to replace the local `PrincipalVariation` with `detail::PrincipalVariation` and give each `SearchContext` one `SearchStack`.
- Test: `tests/unit/search/search_architecture_tests.cpp`

**Interfaces:**
- Consumes: `koi::Move` and `koi::kMaximumLegalMoves` from existing private search code.
- Produces: `koi::detail::SearchFrame`, `koi::detail::PrincipalVariation`, and `koi::detail::SearchStack`.

Required interface:

```cpp
namespace koi::detail {

inline constexpr std::size_t kSearchStackCapacity = 64;

struct SearchFrame {
    Move current_move = Move::no_move();
    Move previous_move = Move::no_move();
    int static_eval = 0;
    int move_count = 0;
    int reduction = 0;
    int extension = 0;
    bool in_check = false;
};

struct PrincipalVariation {
    std::array<Move, kSearchStackCapacity> moves{};
    std::uint8_t length = 0;
    void clear() noexcept;
    void prepend(Move move, const PrincipalVariation& child) noexcept;
    [[nodiscard]] std::vector<Move> to_vector() const;
};

class SearchStack {
public:
    static constexpr std::size_t kCapacity = kSearchStackCapacity;
    [[nodiscard]] constexpr std::size_t capacity() const noexcept;
    [[nodiscard]] SearchFrame& frame(std::size_t ply) noexcept;
    [[nodiscard]] const SearchFrame& frame(std::size_t ply) const noexcept;
    void reset() noexcept;
private:
    std::array<SearchFrame, kCapacity> frames_{};
};

}
```

The public shape must remain private to `src/koi/detail`; `PrincipalVariation::to_vector` is the only heap-producing boundary and is used only for published results, never inside recursive search.

- [ ] **Step 1: Add the minimal stack/PV implementation.**
- [ ] **Step 2: Run `search_architecture_tests` and confirm the stack tests pass.**
- [ ] **Step 3: Add `SearchStack stack` to `SearchContext`, call `stack.reset()` in `begin_iteration`, and record the current frame's check/static/reduction/extension/move-count values during `negamax`.**
- [ ] **Step 4: Run the focused search tests and fixed-depth deterministic checks.**

```powershell
cmake --build build/release --config Release --target search_architecture_tests koi_search_tests search_ordering_tests
ctest --test-dir build/release -C Release -R 'search_architecture_tests|koi_search_tests|search_ordering_tests' --output-on-failure
```

- [ ] **Step 5: Commit the stack extraction.**

```powershell
git add src/koi/detail/search_stack.hpp src/koi/search_service.cpp tests/unit/search/search_architecture_tests.cpp
git commit -m "refactor: add fixed-capacity search stack"
```

### Task 3: Extract deterministic root-line ownership into `RootCoordinator`

**Files:**
- Create: `src/koi/detail/root_coordinator.hpp`
- Create: `src/koi/detail/root_coordinator.cpp`
- Modify: `CMakeLists.txt`
- Modify: `src/koi/search_service.cpp`
- Test: `tests/unit/search/search_architecture_tests.cpp`

**Interfaces:**
- Consumes: `detail::PrincipalVariation` and root-line completion data.
- Produces: `detail::RootLine`, `detail::RootCoordinator::rank`, and a stable best-line selector used by both serial and root-parallel paths.

Required interface:

```cpp
namespace koi::detail {

struct RootLine {
    bool completed = false;
    int score = -1'000'000;
    std::size_t stable_index = 0;
    PrincipalVariation pv;
};

class RootCoordinator {
public:
    [[nodiscard]] static std::vector<std::size_t>
    rank(const std::vector<RootLine>& lines);
    [[nodiscard]] static std::optional<std::size_t>
    best_completed(const std::vector<RootLine>& lines);
};

}
```

`rank` must exclude incomplete lines, sort descending by score, and break ties by `stable_index`. No worker synchronization or UCI output belongs in this class. The existing `RootWorkerPool` remains the first consumer; later stages may move scheduling behind the same boundary.

- [ ] **Step 1: Add the failing tie/incomplete-line tests to the architecture test executable and run RED.**
- [ ] **Step 2: Implement the coordinator with stable, allocation-bounded ranking proportional only to root-line count.**
- [ ] **Step 3: Replace `RootLine` and `rank_root_lines` in `search_service.cpp` with the private coordinator types.**
- [ ] **Step 4: Run root-parallel and serial fixed-depth tests, verifying identical best move, PV, score, and node count for `Threads=1`.**
- [ ] **Step 5: Commit the coordinator extraction.**

```powershell
git add CMakeLists.txt src/koi/detail/root_coordinator.hpp src/koi/detail/root_coordinator.cpp src/koi/search_service.cpp tests/unit/search/search_architecture_tests.cpp
git commit -m "refactor: isolate deterministic root coordination"
```

### Task 4: Make `SearchSession` the lifecycle owner

**Files:**
- Create: `src/koi/detail/search_session.hpp`
- Create: `src/koi/detail/search_session.cpp`
- Modify: `CMakeLists.txt`
- Modify: `src/koi/search_service.hpp`
- Modify: `src/koi/search_service.cpp`
- Test: `tests/unit/search/search_architecture_tests.cpp`

**Interfaces:**
- Consumes: `GameState`, `SearchLimits`, `SearchOptions`, `SearchRequestIdentity`, `SearchResult`, `SearchEventSink`, and `CompletionOnce`.
- Produces: a non-copyable private session with immutable request snapshots, cancellation, thread join, running state, identity, and exactly-once completion publication.

Required interface:

```cpp
namespace koi::detail {

class SearchSession {
public:
    SearchSession(GameState root, SearchLimits limits, SearchOptions options);
    SearchSession(const SearchSession&) = delete;
    SearchSession& operator=(const SearchSession&) = delete;
    ~SearchSession();

    [[nodiscard]] const GameState& root() const noexcept;
    [[nodiscard]] const SearchLimits& limits() const noexcept;
    [[nodiscard]] const SearchOptions& options() const noexcept;
    [[nodiscard]] const SearchRequestIdentity& identity() const noexcept;
    [[nodiscard]] std::uint64_t generation() const noexcept;
    [[nodiscard]] std::atomic_bool& stop_requested() noexcept;
    void launch(std::function<void()> work);
    void stop() noexcept;
    void wait();
    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] bool publish_completion(const SearchEventSink&, const SearchResult&) noexcept;

private:
    GameState root_;
    SearchLimits limits_;
    SearchOptions options_;
    SearchRequestIdentity identity_;
    std::atomic_bool stop_requested_ = false;
    std::atomic_bool running_ = false;
    CompletionOnce completion_once_;
    std::mutex stop_mutex_;
    std::condition_variable stop_condition_;
    std::thread worker_;
};

}
```

`publish_completion` claims `CompletionOnce` before calling the existing safe sink helper. It must set `running_` false exactly once in the worker wrapper even when the work function throws. The session does not format UCI or validate chess results; it only owns request lifecycle and publication identity.

- [ ] **Step 1: Add tests for request snapshot identity, stop visibility, and duplicate completion suppression; run RED.**
- [ ] **Step 2: Implement the session constructor, launch wrapper, stop/wait, and exactly-once completion claim.**
- [ ] **Step 3: Change `SearchHandle` to hold `std::shared_ptr<detail::SearchSession>` and delegate `stop`, `wait`, and `running`.**
- [ ] **Step 4: Move the current `SearchHandle::State` fields into `SearchSession` without changing the search lambda's formulas.**
- [ ] **Step 5: Make `SearchService::start` construct the session from the normalized snapshots and capture only the session plus shared immutable services.**
- [ ] **Step 6: Run completion-gate, UCI controller, cancellation, and search tests.**

```powershell
cmake --build build/release --config Release --target search_architecture_tests completion_gate_tests uci_controller_tests koi_search_tests
ctest --test-dir build/release -C Release -R 'search_architecture_tests|completion_gate_tests|uci_controller_tests|koi_search_tests' --output-on-failure
```

- [ ] **Step 7: Commit the session lifecycle extraction.**

```powershell
git add CMakeLists.txt src/koi/detail/search_session.hpp src/koi/detail/search_session.cpp src/koi/search_service.hpp src/koi/search_service.cpp tests/unit/search/search_architecture_tests.cpp
git commit -m "refactor: give search sessions explicit lifecycle ownership"
```

### Task 5: Extract `SearchContext` behind a private translation-unit boundary

**Files:**
- Create: `src/koi/detail/search_context.hpp`
- Create: `src/koi/detail/search_context.cpp`
- Create or modify: `src/koi/detail/search_context_support.hpp` if helper declarations must be shared with the root coordinator
- Modify: `CMakeLists.txt`
- Modify: `src/koi/search_service.cpp`
- Modify: `src/koi/detail/search_stack.hpp`
- Test: `tests/unit/search/search_architecture_tests.cpp`

**Interfaces:**
- Consumes: `Evaluator`, `TranspositionTable`, `TimeManager`, cancellation/node counters, `MoveMetadataList`, `SearchOptions::QuietHistorySideHook`, `SearchStack`, and the existing native `GameState` operations.
- Produces: private `detail::SearchContext` with `begin_iteration`, `interrupted`, `request_abort`, `best_completed_root_move`, `selectively_research_root_move`, `negamax`, and its existing `stats`, `ordering`, root-score, and cache accessors.

Move the existing struct and method bodies without changing formulas. The context translation unit may expose only the declarations needed by `RootWorkerPool` and `SearchService::start`; the support header may declare the existing search constants and helper predicates (`piece_value`, `null_move_is_safe`, `quiet_move_is_forcing`, `narrow_deep_quiet_check_candidate`, and `root_move_exposes_immediate_check`) while preserving their current definitions and behavior. Do not move short-search fallback policy into this task.

- [ ] **Step 1: Add a compile-only architecture assertion that the context owns a `SearchStack` and no public module/header includes its definition.**
- [ ] **Step 2: Move the struct declaration and method bodies to `search_context.hpp/.cpp`; keep private support declarations in `detail` and remove the old definition from `search_service.cpp`.**
- [ ] **Step 3: Update `RootWorkerPool` and serial start code to use `detail::SearchContext` and `detail::PrincipalVariation`.**
- [ ] **Step 4: Run the focused rules/search tests and verify the test target fails if the old monolithic definition is accidentally retained.**
- [ ] **Step 5: Run fixed-depth serial/paralleled benchmark parity and compare node count, qnodes, score, PV, and best move position-by-position.**
- [ ] **Step 6: Commit the context extraction only after the parity comparison has zero unexplained differences.**

```powershell
git add CMakeLists.txt src/koi/detail/search_context.hpp src/koi/detail/search_context.cpp src/koi/detail/search_context_support.hpp src/koi/detail/search_stack.hpp src/koi/search_service.cpp tests/unit/search/search_architecture_tests.cpp
git commit -m "refactor: isolate worker search context"
```

### Task 6: Document and verify the complete Stage 2 boundary

**Files:**
- Modify: `README.md`
- Modify: `docs/superpowers/specs/2026-09-11-koi-architecture-rework-design.md`
- Create: `docs/superpowers/verification/2026-09-11-koi-architecture-stage2.md`
- Test: all applicable Release/Debug CTest and process suites

**Interfaces:**
- Consumes: the implementation and test evidence from Tasks 1–5.
- Produces: an accurate architecture description and a reproducible Stage 2 verification record; it does not claim the overall architecture is complete.

- [ ] **Step 1: Update README architecture and ownership sections with the actual private search files and lifecycle flow.**
- [ ] **Step 2: Update the architecture spec’s Stage 2 note with the exact extracted types and the remaining root-worker migration boundary.**
- [ ] **Step 3: Run `git diff --check` and the public-boundary scan.**
- [ ] **Step 4: Run the focused Release gate, full Release CTest, and available Debug gate; preserve pre-existing timed failures verbatim rather than weakening tests.**
- [ ] **Step 5: Run deterministic 64-position cold/warm profiles and record before/after differences.**
- [ ] **Step 6: Write the verification record with commands, counts, parity results, performance observations, limitations, and the explicit statement that `Goal.txt` remains because later stages are outstanding.**
- [ ] **Step 7: Commit documentation and verification.**

```powershell
git add README.md docs/superpowers/specs/2026-09-11-koi-architecture-rework-design.md docs/superpowers/verification/2026-09-11-koi-architecture-stage2.md
git commit -m "docs: record stage two search architecture boundary"
```

## Plan self-review

- Spec coverage: Stage 2’s four named concepts, deterministic `Threads=1`, cancellation, exactly-once completion, root coordination, documentation, and benchmark/process gates are each assigned to a task.
- Completeness scan: every task names concrete files, interfaces, commands, and expected evidence; no step is left unspecified.
- Type consistency: `PrincipalVariation` and `SearchStack` are produced by Task 2; `RootLine` consumes them in Task 3; `SearchSession` owns the request types already defined in `search_types.hpp`; `SearchContext` consumes both stack and existing search services in Task 5.
- Scope: this plan deliberately leaves new heuristics, TT storage redesign, NNUE evolution, Lazy SMP, and UCI protocol changes to later architecture stages.
- Safety: no task deletes `Goal.txt`, weakens the existing timed test, or changes the public chess/rules boundary.
