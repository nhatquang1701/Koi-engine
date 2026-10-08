# Koi Engine Future Architecture Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement the approved layered Koi Engine architecture with a library-independent rules state, deterministic classical search, cancellable UCI lifecycle, and strength/tooling seams.

**Architecture:** Keep `chess-library` private to a reversible `GameState` adapter. Build search and evaluation against Koi-owned moves and state; run one controller-owned search worker with an optional deterministic root-worker pool behind the UCI controller, and preserve the random chooser as a compatibility implementation.

**Tech Stack:** C++26-capable x64 MSVC, CMake 3.31+, Ninja, vendored Disservin/chess-library, standard library threads/chrono/expected/random.

**Spec:** `docs/superpowers/specs/2026-08-31-future-architecture-design.md`

## Global Constraints

- Windows x64 is the release target; CMake must require compiler architecture ID `x64`.
- C++26 is required; MSVC uses `/std:c++latest`; CMake extensions remain disabled.
- Standard chess only; preserve castling, en passant, promotion, checkmate, stalemate, and validated FEN behavior.
- No public Koi rules/search header may include `third_party/chess-library/chess.hpp`.
- Stdout must remain protocol-clean and every active search must produce at most one final `bestmove`.
- Deterministic search uses stable ordering; `RandomSeed` affects only the random chooser.
- Every implementation task follows RED → GREEN → refactor and ends with a build/test command.

---

### Task 1: Library-independent rules and move boundary

**Files:**
- Create: `src/koi/game_state.hpp`, `src/koi/game_state.cpp`
- Modify: `src/koi/move.hpp`, `src/koi/move.cpp`, `src/koi/position.hpp`, `src/koi/position.cpp`, `src/koi/move_chooser.*`, `CMakeLists.txt`
- Test: `tests/koi_core_tests.cpp`, new `tests/koi_rules_tests.cpp`

**Interfaces:**
- `Move` stores Koi `Square` coordinates and `Promotion`, exposes `from()`, `to()`, `promotion()`, `uci()`, `parse_uci()`, `no_move()`, and equality; it exposes no native chess-library type.
- `GameState` exposes `startpos()`, `from_fen()` returning `std::expected<GameState, PositionError>`, `fen()`, `side_to_move()`, `piece_at()`, `legal_moves()`, `is_legal()`, `make_move()`, `unmake_move()`, `is_capture()`, `in_check()`, `is_terminal()`, `position_key()`, and `halfmove_clock()`.
- `Position` remains a compatibility alias or forwarding wrapper until all current tests and controller code use `GameState`.

Use the exact public declarations from the spec: `Color` is `enum class Color : std::uint8_t { white, black }`; `PieceType` is `enum class PieceType : std::uint8_t { none, pawn, knight, bishop, rook, queen, king }`; `Piece` has `PieceType type` and `Color color`; `Square` has `kInvalid == 64`, `from_index(std::uint8_t)`, `parse(std::string_view)`, `index()`, `file()`, `rank()`, `uci()`, and equality; `Promotion` is `enum class Promotion : std::uint8_t { none, knight, bishop, rook, queen }`.

```cpp
class Move {
public:
    constexpr Move() noexcept;
    constexpr Move(Square from, Square to, Promotion promotion = Promotion::none) noexcept;
    static Move no_move() noexcept;
    static std::optional<Move> parse_uci(std::string_view uci) noexcept;
    Square from() const noexcept;
    Square to() const noexcept;
    Promotion promotion() const noexcept;
    bool is_no_move() const noexcept;
    std::string uci() const;
    friend constexpr bool operator==(const Move&, const Move&) noexcept = default;
};

class GameState {
public:
    GameState();
    static GameState startpos();
    static std::expected<GameState, PositionError> from_fen(std::string_view fen);
    std::string fen() const;
    Color side_to_move() const noexcept;
    Piece piece_at(Square square) const noexcept;
    std::vector<Move> legal_moves() const;
    bool is_legal(const Move& move) const noexcept;
    bool make_move(const Move& move) noexcept;
    bool unmake_move() noexcept;
    bool is_capture(const Move& move) const noexcept;
    bool in_check() const noexcept;
    bool in_check(Color color) const noexcept;
    bool is_terminal() const noexcept;
    std::uint64_t position_key() const noexcept;
    std::uint16_t halfmove_clock() const noexcept;
};
```

- [x] **Step 1: Write failing boundary tests** for no library type exposure at the move API, FEN construction, legal special moves, make/unmake FEN/key restoration, and start-position legal move count.
- [x] **Step 2: Run `cmake --build out/release-vs --config Release --target koi_core_tests koi_rules_tests` and verify the new API tests fail because the types do not exist.**
- [x] **Step 3: Implement Koi-owned move fields and coordinate parsing without including `chess.hpp` in `move.hpp`.**
- [x] **Step 4: Move validation and board interaction into `GameState` with a private implementation containing `chess::Board`; convert legal moves through coordinate UCI and keep native moves only in private history.**
- [x] **Step 5: Preserve transactional invalid FEN/move behavior and adapt `RandomMoveChooser` to `GameState`.**
- [x] **Step 6: Run Release core/rules tests and the existing process test; expected result is all current and new tests passing.**
- [x] **Step 7: Commit with `refactor: isolate rules state from chess library`.**

### Task 2: Search, evaluation, limits, and perft foundation

**Files:**
- Create: `src/koi/search_types.hpp`, `src/koi/evaluator.hpp`, `src/koi/classical_evaluator.*`, `src/koi/time_manager.*`, `src/koi/search_service.*`, `src/koi/transposition_table.*`, `tools/perft.cpp`
- Modify: `CMakeLists.txt`
- Test: new `tests/koi_search_tests.cpp`, new `tests/perft_tests.cpp`

**Interfaces:**
- `SearchLimits`, `ClockLimit`, `SearchStats`, `SearchInfo`, `SearchResult`, `SearchEventSink`, `Evaluator`, `SearchOptions`, `SearchHandle`, and `SearchService` match the approved spec.
- `SearchService` runs one controller worker per handle and supports an internal deterministic root-worker pool when `Threads > 1`; UCI threading is wired through the controller.

The exact search declarations are: `ClockLimit { std::chrono::milliseconds remaining; std::chrono::milliseconds increment; }`; `SearchLimits { std::optional<int> depth; std::optional<std::uint64_t> nodes; std::optional<std::chrono::milliseconds> movetime; std::optional<ClockLimit> white_clock; std::optional<ClockLimit> black_clock; std::optional<std::uint32_t> moves_to_go; bool infinite; }`; `SearchStats { std::uint64_t nodes; std::uint64_t qnodes; std::uint64_t tt_hits; std::chrono::milliseconds elapsed; }`; `SearchInfo { int depth; int score_cp; std::optional<int> mate; std::uint64_t nodes; std::uint64_t nps; std::chrono::milliseconds elapsed; std::vector<Move> pv; }`; `SearchResult { std::optional<Move> best_move; int score_cp; std::optional<int> mate; int completed_depth; SearchStats stats; }`; `SearchEventSink { std::function<void(const SearchInfo&)> on_info; std::function<void(const SearchResult&)> on_complete; }`; `Evaluator::evaluate(const GameState&, Color) const -> int`; and `SearchOptions { std::size_t hash_mb = 16; std::size_t threads = 1; std::uint8_t speed_percent = 100; }`.

```cpp
class SearchHandle {
public:
    SearchHandle(SearchHandle&&) noexcept;
    SearchHandle& operator=(SearchHandle&&) noexcept;
    SearchHandle(const SearchHandle&) = delete;
    SearchHandle& operator=(const SearchHandle&) = delete;
    ~SearchHandle();
    void stop() noexcept;
    void wait();
    bool running() const noexcept;
};

class SearchService {
public:
    explicit SearchService(std::shared_ptr<const Evaluator> evaluator);
    SearchHandle start(GameState root, SearchLimits limits,
                       SearchEventSink sink = {}, SearchOptions options = {});
    void set_hash_size_mb(std::size_t megabytes);
    void clear_hash() noexcept;
};
```

- [x] **Step 1: Write failing tests** for start-position perft counts, Kiwipete perft counts, evaluator material/PST sign, terminal mate/stalemate scores, deterministic fixed-depth search, legal best moves, and TT store/probe/clear.
- [x] **Step 2: Run the focused tests and verify they fail for missing search/perft implementations.**
- [x] **Step 3: Implement `ClassicalEvaluator` with fixed material values and piece-square tables, returning white/black-perspective centipawns.**
- [x] **Step 4: Implement `TimeManager` for movetime precedence, clock/increment allocation, depth/nodes/infinite limits, and a safety margin.**
- [x] **Step 5: Implement single-thread iterative-deepening negamax/alpha-beta with terminal mate scores, stable ordering, quiescence captures/promotions/check evasions, cancellation checks, and legal fallback moves.**
- [x] **Step 6: Implement a bounded `TranspositionTable` with 16 MB default, 1–4096 MB configuration, exact/lower/upper bounds, generation, and deterministic replacement.**
- [x] **Step 7: Add the `koi_perft` developer executable and register focused tests in CMake.**
- [x] **Step 8: Run Release search/perft tests and commit with `feat: add deterministic search foundation`.**

### Task 3: Asynchronous UCI lifecycle and time controls

**Files:**
- Modify: `src/koi/uci_controller.hpp`, `src/koi/uci_controller.cpp`, `src/main.cpp`, `CMakeLists.txt`, `README.md`
- Test: `tests/uci_controller_tests.cpp`, `tests/uci_process_test.ps1`

**Interfaces:**
- UCI parsing maps `go` tokens to `SearchLimits`; `SearchHandle` callbacks are serialized before writing stdout.
- Controller owns the current `GameState`, one active handle, a generation ID, and output synchronization; workers own copied roots.

The parser accepts `depth <positive int>`, `nodes <uint64>`, `movetime <nonnegative milliseconds>`, `wtime <nonnegative milliseconds>`, `btime <nonnegative milliseconds>`, `winc <nonnegative milliseconds>`, `binc <nonnegative milliseconds>`, `movestogo <positive uint>`, and the flag `infinite`, mapping the clock pairs into `ClockLimit`. Malformed values are ignored with no crash; a `go` command with no usable limit defaults to a bounded depth-1 search so Lucas Chess receives a prompt legal move. `setoption name Hash value <1..4096>` calls `SearchService::set_hash_size_mb`; `setoption name Clear Hash` calls `clear_hash`; `Threads` and `Speed` are advertised and snapshotted when `go` starts. Completion is emitted as `bestmove <Move::uci()>`, using `0000` only for a terminal root.

- [ ] **Step 1: Write failing controller/process tests** for asynchronous `go`, `stop`, `isready` during search, `position` replacement, `quit` cleanup, all supported limits, `Hash`, `Clear Hash`, no duplicate `bestmove`, and clean stdout/stderr.
- [ ] **Step 2: Run Release controller/process tests and verify the new lifecycle expectations fail against synchronous v1 behavior.**
- [x] **Step 3: Implement command parsing and `SearchService` worker callbacks; preserve handshake identity, `RandomSeed`, coordinate moves, and terminal `0000`.**
- [x] **Step 4: Implement stop/join/generation rules: `stop` emits one result, root-changing commands suppress stale results, `quit` joins without late output, and `isready` stays prompt.**
- [x] **Step 5: Emit `info depth`, score, nodes, nps, time, and pv only through valid UCI lines; protect output from worker/controller races.**
- [x] **Step 6: Extend the process harness with asynchronous reads and a five-second timeout for every transcript.**
- [x] **Step 7: Run Debug and Release CTest plus a Lucas Chess registration/game smoke test; commit with `feat: run UCI search asynchronously`.**

### Task 4: Strength options, tooling, CI, and documentation

**Files:**
- Create: `.github/workflows/windows.yml`, optional `tools/koi_bench.cpp`
- Modify: `README.md`, `CMakeLists.txt`, `src/koi/uci_controller.*`, search ordering/TT files
- Test: extend `tests/koi_search_tests.cpp`, `tests/uci_controller_tests.cpp`, and process tests

**Interfaces:**
- `Hash` is a UCI spin option with default 16 MB and range 1–4096 MB; `Clear Hash` clears the table. `Threads` is a UCI spin option from 1 to the portable hardware limit, and `Speed` is a UCI spin option from 1 to 100.
- TT move ordering, captures/MVV-LVA, killer/history heuristics, and optional static exchange evaluation remain internal `SearchContext` policies.

- [x] **Step 1: Write failing tests** for `Hash`/`Clear Hash`, TT move preference, killer/history ordering stability, benchmark output isolation, and Windows CI configuration.
- [x] **Step 2: Run focused tests and verify failure before implementation.**
- [x] **Step 3: Add TT move ordering, MVV-LVA, killer/history heuristics, and bounded hash reconfiguration without changing public rules APIs.**
- [x] **Step 4: Add a deterministic benchmark tool that writes only its own stdout and never affects UCI protocol output.**
- [x] **Step 5: Add Windows Debug/Release CI using the x64 VS developer environment and CTest.**
- [x] **Step 6: Update README with architecture, search limits, options, debugging, perft, benchmark, and Lucas acceptance instructions.**
- [x] **Step 7: Run the complete Release and Debug suites, `git diff --check`, and final manual Lucas games; commit with `docs: document future engine architecture`.**
