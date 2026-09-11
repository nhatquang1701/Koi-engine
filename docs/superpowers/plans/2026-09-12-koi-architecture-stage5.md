# Koi Architecture Stage 5 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Isolate search-facing TT access and serial/shared node-budget accounting while retaining current time, cancellation, and root-parallel behavior.

**Architecture:** `SearchTableAccess` gates one context's TT probes/stores and delegates physical storage to `TranspositionTable`. `SearchBudget` snapshots the node limit and either validates local counts or reserves through a shared atomic counter; `SearchContext` uses both private seams while `RootWorkerPool` keeps its existing scheduling.

**Tech Stack:** Windows x64 MSVC C++26, CMake 3.31 named modules, CTest, existing striped TT/time-manager implementation, and benchmark/process tooling.

**Spec:** `docs/superpowers/specs/2026-09-12-koi-architecture-stage5-design.md` and Stage 5 of `docs/superpowers/specs/2026-09-11-koi-architecture-rework-design.md`.

## Global Constraints

- Preserve public TT/time/search/UCI contracts and all existing formulas.
- Do not modify physical TT storage, replacement, generation, clear, or mate normalization in this stage.
- Do not add Lazy SMP, new worker scheduling, shared histories, or nondeterministic result selection.
- Keep `SearchTableAccess` and `SearchBudget` private to `src/koi/detail`.
- Use RED -> GREEN -> refactor; preserve `Goal.txt`.

---

### Task 1: Add RED runtime-resource seam tests

**Files:**
- Create: `tests/unit/search/search_runtime_tests.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: intended `koi::detail::SearchTableAccess` and `koi::detail::SearchBudget` APIs.
- Produces: focused tests for TT gating and local/shared node accounting.

Required API:

```cpp
namespace koi::detail {
class SearchTableAccess {
public:
    SearchTableAccess(TranspositionTable&, bool enabled = true);
    void set_enabled(bool) noexcept;
    [[nodiscard]] bool enabled() const noexcept;
    [[nodiscard]] std::optional<TranspositionEntry> probe(std::uint64_t key, int ply = 0) const noexcept;
    void store(std::uint64_t key, int depth, int score, TranspositionBound,
               Move best_move, int ply = 0) noexcept;
};

class SearchBudget {
public:
    SearchBudget(const TimeManager&, std::atomic<std::uint64_t>* shared_nodes = nullptr);
    [[nodiscard]] bool reserve(std::uint64_t local_nodes) noexcept;
    [[nodiscard]] std::uint64_t visited(std::uint64_t local_nodes) const noexcept;
    [[nodiscard]] bool uses_shared_counter() const noexcept;
};
}
```

- [x] **Step 1: Write tests that store/probe through enabled access, reject disabled access, exhaust a local limit, and cap a shared atomic counter at the node limit.**
- [x] **Step 2: Add `search_runtime_tests` to CMake/CTest.**
- [x] **Step 3: Build before adding production headers.**

```powershell
cmake --build build/release --config Release --target search_runtime_tests
ctest --test-dir build/release -C Release -R '^search_runtime_tests$' --output-on-failure
```

Expected: compilation fails specifically because the two private types do not exist.

- [x] **Step 4: Commit the RED test.**

```powershell
git add CMakeLists.txt tests/unit/search/search_runtime_tests.cpp
git commit -m "test: specify search runtime resource seams"
```

### Task 2: Implement the TT access and node-budget owners

**Files:**
- Create: `src/koi/detail/search_table_access.hpp`
- Create: `src/koi/detail/search_budget.hpp`
- Modify: `CMakeLists.txt` only for the new test target; both seams are inline to keep hot access predictable.

**Interfaces:**
- Consumes: public `TranspositionTable`, `TimeManager`, and existing score/move contracts.
- Produces: private no-allocation access and budget objects with the exact APIs above.

- [x] **Step 1: Implement `SearchTableAccess` as a disabled no-op or direct TT delegate.**
- [x] **Step 2: Implement local reservation and shared CAS reservation in `SearchBudget`; snapshot `TimeManager::node_limit()` in the constructor.**
- [x] **Step 3: Run `search_runtime_tests` and the existing TT/time-manager tests.**

```powershell
cmake --build build/release --config Release --target search_runtime_tests koi_search_tests
ctest --test-dir build/release -C Release -R 'search_runtime_tests|koi_search_tests' --output-on-failure
```

- [x] **Step 4: Commit the resource seams.**

```powershell
git add src/koi/detail/search_table_access.hpp src/koi/detail/search_budget.hpp
git commit -m "refactor: isolate search runtime resource contracts"
```

### Task 3: Integrate resource seams into `SearchContext`

**Files:**
- Modify: `src/koi/detail/search_context.hpp`
- Test: `search_runtime_tests`, `search_architecture_tests`, search/rules/parallel gates

- [x] **Step 1: Replace direct context TT probes/stores and the mutable TT-enabled flag with `SearchTableAccess`.**
- [x] **Step 2: Replace direct global-node pointer reservation/observation with `SearchBudget`, preserving constructor compatibility for root workers.**
- [x] **Step 3: Keep `TimeManager::should_stop` and `SearchSession` cancellation at their current owners.**
- [x] **Step 4: Run focused serial, node-limit, and root-parallel tests.**

```powershell
cmake --build build/release --config Release --target search_runtime_tests search_architecture_tests search_policy_tests koi_search_tests search_ordering_tests perft_tests koi_strength_tests
ctest --test-dir build/release -C Release -R 'search_runtime_tests|search_architecture_tests|search_policy_tests|koi_search_tests|search_ordering_tests|perft_tests|koi_strength_tests' --output-on-failure
```

- [x] **Step 5: Commit the integration.**

```powershell
git add src/koi/detail/search_context.hpp
git commit -m "refactor: route search through runtime resource seams"
```

### Task 4: Verify and document Stage 5

**Files:**
- Modify: `README.md`
- Modify: `docs/superpowers/specs/2026-09-11-koi-architecture-rework-design.md`
- Create: `docs/superpowers/verification/2026-09-12-koi-architecture-stage5.md`

- [x] **Step 1: Run fresh focused and full Release gates, including serial/root-parallel process tests.**
- [x] **Step 2: Run public-boundary/diff checks and cold/warm profiles against Stage 4.**
- [x] **Step 3: Record TT access/budget ownership and explicitly state that Lazy SMP remains deferred.**
- [x] **Step 4: Commit documentation and verification.**

```powershell
git add README.md docs/superpowers/specs/2026-09-11-koi-architecture-rework-design.md docs/superpowers/verification/2026-09-12-koi-architecture-stage5.md
git commit -m "docs: record stage five runtime resource boundary"
```
