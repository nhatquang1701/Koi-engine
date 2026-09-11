# Koi Stage 1 State Ownership Seam Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the native rules state, compatibility mirror, and feature cache explicit private owners without changing Koi's public rules, search, UCI, or deterministic behavior contracts.

**Architecture:** `Position` remains the authoritative rules state. `GameState::Impl` will contain a private `detail::CompatibilityMirror` for the vendored `chess.hpp` board and its reversible shadow history, plus a private `detail::FeatureState` for feature-cache storage and synchronization. `GameState` remains the stable facade and coordinates native-first transactions, mirror updates, rollback, and diagnostic comparison at the existing validation boundaries.

**Tech Stack:** C++26, MSVC/Ninja via CMake 3.31+, CTest, the existing chess-library differential target, and PowerShell benchmark/release harnesses.

**Spec:** `docs/superpowers/specs/2026-09-11-koi-architecture-rework-design.md`

## Global Constraints

- `Position` remains authoritative for board contents, legal moves, rule state, make/unmake, repetition, and the native position key.
- The vendored `chess.hpp` mirror remains private compatibility infrastructure and is not removed in Stage 1.
- No public Koi header or C++26 module partition may include or export `chess.hpp`, `chess::Board`, `chess::Move`, or a private worker/cache type.
- Preserve `GameState`, `Position`, `MoveMetadata`, `SearchService`, UCI behavior, deterministic `Threads=1` behavior, and the existing 38-test Release gate.
- Preserve native-first transactional behavior: a mirror conversion, mirror update, or mirror comparison failure rolls the native transition back.
- Preserve the search fast path: `make_search_move` updates the mirror transactionally but does not perform an interior-node mirror comparison; explicit validation and diagnostic paths retain comparison.
- Follow RED -> GREEN -> refactor for every new private-interface test; do not change production code before its focused test has failed for the intended missing-interface reason.
- Do not delete `Goal.txt` until the complete multi-stage architecture objective is genuinely complete; Stage 1 completion is not the overall goal completion.

---

### Task 1: Add failing ownership-seam tests and capture the pre-change measurement

**Files:**
- Modify: `tests/unit/rules/native_rule_state_tests.cpp`
- Modify: `CMakeLists.txt` for the private include path needed by the new tests
- Create later in Task 2: `src/koi/detail/compatibility_mirror.hpp`
- Create later in Task 3: `src/koi/detail/feature_state.hpp`

**Interfaces:**
- The tests consume `koi::detail::CompatibilityMirror` with `set_fen`, `apply_generated_move`, `undo_move`, `matches`, `history_size`, `apply_null_move`, `undo_null_move`, and `last_move_is_null`.
- The tests consume `koi::detail::FeatureState::get_or_compute`, `invalidate`, `cache_misses`, and `fast_hits`.
- These are private implementation seams, not public Koi API; later tasks implement exactly these names and parameter types.

- [ ] **Step 1: Record the current cold and warm benchmark profiles before touching production code.**

Run from the repository root:

~~~powershell
New-Item -ItemType Directory -Force artifacts/verification | Out-Null
cmake --build build/release --config Release --target koi_bench
& ./build/release/koi-bench.exe --threads 1 --speed 100 --profile-json ./artifacts/verification/stage1-before-cold.json
& ./build/release/koi-bench.exe --threads 1 --speed 100 --warm-hash --profile-json ./artifacts/verification/stage1-before-warm.json
~~~

Expected: both commands exit 0, emit the normal `Koi benchmark` header, and write `koi-bench-profile-v1` JSON with 64 positions, `threads: 1`, `speed: 100`, and the requested `hash_state`.

- [ ] **Step 2: Add the failing compatibility-mirror ownership test.**

Add `koi/detail/compatibility_mirror.hpp` to the test includes and add this test after the existing `require_state` helper:

~~~cpp
void test_compatibility_mirror_owns_only_shadow_state_and_history() {
    constexpr std::string_view fen =
        "r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1";
    const auto parsed = GameState::from_fen(fen);
    require(parsed.has_value(), "ownership seam fixture must be valid");
    const auto move = Move::parse_uci("e1g1");
    require(move.has_value(), "ownership seam move must parse");
    const auto metadata = parsed->describe_move(*move);
    require(metadata.has_value(), "ownership seam move must have metadata");

    Position native(fen);
    koi::detail::CompatibilityMirror mirror;
    require(mirror.set_fen(fen), "compatibility mirror must accept a valid native FEN");
    require(mirror.matches(native),
            "a freshly initialized compatibility mirror must match the native authority");
    require(mirror.apply_generated_move(*metadata, true),
            "the mirror must apply generated special-move metadata transactionally");

    Position child(native);
    require(child.make_generated_move(metadata->move),
            "the native authority must apply the same generated special move");
    require(mirror.matches(child),
            "mirror state must match the independently advanced native state");
    require(mirror.history_size() == 1 && !mirror.last_move_is_null(),
            "normal mirror moves must add one non-null shadow history record");
    require(mirror.undo_move(), "the mirror must undo its own normal history record");
    require(mirror.matches(native) && mirror.history_size() == 0,
            "undoing the mirror must restore its source state without native mutation");

    require(mirror.apply_null_move(), "the mirror must own null-move shadow transitions");
    require(mirror.last_move_is_null(), "a null transition must be identifiable in shadow history");
    require(mirror.undo_null_move() && mirror.history_size() == 0,
            "the mirror null transition must be independently reversible");
}
~~~

- [ ] **Step 3: Add the failing feature-cache ownership test.**

Add `koi/detail/feature_state.hpp`, define this deterministic builder, and add the test:

~~~cpp
PositionFeatures ownership_feature_builder(const Position& position) noexcept {
    PositionFeatures features{};
    features.side_to_move = position.side_to_move();
    features.fullmove_number = position.fullmove_number();
    return features;
}

void test_feature_state_owns_cache_publication_and_invalidation() {
    const Position native;
    koi::detail::FeatureState cache;
    const std::uint64_t key = native.position_key();

    const PositionFeatures first = cache.get_or_compute(
        0, key, native, ownership_feature_builder);
    const PositionFeatures second = cache.get_or_compute(
        0, key, native, ownership_feature_builder);
    require(first.side_to_move == Color::white && second.fullmove_number == 1,
            "feature ownership seam must return the builder's published value");
    require(cache.cache_misses() == 1 && cache.fast_hits() == 1,
            "a repeated feature request must publish once and hit the lock-free path once");

    cache.invalidate(0);
    (void)cache.get_or_compute(0, key, native, ownership_feature_builder);
    require(cache.cache_misses() == 2,
            "invalidating one position slot must force exactly one subsequent rebuild");
}
~~~

Register both tests in the existing test table. Run:

~~~powershell
cmake --build build/release --config Release --target native_rule_state_tests
ctest --test-dir build/release -C Release -R native_rule_state_tests --output-on-failure
~~~

Expected: the build/test fails because the new private headers and types do not exist yet. A missing-header or missing-symbol failure is the intended RED result; fix only test typos if the compiler reports one.

- [ ] **Step 4: Commit the RED tests and baseline setup.**

~~~powershell
git add tests/unit/rules/native_rule_state_tests.cpp CMakeLists.txt
git commit -m "test: specify stage one state ownership seams"
~~~

The commit may contain a deliberately failing test; the next commits must make it green before broader work continues.

### Task 2: Extract the private compatibility mirror

**Files:**
- Create: `src/koi/detail/compatibility_mirror.hpp`
- Create: `src/koi/detail/compatibility_mirror.cpp`
- Modify: `src/koi/game_state.cpp`
- Modify: `src/koi/game_state.hpp` only for diagnostic-boundary comments; do not change signatures
- Modify: `CMakeLists.txt`
- Test: `tests/unit/rules/native_rule_state_tests.cpp`
- Test: `tests/integration/rules/native_shadow_diff_tests.cpp`

**Interfaces:**

`CompatibilityMirror` stores the private `chess::Board` and a private vector of records `{chess::Move move; bool null_move; std::uint64_t position_key;}`. Its exact private-header interface is:

~~~cpp
namespace koi::detail {
class CompatibilityMirror final {
public:
    CompatibilityMirror() = default;
    CompatibilityMirror(const CompatibilityMirror&) = default;
    CompatibilityMirror& operator=(const CompatibilityMirror&) = default;

    [[nodiscard]] bool set_fen(std::string_view fen);
    [[nodiscard]] bool apply_move(const Move& move) noexcept;
    [[nodiscard]] bool apply_generated_move(const MoveMetadata& metadata,
                                            bool verify_legality) noexcept;
    [[nodiscard]] bool undo_move() noexcept;
    [[nodiscard]] bool apply_null_move() noexcept;
    [[nodiscard]] bool undo_null_move() noexcept;
    [[nodiscard]] bool last_move_is_null() const noexcept;
    [[nodiscard]] std::size_t history_size() const noexcept;

    [[nodiscard]] std::uint64_t position_key() const noexcept;
    [[nodiscard]] std::uint8_t castling_rights() const noexcept;
    [[nodiscard]] Square en_passant_square() const noexcept;
    [[nodiscard]] std::uint16_t halfmove_clock() const noexcept;
    [[nodiscard]] std::uint16_t fullmove_number() const noexcept;
    [[nodiscard]] bool in_check() const noexcept;
    [[nodiscard]] Color side_to_move() const noexcept;
    [[nodiscard]] std::size_t repetition_count() const noexcept;

    [[nodiscard]] std::vector<std::string> legal_move_strings() const;
    [[nodiscard]] std::string fen_for_comparison(const Position& native) const;
    [[nodiscard]] Square en_passant_square_for_comparison(const Position& native) const;
    [[nodiscard]] bool matches(const Position& native) const;
    [[nodiscard]] bool gives_check(const Move& move) const noexcept;
};
}
~~~

- [ ] **Step 1: Add the adapter header with the exact private interface.**

Include the Koi value types and the vendored library only from this `detail` header. Do not include this header from a public Koi header or module partition. Keep the board and history members private.

- [ ] **Step 2: Move chess-library conversion and comparison helpers into the adapter implementation.**

Move `valid_check_counts`, Koi/chess move and promotion conversion helpers, shadow castling/en-passant normalization, sorted shadow move generation, and `mirror_matches_native` from `game_state.cpp` into `compatibility_mirror.cpp`. Preserve the existing comparison semantics: native en-passant state is compared exactly when a legal capture exists, while an uncapturable target is normalized away for FEN/diagnostic comparison.

- [ ] **Step 3: Implement mirror transactions with rollback-safe history ownership.**

`set_fen` parses into a candidate board, rejects failed parsing or invalid check counts without replacing the current board, and clears shadow history only after the candidate is accepted. `apply_move` and `apply_generated_move` convert the Koi move, optionally check shadow legality, push the pre-move shadow key, and make the board move. On an exception or invalid conversion, restore the board/history and return `false`. Null moves use the same record type with `null_move == true`.

- [ ] **Step 4: Replace direct `GameState::Impl` board/history ownership with the adapter.**

Change `GameState::Impl` to contain `detail::CompatibilityMirror compatibility_mirror;` and `Position native_position;`; remove its `chess::Board`, `HistoryRecord`, and mirror helper fields. Copy construction must copy both owners. `GameState::from_fen`, `consistency_snapshot`, `native_shadow_consistent`, metadata check probes, `make_move`, `apply_generated_move`, `unmake_move`, `make_null_move`, `unmake_null_move`, and `polyglot_key` use adapter methods.

Preserve this transaction order in `GameState`:

~~~text
native Position transition
    -> CompatibilityMirror transition
    -> optional mirror.matches(native) at the existing validation boundary
    -> invalidate the feature slot
    -> publish success
~~~

On any mirror failure, call the matching native unmake before returning `false`. `make_search_move` passes `verify_mirror == false` to the coordinating path; `make_move` and `make_legal_move` retain explicit mirror checks.

- [ ] **Step 5: Build the adapter and run the focused RED-to-GREEN cycle.**

~~~powershell
cmake --build build/release --config Release --target native_rule_state_tests koi_rules_tests koi_shadow_diff_tests
ctest --test-dir build/release -C Release -R "native_rule_state_tests|koi_rules_tests|koi_shadow_diff_tests" --output-on-failure
~~~

Expected: all selected tests pass, including the two new ownership tests and the existing special-move, repetition, fixed-seed differential, and shadow-snapshot checks. If a test fails, correct the adapter/transaction code; do not weaken the assertions.

- [ ] **Step 6: Commit the compatibility seam.**

~~~powershell
git add src/koi/detail/compatibility_mirror.hpp src/koi/detail/compatibility_mirror.cpp src/koi/game_state.cpp src/koi/game_state.hpp CMakeLists.txt tests/unit/rules/native_rule_state_tests.cpp tests/integration/rules/native_shadow_diff_tests.cpp
git commit -m "refactor: isolate compatibility mirror ownership"
~~~

### Task 3: Extract feature-cache ownership

**Files:**
- Create: `src/koi/detail/feature_state.hpp`
- Create: `src/koi/detail/feature_state.cpp`
- Modify: `src/koi/game_state.cpp`
- Modify: `CMakeLists.txt`
- Test: `tests/unit/rules/native_rule_state_tests.cpp`
- Test: `tests/unit/core/koi_core_tests.cpp`

**Interfaces:**

`FeatureState` owns the cache entries, publication atomics, maintenance mutex, invalidation, and diagnostic counters currently named `feature_cache_*` in `GameState::Impl`. Use this exact function-pointer boundary so the cache does not depend on evaluator/search code:

~~~cpp
namespace koi::detail {
using FeatureBuilder = PositionFeatures (*)(const Position&) noexcept;

class FeatureState final {
public:
    FeatureState();
    FeatureState(const FeatureState& other);

    [[nodiscard]] PositionFeatures get_or_compute(
        std::size_t cache_index, std::uint64_t position_key,
        const Position& position, FeatureBuilder builder) const noexcept;
    void invalidate(std::size_t cache_index) noexcept;
    [[nodiscard]] std::uint64_t cache_misses() const noexcept;
    [[nodiscard]] std::uint64_t fast_hits() const noexcept;
    [[nodiscard]] std::uint64_t snapshot_copies() const noexcept;
};
}
~~~

- [ ] **Step 1: Add the `FeatureState` header and implementation only after the RED test proves the missing interface.**

Move the existing `FeatureCache` entry type and `kMaximumGameStateHistory` constant into the private implementation. Initialize all published pointers, validity flags, and keys to invalid in the constructor.

- [ ] **Step 2: Implement `get_or_compute` from the existing cache behavior.**

Keep the current ordering intact: acquire-load the published-valid flag, pointer, and key for the lock-free hit; increment `fast_hits` only for that path; take the shared/unique maintenance lock for the slow path; return a valid matching slot without rebuilding; increment `cache_misses` only for a rebuild; call the supplied `FeatureBuilder` from the authoritative `Position`; and catch allocation failure while returning the freshly computed feature value. Do not add heap allocation to a published cache hit.

- [ ] **Step 3: Implement copy and invalidation semantics.**

Copy only valid slots up to `min(source_history_size + 1, kMaximumGameStateHistory)` through `FeatureState`'s copy constructor, republish copied slot pointers, and copy all counters. `invalidate(index)` clears the ordinary-valid flag, published-valid flag, and published key for the current history slot under the maintenance lock.

- [ ] **Step 4: Replace the cache fields and logic in `GameState::Impl`.**

Replace all direct `feature_cache_*` members with `detail::FeatureState feature_state;`. `GameState::position_features()` calls:

~~~cpp
return impl_->feature_state.get_or_compute(
    impl_->compatibility_mirror.history_size(),
    impl_->native_position.position_key(),
    impl_->native_position,
    native_position_features);
~~~

The three existing diagnostic accessors delegate to `FeatureState`; `invalidate_feature_cache()` delegates to `feature_state.invalidate(compatibility_mirror.history_size())`. Preserve the existing `PositionFeatures` values and all cache counter semantics.

- [ ] **Step 5: Run focused cache and rules tests.**

~~~powershell
cmake --build build/release --config Release --target native_rule_state_tests koi_core_tests koi_rules_tests
ctest --test-dir build/release -C Release -R "native_rule_state_tests|koi_core_tests|koi_rules_tests" --output-on-failure
~~~

Expected: the new cache ownership test passes, feature extraction/replay and cache-restoration tests remain green, and make/unmake/key/null/repetition behavior is unchanged.

- [ ] **Step 6: Commit the feature-cache seam.**

~~~powershell
git add src/koi/detail/feature_state.hpp src/koi/detail/feature_state.cpp src/koi/game_state.cpp CMakeLists.txt tests/unit/rules/native_rule_state_tests.cpp tests/unit/core/koi_core_tests.cpp
git commit -m "refactor: isolate feature cache ownership"
~~~

### Task 4: Document the actual Stage 1 boundary

**Files:**
- Modify: `README.md:11-35`
- Modify: `docs/superpowers/specs/2026-09-11-koi-architecture-rework-design.md`
- Create: `docs/superpowers/verification/2026-09-11-koi-architecture-stage1.md`

**Interfaces:**
- Documentation names `Position` as the only production rules authority, `CompatibilityMirror` as a private adapter with its own shadow history, and `FeatureState` as a cache owner derived from `Position`.
- Documentation states that `make_search_move` still updates the mirror transactionally but skips interior mirror comparison, while explicit validation/diagnostic boundaries retain comparison.
- The verification record consumes the before/after profile JSONs and command output from Task 5 and does not claim a strength or Elo change from this structural refactor.

- [ ] **Step 1: Update the README architecture paragraph.**

Replace the current wording that describes the mirror as a generic feature dependency with wording that says feature extraction is native-position based, while the mirror remains required for Polyglot/book compatibility, explicit differential validation, and legacy adapters. Mention both private owners by name and keep the public-header rule.

- [ ] **Step 2: Add the Stage 1 implementation note to the architecture spec.**

Record the concrete file-level seam (`src/koi/detail/compatibility_mirror.*`, `src/koi/detail/feature_state.*`, and `GameState::Impl`) and the still-open mirror consumers. State that mirror removal remains gated on adapter migration and differential evidence.

- [ ] **Step 3: Create the verification record with explicit baseline fields.**

Create the file with these sections:

~~~markdown
# Koi Architecture Stage 1 Verification

Date: 2026-09-11

## Scope

Private state-ownership seam only; no search formula, UCI contract, or public
rules signature change.

## Build and test evidence

Record the exact focused Release command and result, the exact full Release
command and result, the exact full Debug command and result (or that the
configured Debug tree was unavailable), and the exact public-header scan
command and result.

## Benchmark profiles

Record four rows named before-cold, before-warm, after-cold, and after-warm.
For every row record hash state, threads, speed, position count, aggregate
nodes, aggregate qnodes, and the profile path. Record deterministic
move/score/node parity between corresponding before and after rows.

The benchmark is a correctness/performance baseline. It does not establish Elo
or playing-strength improvement.
~~~

Do not leave unfinished field markers in the completed verification record; replace every requested field with an exact value or an explicit unavailable marker.

- [ ] **Step 4: Commit documentation after actual values are available.**

~~~powershell
git add README.md docs/superpowers/specs/2026-09-11-koi-architecture-rework-design.md docs/superpowers/verification/2026-09-11-koi-architecture-stage1.md
git commit -m "docs: record stage one state ownership boundary"
~~~

### Task 5: Run the full Stage 1 verification and record evidence

**Files:**
- Modify: `docs/superpowers/verification/2026-09-11-koi-architecture-stage1.md`
- Create ignored artifacts: `artifacts/verification/stage1-after-cold.json`, `artifacts/verification/stage1-after-warm.json`
- Inspect only: `src/koi/*.hpp`, `src/koi/modules/*.ixx`, `git diff --check`, and `Goal.txt`

**Interfaces:**
- Verification consumes the existing CMake targets, CTest registrations, release/debug build trees, and benchmark profile schema; no new test bypass or relaxed gate is allowed.

- [ ] **Step 1: Build and run focused Release tests after the seam.**

~~~powershell
cmake --build build/release --config Release --target native_rule_state_tests koi_rules_tests koi_core_tests koi_shadow_diff_tests
ctest --test-dir build/release -C Release -R "native_rule_state_tests|koi_rules_tests|koi_core_tests|koi_shadow_diff_tests" --output-on-failure
~~~

Expected: exit 0 and all selected tests pass.

- [ ] **Step 2: Run the full Release and Debug gates.**

~~~powershell
ctest --test-dir build/release -C Release --output-on-failure
ctest --test-dir build/debug -C Debug --output-on-failure
~~~

Expected: each configured tree exits 0; record exact test counts and durations. If the Debug tree is not configured, configure/build it with the repository's documented CMake command before running the command.

- [ ] **Step 3: Run after-change cold/warm benchmark profiles.**

~~~powershell
& ./build/release/koi-bench.exe --threads 1 --speed 100 --profile-json ./artifacts/verification/stage1-after-cold.json
& ./build/release/koi-bench.exe --threads 1 --speed 100 --warm-hash --profile-json ./artifacts/verification/stage1-after-warm.json
~~~

Expected: both profiles are schema-valid, contain the same 64 positions and depths as the before profiles, and have deterministic move/score/node results for each corresponding position. Record wall-clock movement separately; do not call it a strength change.

- [ ] **Step 4: Run the public-boundary and diff checks.**

~~~powershell
$leaks = rg -n '#include <chess\.hpp>|chess::(Board|Move|Color|Piece)' src/koi/*.hpp src/koi/modules/*.ixx
if ($LASTEXITCODE -eq 0) { throw 'public Koi headers or modules leak chess-library types' }
git diff --check
git status --short
~~~

Expected: the scan produces no matches, `git diff --check` exits 0, and the status contains only intended tracked changes plus the temporary `Goal.txt` until the complete multi-stage goal is finished.

- [ ] **Step 5: Update the verification record from fresh outputs, review the plan, and commit.**

Record exact commands, test counts, profile aggregate values, deterministic parity, and limitations. Re-read the design spec against the Stage 1 acceptance criteria; do not claim the overall architecture is complete. Then run:

~~~powershell
git add docs/superpowers/verification/2026-09-11-koi-architecture-stage1.md
git commit -m "test: verify stage one architecture seam"
~~~

## Plan self-review

- Spec coverage: Stage 1 native/mirror/feature ownership is covered by Tasks 2 and 3; transactional and diagnostic behavior by Tasks 1 and 2; benchmark/release gates by Tasks 1 and 5; documentation by Task 4; public-boundary protection by Task 5. Later search/evaluation/TT/time/parallel stages are intentionally outside this plan.
- Placeholder scan: the implementation tasks contain no `TODO`, `TBD`, or deferred implementation instruction; Task 4 Step 3 requires every requested verification field to be filled with a value before commit.
- Type consistency: `CompatibilityMirror` methods used by the tests and GameState integration are defined in Task 2; `FeatureState::get_or_compute` uses the `FeatureBuilder` type defined in Task 3; `history_size()` is the shared index source for mirror and feature-cache ownership.


