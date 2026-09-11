# Koi Architecture Stage 4 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give evaluation execution and NNUE worker-local state an explicit private boundary while preserving classical and fallback behavior.

**Architecture:** `EvaluatorWorker` is an optional public capability with a null default. `NnueEvaluator` supplies a private adapter around one `NnueWorker`; `detail::EvaluationContext` owns that optional worker or the existing evaluator/mutex fallback. `SearchContext` calls only `EvaluationContext` for recursive evaluation.

**Tech Stack:** Windows x64 MSVC C++26, CMake 3.31 named modules, CTest, existing classical/NNUE evaluators, and Koi benchmark profiles.

**Spec:** `docs/superpowers/specs/2026-09-12-koi-architecture-stage4-design.md` and Stage 4 of `docs/superpowers/specs/2026-09-11-koi-architecture-rework-design.md`.

## Global Constraints

- Preserve `GameState`, `Position`, `SearchService`, UCI, evaluator fallback, network manifest, and inference result contracts.
- Keep `EvaluationContext` and the NNUE adapter private; public headers expose only the optional evaluator-worker capability.
- Do not change classical evaluation formulas or NNUE feature encoding/inference formulas.
- Preserve the existing mutex serialization for evaluators without private workers.
- Use RED -> GREEN -> refactor and retain `Goal.txt` until the entire architecture goal is complete.

---

### Task 1: Add RED tests for evaluator-worker ownership

**Files:**
- Create: `tests/unit/evaluation/evaluation_architecture_tests.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: intended `koi::EvaluatorWorker`, `Evaluator::create_worker`, and private `koi::detail::EvaluationContext` APIs.
- Produces: focused tests for worker creation/isolation, fallback evaluation, and search-context ownership.

Required assertions:

```cpp
class CountingWorker final : public koi::EvaluatorWorker {
public:
    explicit CountingWorker(int value) : value_(value) {}
    int evaluate(const koi::GameState&, koi::Color) override { return value_; }
private:
    int value_;
};
```

The test evaluator must return one worker per `create_worker()` call and a
different base value from `evaluate()`. Assert that two `EvaluationContext`
instances create independent workers, worker evaluation returns the worker
value, a null factory uses the base evaluator, and `SearchContext` exposes its
evaluation-context ownership boundary. Use real evaluator implementations in
the test; do not mock mutexes or search state.

- [ ] **Step 1: Write the focused test and CMake target.**
- [ ] **Step 2: Build before production implementation.**

```powershell
cmake --build build/release --config Release --target evaluation_architecture_tests
ctest --test-dir build/release -C Release -R '^evaluation_architecture_tests$' --output-on-failure
```

Expected: compilation fails specifically because `EvaluatorWorker`,
`create_worker`, and `EvaluationContext` do not yet exist.

- [ ] **Step 3: Commit the RED test.**

```powershell
git add CMakeLists.txt tests/unit/evaluation/evaluation_architecture_tests.cpp
git commit -m "test: specify evaluation worker ownership"
```

### Task 2: Add the optional public evaluator-worker capability

**Files:**
- Modify: `src/koi/evaluator.hpp`
- Modify: `src/koi/nnue.hpp`
- Modify: `src/koi/nnue.cpp`

**Interfaces:**
- Consumes: existing `Evaluator` and `NnueWorker` types.
- Produces: `EvaluatorWorker` with `evaluate(const GameState&, Color)` and
  `Evaluator::create_worker() const`, defaulting to an empty pointer;
  `NnueEvaluator::create_worker()` returns a private adapter around one
  `NnueWorker` when NNUE is enabled.

- [ ] **Step 1: Add the failing-to-green optional capability with no default-path change.**
- [ ] **Step 2: Implement the private NNUE adapter and ensure invalid/absent weights return no worker.**
- [ ] **Step 3: Run `nnue_boundary_tests`, `evaluation_boundary_tests`, and the new architecture test.**

```powershell
cmake --build build/release --config Release --target evaluation_architecture_tests nnue_boundary_tests evaluation_boundary_tests
ctest --test-dir build/release -C Release -R 'evaluation_architecture_tests|nnue_boundary_tests|evaluation_boundary_tests' --output-on-failure
```

- [ ] **Step 4: Commit the evaluator capability.**

```powershell
git add src/koi/evaluator.hpp src/koi/nnue.hpp src/koi/nnue.cpp tests/unit/evaluation/evaluation_architecture_tests.cpp
git commit -m "refactor: add optional evaluator worker capability"
```

### Task 3: Extract and integrate `EvaluationContext`

**Files:**
- Create: `src/koi/detail/evaluation_context.hpp`
- Create: `src/koi/detail/evaluation_context.cpp`
- Modify: `src/koi/detail/search_context.hpp`
- Modify: `src/koi/detail/search_context.cpp`
- Modify: `CMakeLists.txt`
- Test: `tests/unit/evaluation/evaluation_architecture_tests.cpp`, search gates

**Interfaces:**
- Consumes: `Evaluator`, optional `EvaluatorWorker`, and the existing evaluator mutex pointer.
- Produces: `detail::EvaluationContext(const Evaluator&, std::mutex*)`,
  `int evaluate(const GameState&, Color)`, and `bool has_private_worker() const`.

- [ ] **Step 1: Implement `EvaluationContext` using the tested worker-or-fallback decision.**
- [ ] **Step 2: Replace direct `evaluator.evaluate` calls in `SearchContext::evaluate` with `EvaluationContext::evaluate`.**
- [ ] **Step 3: Preserve the evaluation cache and all statistics updates around the context call.**
- [ ] **Step 4: Run evaluation, search, ordering, perft, and strength focused gates.**

```powershell
cmake --build build/release --config Release --target evaluation_architecture_tests evaluation_boundary_tests nnue_boundary_tests search_policy_tests search_architecture_tests koi_search_tests perft_tests koi_strength_tests
ctest --test-dir build/release -C Release -R 'evaluation_architecture_tests|evaluation_boundary_tests|nnue_boundary_tests|search_policy_tests|search_architecture_tests|koi_search_tests|perft_tests|koi_strength_tests' --output-on-failure
```

- [ ] **Step 5: Commit the evaluation-context extraction.**

```powershell
git add CMakeLists.txt src/koi/detail/evaluation_context.hpp src/koi/detail/evaluation_context.cpp src/koi/detail/search_context.hpp src/koi/detail/search_context.cpp
git commit -m "refactor: isolate worker evaluation context"
```

### Task 4: Verify and document Stage 4

**Files:**
- Modify: `README.md`
- Modify: `docs/superpowers/specs/2026-09-11-koi-architecture-rework-design.md`
- Create: `docs/superpowers/verification/2026-09-12-koi-architecture-stage4.md`

- [ ] **Step 1: Run the focused Release gates and the public-boundary/diff checks.**
- [ ] **Step 2: Run fresh cold/warm profiles and compare Stage 3 row signatures.**
- [ ] **Step 3: Run full Release CTest; preserve known timed limitations exactly.**
- [ ] **Step 4: Record worker/fallback, feature, scalar/AVX2, and benchmark evidence.**
- [ ] **Step 5: Commit documentation and verification.**

```powershell
git add README.md docs/superpowers/specs/2026-09-11-koi-architecture-rework-design.md docs/superpowers/verification/2026-09-12-koi-architecture-stage4.md
git commit -m "docs: record stage four evaluation architecture boundary"
```
