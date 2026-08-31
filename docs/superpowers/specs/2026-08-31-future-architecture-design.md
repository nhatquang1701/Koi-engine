# Koi Engine Future Architecture Design

## Goal

Evolve the v1 random-move UCI executable into a deterministic classical chess engine while preserving standard UCI behavior and Lucas Chess compatibility.

## Decisions

- Windows x64, Lucas Chess, C++26, CMake 3.31+, Ninja, and MSVC remain the primary release constraints.
- Standard chess is the only implemented ruleset; Chess960 and other variants are deferred.
- `third_party/chess-library/chess.hpp` remains pinned and private to the rules adapter.
- Koi-owned `GameState`, `Move`, `SearchLimits`, and `SearchResult` types are the public engine boundary.
- The rules state uses reversible make/unmake and retains repetition/key history.
- Search uses a controller-owned iterative-deepening negamax/alpha-beta worker with deterministic move ordering and an optional deterministic root-worker pool.
- A classical evaluator is implemented behind an `Evaluator` interface; learned evaluators are future implementations of that interface.
- UCI search runs on a cancellable worker so the command loop remains responsive to `stop`, `isready`, and `quit`.
- Search is deterministic by default. `RandomSeed` remains meaningful only to the random compatibility chooser.
- Correctness, protocol stability, complete Lucas Chess games, and deterministic threaded behavior are release gates before rating-oriented tuning.

## Layering

```text
protocol/controller -> search service -> rules state -> chess-library adapter
                                      \-> evaluator
                                      \-> time manager
```

The controller owns the current root state. A search receives an independent copy and owns its mutable make/unmake stack. No public engine header includes `chess.hpp`.

## Interfaces

`Move` stores Koi coordinates and an optional promotion piece and formats coordinate UCI such as `e2e4` and `e7e8q`. Castling is represented by its UCI king destination.

`GameState` provides `startpos()`, validated FEN construction, FEN output, side-to-move, piece queries, legal moves, legality checks, `make_move`, `unmake_move`, capture checks, check checks, terminal checks, halfmove state, and a 64-bit position key.

`SearchLimits` represents depth, node, move-time, both clocks, increments, moves-to-go, and infinite search. `SearchResult` contains an optional legal best move, centipawn/mate score, completed depth, and search statistics.

`Evaluator::evaluate(const GameState&, Color)` returns a score from the requested perspective. `SearchService::start(...)` returns a move-only `SearchHandle` with `stop()`, `wait()`, and `running()`. Search event callbacks report completed-depth `info` and exactly one final result.

## Normative C++ boundary signatures

The following declarations define the names, ownership, and return-value contracts used by the implementation. Headers may add `constexpr`, `noexcept`, default arguments, and private helpers, but public Koi headers must not expose a chess-library type.

```cpp
namespace koi {

enum class Color : std::uint8_t { white, black };
[[nodiscard]] constexpr Color opposite(Color color) noexcept;

enum class PieceType : std::uint8_t { none, pawn, knight, bishop, rook, queen, king };
struct Piece {
    PieceType type = PieceType::none;
    Color color = Color::white;
    [[nodiscard]] constexpr bool empty() const noexcept;
};

struct Square {
    static constexpr std::uint8_t kInvalid = 64;
    constexpr Square() noexcept;
    [[nodiscard]] static constexpr Square from_index(std::uint8_t index) noexcept;
    [[nodiscard]] static std::optional<Square> parse(std::string_view coordinate) noexcept;
    [[nodiscard]] constexpr std::uint8_t index() const noexcept;
    [[nodiscard]] constexpr char file() const noexcept;
    [[nodiscard]] constexpr char rank() const noexcept;
    [[nodiscard]] std::string uci() const;
    friend constexpr bool operator==(Square, Square) noexcept = default;
};

enum class Promotion : std::uint8_t { none, knight, bishop, rook, queen };

class Move {
public:
    constexpr Move() noexcept; // no move
    constexpr Move(Square from, Square to, Promotion promotion = Promotion::none) noexcept;
    [[nodiscard]] static Move no_move() noexcept;
    [[nodiscard]] static std::optional<Move> parse_uci(std::string_view uci) noexcept;
    [[nodiscard]] Square from() const noexcept;
    [[nodiscard]] Square to() const noexcept;
    [[nodiscard]] Promotion promotion() const noexcept;
    [[nodiscard]] bool is_no_move() const noexcept;
    [[nodiscard]] std::string uci() const;
    friend constexpr bool operator==(const Move&, const Move&) noexcept = default;
};

enum class PositionErrorCode : std::uint8_t { malformed_fen, illegal_position };
struct PositionError {
    PositionErrorCode code;
    std::string message;
};

class GameState {
public:
    GameState();
    GameState(const GameState&);
    GameState(GameState&&) noexcept;
    GameState& operator=(const GameState&);
    GameState& operator=(GameState&&) noexcept;
    ~GameState();

    [[nodiscard]] static GameState startpos();
    [[nodiscard]] static std::expected<GameState, PositionError> from_fen(std::string_view fen);
    [[nodiscard]] std::string fen() const;
    [[nodiscard]] Color side_to_move() const noexcept;
    [[nodiscard]] Piece piece_at(Square square) const noexcept;
    [[nodiscard]] std::vector<Move> legal_moves() const;
    [[nodiscard]] bool is_legal(const Move& move) const noexcept;
    [[nodiscard]] bool make_move(const Move& move) noexcept;
    [[nodiscard]] bool unmake_move() noexcept;
    [[nodiscard]] bool is_capture(const Move& move) const noexcept;
    [[nodiscard]] bool in_check() const noexcept;
    [[nodiscard]] bool in_check(Color color) const noexcept;
    [[nodiscard]] bool is_terminal() const noexcept;
    [[nodiscard]] std::uint64_t position_key() const noexcept;
    [[nodiscard]] std::uint16_t halfmove_clock() const noexcept;
};

struct ClockLimit {
    std::chrono::milliseconds remaining{0};
    std::chrono::milliseconds increment{0};
};

struct SearchLimits {
    std::optional<int> depth;
    std::optional<std::uint64_t> nodes;
    std::optional<std::chrono::milliseconds> movetime;
    std::optional<ClockLimit> white_clock;
    std::optional<ClockLimit> black_clock;
    std::optional<std::uint32_t> moves_to_go;
    bool infinite = false;
};

struct SearchStats {
    std::uint64_t nodes = 0;
    std::uint64_t qnodes = 0;
    std::uint64_t tt_hits = 0;
    std::chrono::milliseconds elapsed{0};
};

struct SearchInfo {
    int depth = 0;
    int score_cp = 0;
    std::optional<int> mate;
    std::uint64_t nodes = 0;
    std::uint64_t nps = 0;
    std::chrono::milliseconds elapsed{0};
    std::vector<Move> pv;
};

struct SearchResult {
    std::optional<Move> best_move;
    int score_cp = 0;
    std::optional<int> mate;
    int completed_depth = 0;
    SearchStats stats;
};

struct SearchEventSink {
    std::function<void(const SearchInfo&)> on_info;
    std::function<void(const SearchResult&)> on_complete;
};

class Evaluator {
public:
    virtual ~Evaluator() = default;
    [[nodiscard]] virtual int evaluate(const GameState&, Color perspective) const = 0;
};

struct SearchOptions {
    std::size_t hash_mb = 16;
    std::size_t threads = 1;
    std::uint8_t speed_percent = 100;
};

class SearchHandle {
public:
    SearchHandle(SearchHandle&&) noexcept;
    SearchHandle& operator=(SearchHandle&&) noexcept;
    SearchHandle(const SearchHandle&) = delete;
    SearchHandle& operator=(const SearchHandle&) = delete;
    ~SearchHandle();
    void stop() noexcept;
    void wait();
    [[nodiscard]] bool running() const noexcept;
};

class SearchService {
public:
    explicit SearchService(std::shared_ptr<const Evaluator> evaluator);
    [[nodiscard]] SearchHandle start(GameState root, SearchLimits limits,
                                     SearchEventSink sink = {},
                                     SearchOptions options = {});
    void set_hash_size_mb(std::size_t megabytes);
    void clear_hash() noexcept;
};

} // namespace koi
```

`Position` may remain as a forwarding compatibility wrapper during the migration, but it must delegate to `GameState` and must not re-expose `chess::Board` or `chess::Move`.

## Lifecycle

- `go` stops and joins any prior search, snapshots the root, and starts one controller worker; that worker may create a root-worker pool according to the snapped `Threads` value.
- `stop` requests cancellation, joins the worker, and emits the worker's one final `bestmove`.
- `position`, `ucinewgame`, and search-affecting `setoption` commands stop and join before mutating state; stale callbacks are ignored.
- `quit` stops and joins without emitting a late result, then exits normally.
- `isready` responds promptly and does not mutate the search.
- Cancellation is checked at node, move, and iteration boundaries. A stopped search returns its best legal root move, or the deterministic first legal move if no iteration completed. Only terminal positions return `0000`.

## Strength sequence

1. Rules boundary, make/unmake, history, and perft.
2. Iterative-deepening negamax/alpha-beta, material plus piece-square evaluation, quiescence, and deterministic ordering.
3. UCI time manager and asynchronous lifecycle.
4. Bounded transposition table (`Hash`, default 16 MB, range 1–4096 MB), TT ordering, killers, history, MVV-LVA, static exchange evaluation, aspiration windows, conservative pruning, and deterministic root-parallel search with `Threads`/`Speed` controls.
5. Optional opening book, tablebases, and NNUE-compatible evaluators.

## Compatibility and diagnostics

The engine continues to emit only valid UCI responses on stdout. Diagnostics and invalid-position explanations use stderr or `info string`. Existing FEN/move transactional behavior, coordinate notation, terminal `bestmove 0000`, and `RandomSeed` compatibility remain covered.
