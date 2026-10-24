# Koi engine specification

This page is the engineering specification for the Koi engine in this repository.
It describes the code as it stands on branch `koi-engine` at commit `7f2b2ed`
(the v1.1.4 release plus the unreleased 1.1.5 work in the tree), and it names the
implementation files and constants that define each behaviour. It is a
specification of observed behaviour, not a roadmap: where a feature is absent,
experimental, or deliberately limited, the text says so.

The engine is a standard-chess UCI engine written in C++26. `Threads=1` is the
deterministic configuration; `Threads>1` is intentionally nondeterministic Lazy
SMP. The classical evaluator is the default; the Koi-native NNUE evaluator is
opt-in through `EvalFile` or boot-time auto-discovery. GPU NNUE is experimental,
off at runtime, and scheduled for removal.

## 1. Identity and platforms

### 1.1 Identity and version provenance

| Property | Value | Source |
| --- | --- | --- |
| Product name | `Koi` | `src/koi/uci_controller.cpp` (`write_handshake`) |
| Author string | `Koi contributors` | `src/koi/uci_controller.cpp` |
| Protocol | UCI (Universal Chess Interface) | `src/koi/uci_controller.cpp` |
| Rules variant | Standard chess only; no Chess960 or other variants | `src/koi/position.cpp` |
| Language | C++26 (`CMAKE_CXX_STANDARD 26`) | `CMakeLists.txt` |
| License | MIT | `LICENSE` |
| Current build version pin | `1.1.4` | `project(koi_engine VERSION 1.1.4 ...)`, `CMakeLists.txt:3` |
| Compiled build identity | `KOI_ENGINE_BUILD_VERSION="${PROJECT_VERSION}"` | `CMakeLists.txt:412-414` |
| UCI version token | none: the handshake emits only `id name` and `id author` | `src/koi/uci_controller.cpp:1631-1664` |

The version is not part of the UCI handshake. The CMake project version is
compiled into the core library and is consumed by `koi-bench` for its JSON
`build` field (`tools/engine/koi_bench.cpp:577` and `:685`). Release provenance
instead comes from the package manifest written by
`tools/build/package_release.ps1`: schema `koi-engine-package-v1`, hard-coded
`version = '1.1.4'`, per-file SHA-256 hashes, and a `provenance` object with
`source_commit`, `source_dirty`, `source_ref`, and `created_utc`
(`tools/build/package_release.ps1:254-268`). A package archive also gets a
`.sha256` sidecar (`tools/build/package_release.ps1:283-284`).

The v1.1.5 work in this tree is unreleased and the version pins still read
`1.1.4`; the build identity therefore remains 1.1.4 until a release commit
bumps `CMakeLists.txt`, the packaging script, and the documentation.

### 1.2 Supported operating systems and CPUs

| Platform | Supported toolchain | Bitness / CPU | Notes |
| --- | --- | --- | --- |
| Windows | MSVC x64 only; `/std:c++latest`; CMake 3.31+; Ninja | x86-64 only; 64-bit required | Non-MSVC Windows compilers fail configuration; MinGW/GCC/Clang are not supported |
| Linux | GCC 14+ or Clang 18+; CMake 3.31+; Ninja | x86-64 only; 64-bit required | Requires pthreads; `pwsh` and Python 3 enable optional process/Python tests |
| macOS | AppleClang 18+ or Homebrew LLVM clang 18+ (clang 19+ recommended) | Documented release target is arm64 Apple Silicon; the CMake tree also contains an x86-64 macOS path, but releases ship arm64 only | Documented build passes `-DKOI_BUILD_MODULES=OFF -DKOI_STATIC_RUNTIME=OFF`; GPU NNUE is unavailable |

Build constraints are enforced in `CMakeLists.txt:1-87`. A 64-bit toolchain is
mandatory (`CMAKE_SIZEOF_VOID_P EQUAL 8`), the Windows compiler must be MSVC, the
Linux architecture must be `x86_64`, and the macOS compiler must be AppleClang
18+ or Clang 18+.

### 1.3 Build matrix

| Configuration | Default | Effect |
| --- | --- | --- |
| `KOI_BUILD_MODULES` | ON for MSVC, Clang 18+, GCC 14+; OFF for AppleClang | Builds the optional C++26 named-module interface (`src/koi/modules/*.ixx`) and `koi_module_tests`; nothing in the engine imports the modules, so the engine itself never depends on them |
| `KOI_STATIC_RUNTIME` | ON on Linux, OFF on Windows/macOS | Linux links `-static-libstdc++ -static-libgcc` by default; setting it ON on macOS is a fatal error; sanitizer builds should turn it off |
| `KOI_SANITIZE` | OFF | MSVC-only AddressSanitizer build: dynamic CRT, no `/RTC`, no LTO, `/fsanitize=address` |
| `KOI_TSAN` | OFF | Disables Release IPO for the Linux GCC ThreadSanitizer CI leg; sanitizer flags themselves come from the CI command line |
| `KOI_PEXT_ATTACKS` | ON | AVX2/AVX-512 cores use `_pext_u64` slider tables; the baseline core keeps the ray path; OFF builds every x86-64 variant with rays |
| `KOI_PGO` | `OFF` | MSVC-only profile-guided optimization; `generate` builds with `/GENPROFILE` + `/LTCG:PGI`, `use` rebuilds with `/USEPROFILE` + `/LTCG`; cannot be combined with `KOI_SANITIZE` or non-MSVC |
| `KOI_ENABLE_GPU_NNUE` | ON at configure time | Attempts to find `nvcc` and build the CUDA PTX payload; a build without `nvcc` is CPU-only, and the runtime `GpuNnue` option still defaults to `off` |
| `KOI_GPU_PTX_ARCHS` | `61;75;86;89;120` | Compute capabilities compiled to PTX, one embedded module each; unsupported architectures are filtered out by `nvcc --list-gpu-arch` |
| `KOI_BUILD_SHADOW_DIFF` | OFF | Builds `koi_shadow_diff_tests`, the native-vs-chess-library differential oracle |
| `KOI_TEST_TIMEOUT_SECONDS` | 600 | Default CTest timeout for tests without an explicit budget |
| `KOI_TIMEOUT_SCALE` | `auto` | Multiplier for explicit process-test and default timeouts: 3 for Debug, otherwise 1 |
| `KOI_PYTHON_TEST_TIMEOUT_SECONDS` | 90 | Timeout for tests labelled `python` |

The Release build enables IPO/LTO when the toolchain supports it and the build is
not a sanitizer, PGO, or TSan build (`CMakeLists.txt:204-214`). MSVC Release uses
`/O2 /Ob3`, non-MSVC Release uses `-O2 -O3`; MSVC links a 16 MiB stack reserve so
deep tactical recursion fits, and Linux/glibc raises the worker-thread default
stack to 16 MiB in `main.cpp`.

### 1.4 Executables and CPU-variant selection

| Executable | Build | Purpose |
| --- | --- | --- |
| `koi-engine` | Baseline `koi_core_generic` | The single GUI entry point. On x86-64 it starts the fastest sibling it can use and otherwise runs the baseline in place. On macOS arm64 it is the only engine binary. |
| `koi-engine-avx2` | x86-64 AVX2 Release core | Directly runnable; gates itself on AVX2 at startup and exits with code 3 if unsupported |
| `koi-engine-avx512` | x86-64 AVX-512 (F/BW/CD/DQ/VL) Release core | Directly runnable; gates itself on AVX-512 at startup and exits with code 3 if unsupported |
| `koi-bench` | AVX2 on x86-64; baseline on macOS arm64 | 64-position hard tactical suite and 128-position optional positional corpus; JSON profiles and speed reports |
| `koi-perft`, `koi-replay`, `koi-eval-features`, `koi-nnue-fixture` | Same policy as `koi-bench` | Rules/perft, replay, evaluator-feature dump, NNUE fixture generator |
| `koi-gpu-probe` | Same policy as `koi-bench` | CUDA driver/kernel probe; CPU-only builds report unavailable |

CPU variant selection lives in `src/koi/cpu_variant.cpp` and `src/koi/main.cpp`:

- The selector is compiled only into the baseline `koi-engine` (`KOI_CPU_SELECTOR=1`).
- `KOI_CPU_VARIANT` accepts `generic`, `avx2`, `avx512` case-insensitively; `auto`
  and unknown values mean no override (`parse_cpu_variant`).
- A forced variant is honored only when its sibling executable exists; the target
  still gates itself on the CPU.
- Automatic selection requires the CPU feature **and** fast PEXT
  (`cpu_supports_fast_pext`), because both optimized siblings use `_pext_u64`.
  Pre-Zen 3 AMD CPUs therefore stay on the baseline build even when AVX2 exists.
- On Windows the selector launches the sibling as a child in a kill-on-close job
  object and forwards its exit code; on Linux/macOS it `execv`s the sibling so the
  process image is replaced.
- A direct AVX2/AVX-512 launch checks the CPU before `main` and exits 3 with a
  clear message if unsupported. GCC/Clang variants also install a pre-main guard
  (`cpu_variant_guard.cpp`) so vectorized static initializers cannot execute
  unsupported instructions first.

Other environment variables relevant to operation:

| Variable | Meaning |
| --- | --- |
| `KOI_NNUE_PATH` | Explicit NNUE network path; wins over auto-discovery |
| `KOI_NNUE_AUTODETECT=0` | Disables boot-time NNUE discovery; `EvalFile` still works |
| `KOI_GPU_NNUE=0` | Forces GPU NNUE off; any other non-empty value forces `on` over the option, except that an explicit `off` from option or environment always wins |
| `KOI_GPU_PTX_ARCH=<arch>` | Forces one embedded PTX module (for example `75`) for diagnostics |
| `KOI_GPU_BATCH=<n>` | Test seam that can only lower the GPU batch request within the 256-position cap |

## 2. Architecture

### 2.1 Layers and ownership

| Layer | Owner / file | Contract |
| --- | --- | --- |
| Process entry point | `src/main.cpp` | Resolves the executable directory, boot-time NNUE path, evaluator, and starts the UCI controller. Evaluator selection never aborts the process. |
| UCI controller | `src/koi/uci_controller.cpp/.hpp` | Owns the current `GameState`, the worker/search handle, option state, book and Syzygy instances. It is the only layer that writes UCI protocol output. |
| Public search boundary | `SearchService`, `SearchHandle` in `src/koi/search_service.cpp/.hpp` | `SearchService` owns the shared evaluator, transposition table, and search lifecycle. `SearchHandle` exposes `stop()`, `wait()`, `running()`, and `request_ponderhit()`. |
| Request session | `detail::SearchSession` in `src/koi/detail/search_session.hpp` | Owns one immutable request snapshot (root, limits, options), identity (generation + root key + root FEN), cancellation flag, worker thread, and the exactly-once completion claim (`CompletionOnce`). |
| Search orchestration | `detail::SearchRunner` in `src/koi/detail/search_runner.cpp` | Backend dispatch, timing setup, root tablebase probe, iterative deepening, aspiration, serial/Lazy SMP/root-parallel drivers, short-search fallbacks, and result publication. |
| Recursive search state | `detail::SearchContext` in `src/koi/detail/search_context.cpp/.hpp` | Negamax and quiescence, move loop, pruning call sites, per-worker ordering tables, fixed-capacity `SearchStack`, evaluation context, budget, and statistics. |
| Transposition table | `TranspositionTable` in `src/koi/transposition_table.cpp/.hpp` | Physical storage, striped locks, hazard leases, generations, clearing, resizing, replacement, and mate-score normalization. Search accesses it only through `detail::SearchTableAccess`. |
| Rules authority | `Position` in `src/koi/position.cpp/.hpp` | Production authority for legality, move generation, position keys, rule state, reversible history, and make/unmake. |
| Compatibility facade | `GameState` in `src/koi/game_state.cpp/.hpp` | The stable API used by evaluator, search, and UCI. It keeps a private `CompatibilityMirror` (vendored chess-library board plus reversible shadow history) transactionally synchronized for Polyglot, legacy adapters, completion validation, and explicit differential diagnostics. |
| Feature cache | `detail::FeatureState` in `src/koi/detail/feature_state.cpp/.hpp` | Caches `PositionFeatures` derived from native `Position`; invalidated by make/unmake and keyed by native history depth. |
| Evaluator execution | `detail::EvaluationContext` in `src/koi/detail/evaluation_context.cpp/.hpp` | Selects a per-worker `EvaluatorWorker` (used by NNUE) or the evaluator-plus-mutex fallback. |
| NNUE | `src/koi/nnue.cpp/.hpp` | Versioned container loader/serializer, integer inference, incremental accumulators, AVX2/NEON/scalar paths, discovery, and evaluator selection. |
| Time | `TimeManager` in `src/koi/time_manager.cpp/.hpp` | Deadline, node limit, budgets, pacing, and ponderhit reconfiguration. |
| Optional assets | `OpeningBook`, `SyzygyTablebase` | Polyglot and Syzygy; both optional and inert without files. |
| Modules | `src/koi/modules/*.ixx` | Aggregate `koi` module with `types`, `position`, `eval`, `tablebase`, `search`, and `runtime` partitions exporting stable value contracts. Implementation headers are private and establish no ABI. |

### 2.2 Lifecycle contracts

1. `UciController` validates `position`/`go`, bumps its protocol generation, and
   snapshots the root into a `GameState`.
2. `SearchService::start` normalizes `SearchOptions` (threads, speed, MultiPV,
   overhead, slow mover, Elo) and creates a `SearchSession`. The session publishes
   the request identity and launches `SearchRunner::run` on its worker.
3. The runner detaches the compatibility mirror from its private root copy
   (`GameState::detach_mirror`), because search never reads the shadow board.
   Native `Position` remains authoritative.
4. The runner probes the root TT entry for timing context, generates legal root
   moves with metadata, applies `searchmoves`, advances the TT generation, and
   constructs the `TimeManager`.
5. Completed iterations publish through `SearchEventSink`. The UCI controller
   filters every PV to its legal prefix, records ponder hints, and validates the
   final candidate through `CompletionGate` (identity, native legality, shadow
   legality, searchmoves, PV, ponder legality).
6. Exactly one `bestmove` is emitted per `go`. A stale completion is dropped, a
   duplicate claim is ignored, an illegal candidate falls back to a legal root
   move, and a root with no common legal move answers `bestmove 0000` instead of
   terminating the engine.
7. `stop`, `quit`, EOF, a replacing `position`, a resource `setoption`, and
   `ucinewgame` cancel and join the worker. A running search keeps the evaluator
   snapshot it started with; `EvalFile` swaps the evaluator only for later
   searches.
8. `ponderhit` is consumed at the top of the next iteration; `TimeManager` is
   reconfigured in place and the elapsed clock restarts so the converted budget
   is fully available. A node-limited ponder may already have finished; in that
   case the controller starts a fresh bounded search so the move is still
   answered.

Three unrelated "generation" concepts are deliberately distinct
(`src/koi/search_types.hpp:205-236`): the controller's protocol staleness
counter, `SearchRequestIdentity` request identity (carries that counter plus root
key and FEN), and the transposition table's replacement epoch advanced once per
search by `new_generation()`.

## 3. Rules and move generation

### 3.1 Position representation

`Position` is a pimpl wrapper around a native state (`src/koi/position.cpp`). The
state contains:

- a 64-square mailbox (`Piece` = type + color), with no piece on empty squares;
- incrementally maintained piece bitboards (2 colors × 6 piece types) and
  combined occupancy;
- a 64-bit side-to-move flag, castling rights as a 4-bit mask
  (`K/Q/k/q` = `0x1/0x2/0x4/0x8`), and an en-passant target square
  (`Square::kInvalid` = none);
- a halfmove clock (native `std::uint16_t`, clamped at 65535) and a fullmove
  number;
- a Zobrist key and a reversible history of `UndoRecord` entries carrying the
  prior key, move, captured piece, castling state, en-passant square, clocks, and
  flags.

Moves are packed into 32 bits: 6 bits origin, 6 bits destination, 3 bits
promotion (`Move` in `src/koi/move.hpp`). `Move::no_move()` is `0xFFFFFFFF`.
Promotion is one of knight, bishop, rook, queen; pawn, king, and "none" are not
legal promotion targets.

`MoveMetadata` (`src/koi/game_state.hpp`) adds moving/captured piece, `MoveKind`,
a gives-check flag, optional SEE score, ordering score, the position key the
record was generated for, and an internal validation token. `MoveMetadataList` is
a fixed 256-entry, intentionally uninitialized container for the search hot path.

### 3.2 Legal move generation

- `Position::legal_moves()` returns a deterministic, coordinate-tied legal move
  vector. `legal_moves_into(span)` is the allocation-free form and returns the
  total count; `legal_tactical_moves_into` emits captures, en-passant, and
  promotions in the same relative order.
- Generation is pseudo-legal followed by a legality filter that accounts for
  checkers, absolute pins, king moves, and en-passant discovered checks. Castling
  additionally requires the king and rook rights, empty transit squares, and that
  the king does not start in, pass through, or land on an attacked square.
- En-passant captures are generated only when the target square is set and the
  capture is actually legal; the key includes the en-passant file only when such
  a legal capture exists.
- Promotions generate all four piece choices. `is_legal`, `is_capture`, and
  `move_facts` resolve board facts without generating a list. `has_legal_move()`
  early-exits.
- The theoretical maximum is 256 legal moves; the search stack, move lists, and
  ordering arrays are all sized to `kMaximumLegalMoves = 256`.

### 3.3 Check detection and make/unmake

- `in_check()` and `in_check(Color)` use attack tables built from the position's
  own mailbox/bitboards (`src/koi/detail/attack_tables.*`).
- `make_move` validates and applies a move transactionally; `make_generated_move`
  and `make_search_move` are faster paths for metadata produced by this position's
  generator and still verify native legality. A failed application rolls back.
- `make_null_move` flips the side and clears en-passant; null moves are
  search-only and never legal game history. Unmake restores the exact prior state from
  the `UndoRecord`, including repetition fingerprints and cached feature state.
- The compatibility mirror is updated on every public make/unmake. Search
  detaches it and performs mirror checks at search boundaries instead of every
  node (`GameState::detach_mirror`, `native_shadow_consistent`).

### 3.4 Draw and terminal-status model

`DrawStatus` (`src/koi/game_state.hpp:27-36`) separates optional claims from
automatic results:

| Status | Condition | Search/terminal treatment |
| --- | --- | --- |
| `none` | No draw rule applies | Normal play |
| `claimable_threefold` | Same position occurred at least 3 times in the reversible history, and the position is not checkmate | The side to move may claim; the engine still returns a legal move. Search treats a claim as an available option, not an automatic terminal. |
| `claimable_fifty_move` | Halfmove clock ≥ 100, not checkmate, and not under a suppressed null-move history | Claimable, same as above |
| `automatic_fivefold` | Same position occurred at least 5 times, not checkmate | Automatic terminal draw |
| `automatic_seventy_five_move` | Halfmove clock ≥ 150, not checkmate | Automatic terminal draw |
| `dead_position` | Insufficient material, or the one recognized locked-pawn-wall pattern | Automatic terminal draw |

Details (`src/koi/position.cpp`):

- `repetition_count()` returns 1 immediately when the nullable history is
  suppressed, when the halfmove clock is below 4, or when no earlier key
  matches. Otherwise it scans backward at most `halfmove` plies (unbounded when
  the clock is clamped at ≥ 255), stopping at a null-move record, and counts
  matching keys. A null-move branch therefore cannot create a repetition claim.
- The fifty-move rule is implemented as **claimable** at 100 halfmoves and
  **automatic** at 150 halfmoves. The 75-move rule takes precedence in
  `draw_status` before fivefold; fivefold before fifty-move claim; fifty-move
  claim before threefold claim.
- `is_dead_position()` is deliberately narrow: it recognizes insufficient
  material (no pawns/rooks/queens and at most one minor, or only same-colored
  bishops) and exactly one documented 14-piece locked pawn wall, with a legal
  en-passant capture as an exception.
- Claimable draws are never promoted to terminal by `is_terminal()`; automatic
  draws and dead positions are. Checkmate is evaluated with legal generation and
  takes precedence over draw clocks.

### 3.5 Position keys

`Position::position_key()` is a Zobrist key over exactly:

- every piece on its square;
- side to move;
- the 4-bit castling-rights mask;
- the en-passant file, and only when a legal en-passant capture currently exists.

It **does not** include the halfmove clock, fullmove number, repetition history,
or future repetition potential. `repetition_history_fingerprint()` is a separate
search-only key over reversible ancestor keys. `pawn_key()` covers only the two
pawn bitboards, and `polyglot_key()` is the compatibility-mirror key used by the
opening book. Search also mixes `fullmove_number` into its evaluation-cache key
because the classical evaluator reads that counter (`SearchContext::evaluate`).

## 4. Evaluation

### 4.1 Evaluator selection and snapshot rules

- `make_evaluator()` constructs the classical evaluator unless a network path is
  supplied. At boot `main.cpp` resolves `KOI_NNUE_PATH`, otherwise calls
  `discover_nnue_network` on the executable directory and then the working
  directory.
- A loaded network is wrapped by `NnueEvaluator` (and, if the GPU path is active,
  by `GpuNnueEvaluator`). The classical evaluator remains the fallback at every
  stage: boot failure, malformed file, rejected dimensions, evaluator
  installation failure, or GPU unavailability.
- `UCI EvalFile` with a non-empty value stops and joins the active search, loads
  the file relative to the executable directory, and installs it for subsequent
  searches. An empty `EvalFile` is a no-op that keeps the boot-time evaluator.
  A rejected file emits `info string EvalFile rejected: ...` and leaves the
  current evaluator usable.
- A running search never changes evaluator mid-search. `SearchService::start`
  captures the current evaluator; `set_evaluator` affects only later searches.

### 4.2 Classical evaluator

The default evaluator is `ClassicalEvaluator` with feature set
`koi-classical-v1` and parameter version
`classical-eval-v8-endgame-scaling` (`src/koi/evaluation_parameters.hpp`). It
builds a `PositionFeatures` snapshot and sums the terms exposed by
`EvaluationBreakdown` (`src/koi/classical_evaluator.hpp:12-31`):

| Term | Main inputs / weights |
| --- | --- |
| Material | P 100, N 320, B 330, R 500, Q 900 (`piece_values.hpp`) |
| Piece-square | Static tables in `evaluation_piece_squares.hpp` |
| Mobility | Weight 4; bishop weight 2; queen weight 1 |
| Pawn structure | Doubled 12, isolated 10, island 4, passed base 20 + 4/rank, connected 6, attacked 6, backward 8; passed-pawn phase/protection/blockade/king-support terms |
| Activity | Knight outpost 12, rook semi-open 10, rook open 20, seventh rank 15, bishop pair 30, trapped piece 12 |
| Development | Bonus 10, castling readiness 8, early queen penalty 14 (central 6), scaled by phase |
| Center control | Weight 7, phase offset 8 |
| Initiative | Pawn break 12, attacked-piece pressure 2, hanging piece 5 |
| King safety | Shield 14, open file 10, king-zone attack 8, attacker weight 6, phase offset 8 / divisor 32 |
| King activity | Weight 1; fades in above endgame threshold 12 |
| Passed pawn extras | Promotion weight 8, enemy-king penalty 2, king proximity 3, king support 12 |
| Tempo | 10 |
| Endgame scaling | Base 64 (unchanged); minor-only and opposite-bishop endings scale to 32 when phase ≤ 8 |

The evaluator returns a side-to-move-relative score and supports concurrent
evaluation. Its positional score range is deliberately narrower than an NNUE's;
several search margins are calibrated to that range (see comments in
`src/koi/detail/search_constants.hpp`).

### 4.3 NNUE containers and features

The loader accepts Koi NNUE containers with magic `KOI-NNUE`, quantization
`int16/int8`, a SHA-256 payload digest, and a 256 MiB maximum file size
(`src/koi/nnue.cpp`). Supported versions are 2, 3, 4, 5, 6, and 8; version 1 is
not accepted by `validate_manifest` even though legacy v1 constants remain in the
header.

| Version | Feature set | Inputs | Head | Notes |
| --- | --- | --- | --- | --- |
| 2 | `piece-square-king-pawn-v2` | 960 | 256 hidden → 32 bottleneck → 1 | Original perspective network; sparse input path |
| 3 | `piece-square-king-pawn-v2` | 960 | Same tensor layout as v2 | Adds explicit fixed-point right shifts (hidden, bottleneck, output) |
| 4 | `halfka-king-bucket-v1` | 9216 | even hidden 32..8192 → CReLU pair products → 8 piece-count buckets | 12 king buckets × 12 piece planes × 64 squares; reserved layer word must be 0 |
| 5 | `halfka-king-bucket-v1+threat-pairs-v1` | 36864 | Two-layer integer head: 8..128 L1 units → 8 buckets | Adds threat-pairs group and cross-perspective pair products |
| 6 | Same as v5 | 36864 | Adds an antisymmetric L1 difference channel | Extra `l1_units × hidden_units` int8 matrix; split diff shift |
| 8 | v5/v6 threat form, or halfka-only 9216 layout | 36864 or 9216 | Same math as v6 (or v4 for the halfka-only layout) | CPU loader treats it like v6/v4. The GPU kernel still accepts only v5 (see §4.4); the header comment that calls v8 the GPU container is stale |

Feature geometry (`src/koi/evaluation_features.hpp`):

- `halfka-king-bucket-v1`: 12 own-king buckets × 12 piece planes × 64 squares =
  **9216** inputs. Plane order is own pawn..king followed by opponent pawn..king,
  relative to the perspective.
- `threat-pairs-v1`: 2304 inputs per king bucket × 12 buckets = **27648**.
  Each attack relation yields one input; non-sliders index victim type and
  square, sliders also index attacker type and square.
- Combined v5 input: 9216 + 27648 = **36864**.
- Sparse feature capacities: 64 (v4), 128 (threat group), 160 (combined v5).

Inference:

- First layer is an int16 weight matrix over sparse binary inputs, accumulated
  into int32 sums. Clipped ReLU clamps to `[0, 127]`.
- v4 forms pair products from the two halves of the hidden activation and a
  linear head selected by piece-count bucket.
  `output_bucket_for_pieces()` is `min(7, (32 - min(32, pieces)) / 4)`.
- v5/v6 pair the two perspectives cross-wise (`own[i] * opp[i]`); v6 adds an
  own-minus-opp diff channel. The L1 stage is followed by an 8-bucket int8 head.
- Explicit shifts are stored for v3+ and capped at 20
  (`kKoiNnueMaximumShift`). v5 adds `l1_shift`; v6 adds `l1_diff_shift`, where
  zero selects the legacy combined head.
- The loader validates magic, version, feature string, layer dimensions,
  hidden-unit parity and range, L1 range, quantization, payload size, and SHA-256
  before the network is usable. A failed load returns a typed `NnueError`; the
  engine keeps the previous evaluator.

Incremental accumulator (`NnueWorker`):

- One worker per search context, so immutable weights are shared and mutable
  accumulators stay thread-local.
- v4+ networks keep two perspective accumulators in 132 slots (root plus deepest
  line). Every cached slot is keyed by position key; a skipped or failed
  notification causes a refresh, never a wrong score.
- v5+ additionally stores each perspective's sorted threat list and applies a
  sorted-list difference on make/unmake.
- The worker reports `incremental_make_count` and `incremental_fallback_count`
  for diagnostics.

SIMD paths (`src/koi/nnue.cpp`):

| Path | Availability | Scope |
| --- | --- | --- |
| AVX2 | x86-64 builds compiled with AVX2 **and** runtime `cpu_supports_avx2()` | Sparse/dense accumulator, v4 pair products and bucket dot product, v5 cross pair products and L1 stage |
| NEON | arm64 builds with `__ARM_NEON` | v4 CReLU clamp/pack only; arithmetic is identical to scalar |
| Scalar | Always compiled | Reference path and fallback; every vector path must agree exactly |

The AVX2 code is only compiled on x86-64; macOS arm64 uses the scalar network
path with the NEON clamp. An AVX2 result that would overflow is rejected and
recomputed scalar in the bucket-dot path so the vector path stays bit-identical.

### 4.4 GPU NNUE (experimental, off by default, scheduled for removal)

- The CUDA backend is optional, experimental, disabled at runtime by default, and
  scheduled for removal (`src/koi/gpu/*`, `src/koi/nnue.cpp`).
- `GpuNnue` accepts `off` (default), `auto`, and `on`. `auto` uses the GPU only
  when the build has a kernel, the driver loads, the network is compatible, and
  `Threads > 1`. `on` requests it and falls back to CPU with a diagnostic.
  `Threads=1` always uses the deterministic CPU path.
- Precedence: an explicit `off` from the option or `KOI_GPU_NNUE=0` always wins;
  otherwise a non-empty `KOI_GPU_NNUE` other than `0` forces `on`; otherwise the
  option decides.
- The kernel supports **v5 containers only**, with **hidden = 1536 and L1 = 32**
  exactly; v6/v8 and other widths are rejected and fall back to CPU
  (`gpu_nnue_service.cpp:102-128`).
- Build-time PTX modules cover sm_61, 75, 86, 89, and 120 by default. At runtime
  only `nvcuda.dll`/`libcuda.so.1` is resolved dynamically; the newest module the
  device can run is JITed. A GPU older than sm_61 stays on CPU.
- The GPU result is intended to be bit-exact with the CPU scalar evaluation. No
  speedup, strength, or Elo claim is made. One diagnostic line is emitted at the
  start of a search when NNUE is active, for example
  `info string GPU NNUE: on -> cuda (NVIDIA GeForce GTX 1060)`.

### 4.5 Automatic discovery and `EvalFile`

`discover_nnue_network(directory)` (`src/koi/nnue.cpp:2350-2393`) uses this order:

1. `<dir>/koi.nnue`
2. `<dir>/nnue/koi.nnue` (packaged layout)
3. the single `*.nnue` file in `<dir>` (ambiguous matches are rejected)
4. the single `*.nnue` file in `<dir>/nnue`

`KOI_NNUE_AUTODETECT=0` disables the scan. `KOI_NNUE_PATH` bypasses it entirely.
`EvalFile` is the runtime override; an empty value keeps the boot evaluator.
Missing, malformed, oversized, or rejected files leave the current evaluator
usable and produce one explanatory stderr line or UCI `info string`.

## 5. Search

### 5.1 Driver and iterative deepening

`SearchRunner::run` (`src/koi/detail/search_runner.cpp:2696`) selects one of
three drivers after root setup:

- **Serial driver**: `Threads=1` and `MultiPV=1`, forced-draw roots, roots with
  fewer than 2 legal moves, node-limited single-PV searches, and short tactical
  searches. Exclusive TT access is enabled for the session.
- **Lazy SMP**: `Threads>1`, `MultiPV=1`, no node limit, not a short tactical
  budget, not a forced draw, and at least 2 legal root moves. The main thread is
  the only publisher; helpers search the same root against the shared TT.
- **Root-parallel pool**: `MultiPV > 1` (including node-limited MultiPV) and the
  ultra-short non-checking parallel case. Root moves are scheduled as jobs across
  up to `min(Threads, legal moves)` workers, with exact shared-node accounting via
  `SearchBudget`. Forced-draw roots, single-PV node-limited searches, roots with
  fewer than 2 legal moves, and short tactical searches stay on the serial driver.

The serial loop:

1. Starts at depth 1 and increments to `min(64, limits.depth)`; infinite and
   ponder searches are unbounded. A ponder search with a depth limit ignores
   that limit for the ponder phase and deepens until stop/ponderhit.
2. Seeds an aspiration window around the previous proven score:
   `window = 50 + min(32, 2 * depth)`. It widens a failed side up to three times
   (`window * 2 + 16`, capped at `kInfinity / 4`), then falls back to a full
   `[-inf, +inf]` search. A selective root iteration does not seed the next
   window (`previous_score` is reset).
3. Calls `SearchContext::negamax` with the root move list, full check-extension
   budget, no excluded move, and the root-authority output flag.
4. Publishes only a complete root result: every root move has a recorded score
   and the PV starts with one of those moves. Otherwise the previous completed
   iteration is retained and the attempt only paces the next one.
5. Stops when the hard deadline fires, when the depth cap is reached, or when the
   `TimeManager` says the next iteration cannot fit (the estimate is twice the
   previous iteration's elapsed time).

PVS: the first root move is searched with the full window; later moves use a
null-window scout and a full re-search on a fail-high. The recursive move loop
tracks `pvs_searches` and `pvs_researches`; root counters are separate
(`root_pvs_*`). A failing reduced child is re-searched at full depth before it
can challenge alpha.

### 5.2 Transposition table

| Property | Value / behaviour |
| --- | --- |
| Entry | 24 bytes exactly (`static_assert`): 64-bit key, int score, move, int16 eval (`-32768` = none), int8 depth, uint8 generation, uint8 generation age, bound, occupied, PV flag |
| Cluster | 4 entries (96 bytes) |
| Locks | 64 striped `std::shared_mutex` locks; a hazard-pointer lease keeps storage alive across a concurrent resize; a session-scoped pin provides lock-free exclusive access for the serial driver |
| Probe | Returns the entry normalized to the current ply for mate scores |
| Store | Prefers empty slots, then replacement priority based on generation age, depth, bound, and deterministic key tie-break; does not overwrite a deeper entry or an equal-depth exact entry with a non-exact one |
| Generations | `new_generation()` advances once per search; `generation_age` is also the logical `Clear Hash` epoch, so clearing invalidates entries without sweeping memory |
| Mate normalization | Scores within `kMateThreshold = 99'000` of ±`kMateScore = 100'000` are adjusted by ply on store/probe |
| Hash size | 1..4096 MB, default 512; physical/commit memory caps can reduce the request and emit a diagnostic; the UCI engine starts with a 1 MB bootstrap table and materializes the configured size at `isready` |
| Hashfull | Approximate occupancy in permill (0..1000), sampled over a bounded slot set |

### 5.3 Method inventory and parameters

The table below is the live search policy. Constants live in
`src/koi/detail/search_constants.hpp` and `src/koi/detail/search_policy.hpp`
unless another file is named.

| Method | Behaviour and parameters |
| --- | --- |
| Iterative deepening | Depth 1..64; aspiration as above; `previous_iteration_elapsed * 2` fit check |
| PVS | Full-window first move, null-window scouts for the rest, full re-search on fail-high |
| Null-move pruning | Eligible when depth ≥ 3, not checked, `beta` finite, `beta - alpha <= 1`, non-pawn endgame, both sides have non-pawn material, phase ≥ 8, halfmove clock < 90, more than 8 pieces, and static eval above `beta + 96 - 13*depth - 47*improving`; `R = 7 + depth/3 + max(0, eval-beta)/256`; fail-high is verified before cutoff |
| Null-move verification | At `depth >= 16` and `R >= 3`, a no-null/no-TT confirmation search must reproduce the fail-high; below that the reduced probe itself must reach beta without a selective cutoff |
| Reverse futility | Depth 2..18, non-PV, not checked, static eval ≥ beta; margin `80 + 60*depth - 32*improving + 24*opponent_worsening` |
| Razoring | Depth 1 only, static eval + `120 * depth^2 <= alpha`; a qsearch upper bound is used and may return a selective score |
| ProbCut | Depth ≥ 3, non-PV cut node, static eval ≥ `beta - 160 - 16*depth`; margin `241 - 64*improving`; reduced depth `max(0, depth - (improving ? 5 : 3))`; qsearch then reduced regular confirmation; TT ProbCut additionally needs depth ≥ 4, a lower TT bound at `depth - 4`, and score ≥ `beta + 428` |
| Internal iterative reduction (IIR) | Depth ≥ 6, non-PV, not checked, no TT move, quiet and not repetition-sensitive: depth is reduced by 1 |
| True internal iterative deepening (IID) | PV, depth ≥ 6, no usable TT ordering move: a depth-2 null-window probe seeds the table; score is discarded |
| Singular extensions | Depth ≥ 6 (+1 if TT-PV), not a claimable draw, not an excluded-move search, not checked, not a pawn endgame, ply > 0, not repetition-sensitive, TT move with a lower/exact bound at depth ≥ `depth - 3`, score below mate threshold. Margin `(59 + 66*(tt_pv && !pv_node)) * depth / 63`; probe depth `max(1, (depth-1)/2)`. Ladder: +1 if excluded score < singular beta, +2 below `max(16, 64 + 32*pv - 16*quiet)`, +3 below `max(double+16, 144 + 48*pv - 24*quiet)`. Maximum 2 singular extensions per path |
| Multicut | At the singular probe, non-PV depth ≥ 8, excluded score ≥ beta: return as a selective bound |
| Negative singular extension | If alternatives are not singular and the TT score ≥ beta or the node is a cut node: `-3` plies |
| Check extension | Checked node, depth < 64, not a short timed root, extension budget remaining: +1 ply, maximum 2 per path |
| Quiet-forcing extension | A quiet non-checking move with a direct forcing target gets +1 ply (`quiet_move_is_forcing`); root forcing extensions are limited to depth-1/short-fallback diagnostics |
| LMR | Candidate: move number ≥ 2, depth ≥ 3, not checked, not giving check, no promotion, not TT, not killer, and (not capture or capture history > -8192). Base reduction 0 for captures, 1 for quiets, plus depth/move-number bands at (7,7), (11,14), (16,24), plus cut node, minus TT-PV, plus non-improving, minus PV, plus no TT move, plus next-cutoff-count >1/>2, minus prior fail-high, plus history/continuation < -512, plus capture history < -512. Clamped to `[0, full child depth]`; high history ≥ 128 is excluded; quiets that are quiet-forcing are not reduced. A reduced fail-high is re-searched at full depth |
| Quiet futility | Phase-rich non-PV quiet node, `static_eval + 80 + 60*depth <= alpha` |
| Child futility | Non-PV, not checked, not tactical, move number > 0, quiet, reduced depth < 12, `static_eval + 96 + 71*lmr_depth + (54 if eval > alpha) <= alpha` |
| Capture futility | Non-PV, not checked, non-repetition, capture, not TT, `lmr_depth < 8`, `static_eval + 234 + 247*lmr_depth + captured_value + 134*capture_history/1024 <= alpha` |
| SEE pruning | Capture with `alpha >= 0` and `see_score < -(177*depth + 34*capture_history/1024)`; qsearch removes captures below SEE `-74` and applies delta pruning (`best + captured_value + 100 < alpha`) |
| Killer moves | Two killers per ply (`kMaximumPly = 64`); ordered after TT and good captures |
| Counter moves | Counter-move table with confidence; a proven counter move requires confidence ≥ 49 (`7*7`) |
| Continuation history | 6 plies (`kSearchContinuationPlies = 6`) of parent move context, 16K entries per continuation |
| Quiet history | Side-to-move and pawn-key indexed, 64×64 move table, clamped to ±`kMaximumHistoryScore` (300,000 - 2); initial bias -586 |
| Capture history | 7×7×64×64 entries, initial -742, clamped to ±16,384 |
| Low-ply history | Plies 0..4 (`kLowPlyHistoryPlies = 5`) for quiet ordering |
| Correction history | Bounded correction by pawn structure, material signature (FNV-1a over piece counts), and side-to-move king square; entry limit 128, total limit 192; applied only to static eval, never to TT scores |
| Root move ordering | TT move, staged good captures/promotions, history-ranked quiets (checks, killers, proven counters), deferred losing captures, bad quiets; deterministic UCI-coordinate tie-break; previous completed best move is retained as a hint even after TT displacement |
| Root safety heuristics | Bounded confirmation/verification passes for shallow depth-1/depth-2/depth-3/depth-5 roots, emergency king-escape and short-clock fallbacks; these can replace a completed move only through the documented candidate sets and margins |
| Mate-distance pruning | `alpha = max(alpha, -100000 + ply)`, `beta = min(beta, 100000 - ply - 1)` after the extension decision |
| Node/time limits | Node limit checked through `SearchBudget` (serial local count or shared atomic reservation); time limit through `TimeManager` sampled once per 256 nodes |
| MultiPV | Up to 16 lines; root-parallel jobs per root move, per-move aspiration hints, ranked by score then deterministic move index, one `info` line per line with `multipv` |
| Lazy SMP | `Threads-1` helpers, each with its own position, ordering tables, and evaluator worker; shared TT and time; helpers publish depth/stats but never protocol output; a helper exception is recorded and stops deepening without killing the process |
| Root-parallel | `min(Threads, legal moves)` workers, per-move jobs, exact node accounting, used for `MultiPV > 1` and the ultra-short non-checking parallel case |
| Determinism | `Threads=1` is deterministic for a fixed position, options, and version. `Threads>1` is intentionally nondeterministic; validate legality, completion, and coherent lines instead of byte equality |

### 5.4 Important constants

| Constant | Value | Source |
| --- | --- | --- |
| `kInfinity` | 1,000,000 | `detail/search_constants.hpp` |
| `kMateScore` | 100,000 | same |
| `kMateThreshold` | 99,000 | same |
| `kMaximumSearchDepth` | 64 | same |
| `kMaximumQuiescenceDepth` | 16 | same |
| `kMaximumQuiescenceSafetyDepth` | 16 | same |
| `kMaximumCheckExtensionsPerPath` | 2 | same |
| `kMaximumSingularExtensionsPerPath` | 2 | same |
| `kMaximumQuiescenceCheckDepth` | 3 (2 for budgets ≤ 500 ms) | same / `SearchContext` constructor |
| `kQuiescenceFutilityMargin` | 200 | same |
| `kQuiescenceFutilityMoveLimit` | 2 | same |
| `kQuiescenceSeeThreshold` | -74 | same |
| `kRazorMarginPerDepthSquared` | 120 | same |
| `kRazorMaximumDepth` | 1 | same |
| `kInternalIterativeReductionMinimumDepth` | 6 | same |
| `kTrueInternalIterativeDeepeningMinimumDepth` | 6 | same |
| `kTranspositionProbCutMargin` | 428 | same |
| `kReverseFutilityMinimumDepth` / `MaximumDepth` | 2 / 18 | same |
| `kReverseFutilityBaseMargin` / `DepthMargin` / `ImprovingDiscount` / `WorseningSurcharge` | 80 / 60 / 32 / 24 | same |
| `kNegativeContinuationHistoryBase` | 4,136 | same |
| `kNullMoveStaticMargin` | 96 | same |
| `kNullMoveVerificationMinimumDepth` | 16 | same |
| `kTablebaseInteriorWinScore` / `LossScore` | ±90,000 | same |
| `kAspirationWindow` | 50 (+ min(32, 2·depth)) | `detail/search_runner.cpp:42,3054` |
| `kRootSelectiveDepth` / `Margin` / `Improvement` | 3 / 20 / 15 | `detail/search_runner.cpp:43-45` |
| `kRootShallowConfirmationDepth` / `Margin` / `ForcingMargin` | 2 / 32 / 200 | `detail/search_runner.cpp:52-54` |
| `kRootKingSafetySelectiveDepth` / `TieMargin` | 5 / 50 | `detail/search_runner.cpp:56-57` |
| `kMaximumMultiPv` | 16 | `detail/search_runner.cpp:89` |
| `kEvaluationCacheSize` | 64 × 1024 | `detail/search_context.hpp:166` |
| `kQSearchCacheSize` | 16 × 1024 | `detail/search_context.hpp:170` |
| `kClockPollInterval` | 256 nodes | `detail/search_context.hpp:175` |
| `kSearchContinuationPlies` | 6 | `detail/search_ordering_tables.hpp:13` |
| `kLowPlyHistoryPlies` | 5 | same |
| `PawnHistorySize` | 32 × 1024 | same |
| `ContinuationHistorySize` | 16 × 1024 | same |
| `CorrectionEntryLimit` / `TotalLimit` | 128 / 192 | same |
| TT entry size | 24 bytes | `transposition_table.hpp:76` |

## 6. Time management

### 6.1 Budget construction

`TimeManager::initialize` (`src/koi/time_manager.cpp:124-281`) applies these rules
in order. `Speed` is a percentage (1..100, default 100); `Slow Mover` is a
percentage (10..1000, default 100); `Move Overhead` is milliseconds (0..5000,
default 30); `Dynamic Overhead` switches the overhead to a clock-derived value.

1. **Unbounded searches** (`infinite`, `ponder`) get no deadline. Node limits
   still apply.
2. **Node limit**: stored as the exact requested `nodes` value and checked on
   every node reservation.
3. **`movetime`**: `scaled = movetime * Speed / 100`; then subtract the move
   overhead; then apply the safety margin `max(1, min(50, duration/20))`. The
   result is capped at the original request. `Slow Mover` does not extend a
   `movetime`. Soft and hard budgets are both this value.
4. **Clock search**:
   - `remaining` is the side-to-move clock, clamped to ≥ 0.
   - Dynamic overhead (if enabled) is `clamp(remaining / 50, 10, 100)` ms;
     otherwise the fixed `Move Overhead` is used.
   - Horizon: `movestogo` when supplied, otherwise **25** with a positive
     increment and **50** for sudden death.
   - Reserve: `min(750 ms, max(50 ms, 3% of remaining, 2 × overhead))`, then
     `min(reserve, remaining)`.
   - Usable: `remaining - reserve`.
   - Emergency pacing starts when `remaining <= 3000 ms`.
   - Base allocation: `usable / horizon`.
   - Increment credit: `min(3/4 × increment, 1/4 × usable)`.
   - Raw soft: `base + increment_credit`.
   - Raw hard: `hard_units × base + 1.5 × increment_credit`, where
     `hard_units = 3` with an increment or at ≤ 3000 ms remaining, otherwise 2.
   - Budgets are scaled by `Slow Mover`, then by `Speed`, and capped at
     `usable`; soft is capped at hard.
   - Emergency taper: horizon `max(10, horizon / 2)`, emergency base
     `usable / horizon`; soft becomes `min(soft, emergency_base + credit)`; the
     hard extension is `max(5 ms, emergency_base / 2)` and is suppressed for a
     non-hard position; hard is raised to at least soft.
   - Flag-safety caps are applied last: safety margin
     `max(2 × overhead + 15 ms, min(50 ms, remaining / 20))`; hard is capped at
     `usable - safety` and at `max(5 ms, usable / 4)`; hard has a 1 ms floor and
     soft is capped at hard.

The hard deadline is fixed when the search starts. Iteration evidence can pace
the search but cannot push it past the cap.

### 6.2 Pacing and hardness

- `initial_hardness` scores the root TT/check/move-count context from 0 to 100;
  a score ≥ 35 marks a hard position. Missing/non-exact/shallow TT entries, old
  generations, no TT move, check, many forcing moves, and ≥ 30 legal moves add
  points; a fresh exact deep TT hit subtracts 25.
- `observe_iteration` treats a best-move change, PV change, ≥ 75 cp score swing,
  or aspiration re-search as hard evidence. Stable means no change and |Δ| ≤ 25.
- A hard position may exceed the soft budget only while the remaining usable
  clock is at least 4× the hard budget (`hard_extension_affordable`). Near the
  end the soft budget wins.
- `should_stop_after_iteration`: stops at the hard deadline; in emergency pacing
  stops at soft when the extension is unaffordable or the position is not hard;
  in clock mode stops at soft after at least 2 stable observations when the
  extension is unaffordable.
- `should_start_next_iteration`: refuses a new iteration when projected 2× the
  previous iteration would cross the hard deadline, and in emergency pacing at
  soft.
- `ponderhit` calls `reconfigure`: limits/root context are replaced, the start
  time resets, and the new budget is fully available.

### 6.3 Limit interactions

- Explicit `go depth N` and `go nodes N` without a clock or `movetime` are not
  given an artificial deadline.
- When a depth or node limit is combined with a clock or `movetime`, the explicit
  limit remains an upper bound and the time limit a deadline; whichever is
  reached first stops the search.
- A bare `go`, or one with only the non-moving side's clock, uses a 250 ms
  fallback scaled by `Speed` (`kBareGoFallback`).
- A depth limit is clamped to 64 in the runner; `go mate N` is capped at 64 and
  converted to `2N - 1` plies; `go perft N` is capped at 10.

## 7. UCI surface

### 7.1 Handshake and options

`uci` writes `id name Koi`, `id author Koi contributors`, then every
advertised option in the fixed `kUciOptions` order, then `uciok`
(`src/koi/uci_controller.cpp:182-248, 1631-1664`). `Debug` and `DebugFile` are
settable but intentionally not advertised, so 27 of the 29 entries are visible.

| Option | Type | Default | Range | Semantics |
| --- | --- | --- | --- | --- |
| `RandomSeed` | spin | 0 | 0..2147483647 | Seeds book selection only; 0 is deterministic per position. Search itself never uses it |
| `Hash` | spin | 512 | 1..4096 MB | Shared transposition table size; resizing stops and joins the active search; physical/commit caps may reduce it with a diagnostic |
| `Threads` | spin | 1 | 1..min(64, hardware concurrency) | Worker count; 1 is deterministic; >1 uses Lazy SMP and is nondeterministic |
| `Speed` | spin | 100 | 1..100 | Scales movetime and clock budgets only; explicit depth/node searches are unchanged |
| `UCI_AnalyseMode` | check | false | true/false | Analysis mode; bypasses book selection and affects root tablebase eligibility |
| `MultiPV` | spin | 1 | 1..16 | Number of published PV lines; >1 uses the root-parallel path |
| `Ponder` | check | false | true/false | Informational for the GUI; the engine always appends a known ponder move to `bestmove` |
| `OwnBook` | check | false | true/false | Enables the Polyglot book; once set explicitly it also disables auto-detection |
| `BookFile` | string | `book.bin` | path | Relative paths resolve beside the executable; empty values are ignored |
| `BookDepth` | spin | 16 | 0..40 | Maximum root ply for book use; 0 means unlimited |
| `BookRandom` | check | false | true/false | Weighted random selection instead of the highest-weight legal move |
| `BookSafety` | check | true | true/false | Shallow forcing-material probe before accepting a book move |
| `BookSafetyDepth` | spin | 2 | 0..3 | Safety probe horizon; 0 disables the probe while retaining legal-move filtering |
| `Clear Hash` | button | — | — | Stops and joins the active search, then logically invalidates the table |
| `UCI_ShowWDL` | check | false | true/false | Appends heuristic `wdl W D L` to scored lines; Syzygy root results use exact WDL |
| `Move Overhead` | spin | 30 | 0..5000 ms | Safety margin subtracted from clock/movetime budgets |
| `Dynamic Overhead` | check | false | true/false | Derives overhead from the clock: 2% of remaining, clamped to 10..100 ms |
| `Slow Mover` | spin | 100 | 10..1000 | Scales clock-derived budgets; not applied to `movetime` |
| `Contempt` | spin | 0 | 0..1000 cp | Draw avoidance: a draw is worth `-Contempt` to the root side and `+Contempt` to the opponent; 0 keeps neutral draws |
| `StrengthMode` | check | false | true/false | Accepted but currently neutral; no strength reduction is wired to it |
| `SyzygyPath` | string | empty | path/list | Directory or path list of `.rtbw`/`.rtbz` files; relative single paths resolve beside the executable; empty disables |
| `SyzygyProbeDepth` | spin | 1 | 1..100 | Minimum search depth for the root probe |
| `SyzygyProbeLimit` | spin | 7 | 0..7 | Maximum pieces probed |
| `SyzygyInteriorDepth` | spin | 0 | 0..100 | Experimental interior WDL cutoff depth; 0 disables |
| `Syzygy50MoveRule` | check | true | true/false | Honors the 50-move rule in tablebase WDL results and requires a zero halfmove clock for cutoffs |
| `EvalFile` | string | empty | path | Loads a Koi NNUE network for subsequent searches; empty keeps the boot evaluator; rejected files fall back |
| `GpuNnue` | combo | `off` | `auto`, `on`, `off` | Experimental GPU policy; `off` never uses the GPU; see §4.4 |
| `Debug` (hidden) | check | false | true/false | Enables the debug log; changing it stops and joins the active search |
| `DebugFile` (hidden) | string | `koi-debug.log` | path | Log path; relative paths resolve beside the executable; rotates at 8 MiB with 3 backups |

Rules for `setoption`:

- Unknown names are ignored. Unknown values for known options are ignored without
  disturbing a live search. Re-sending the current value is a no-op, including
  for resource options that would otherwise stop the search.
- A changed option that alters search resources or policy stops and suppresses the
  active search before the new value is committed, so the next `go` uses a coherent
  snapshot. This includes `Hash`, `Threads`, `Speed`, `MultiPV`, `Slow Mover`,
  `Move Overhead`, `Contempt`, `OwnBook`, book options, `Syzygy*`, `EvalFile`,
  `GpuNnue`, `Debug`, and `StrengthMode`. `Clear Hash` and `ucinewgame` also stop
  and join first.
- `Threads` is clamped by `maximum_search_threads()` = `max(1, min(64,
  hardware_concurrency))`; an unavailable hardware count is treated as 1.
- `Hash` requests outside 1..4096 MB are ignored. A successful resize may be
  `applied`, `reduced`, `unchanged`, or `disabled`; failures keep the previous
  table and report `info string hash resize failed; previous table preserved`.
- `SyzygyPath` rebuild emits `info string Syzygy: <n>-man tables at <path>` or
  `info string Syzygy: no usable tables at <path>`.
- `EvalFile` with a non-empty value reports `info string NNUE enabled from <path>`
  or `info string EvalFile rejected: <reason>`.
- `Debug` can also be toggled with the standard `debug on` / `debug off` command.
  Debug output never goes to UCI stdout or normal stderr.

### 7.2 Command handling

| Command | Contract |
| --- | --- |
| `uci` | Emits the identity, option declarations, and `uciok`. |
| `isready` | Emits `readyok` immediately, including during a search. While idle it also materializes the configured hash size (the engine starts with a 1 MB bootstrap table so a large default does not consume a fast game's clock). |
| `ucinewgame` | Stops and joins the active search, clears the hash, resets the position to startpos, logs the reset. |
| `position [startpos | fen <6 fields>] [moves ...]` | Replaces the root. The old search is cancelled with generation suppression before parsing. Malformed FEN/position/move emits `info string invalid ...` and writes `UCI position error: ...` to stderr; the prior position is retained on a failed parse. Moves are applied only if legal. |
| `go` | Parses `depth`, `nodes`, `mate`, `perft`, `movetime`, `wtime`, `btime`, `winc`, `binc`, `movestogo`, `infinite`, `ponder`, and `searchmoves`. Malformed values are ignored. Bare or asymmetric clock commands fall back to 250 ms. |
| `stop` | Cancels and joins the active search and emits exactly one final legal `bestmove` with a `ponder` suffix when the completed PV has a reply. |
| `ponderhit` | Converts a running ponder search in place and re-arms timing; if no ponder search is running, starts a bounded search of the current position. |
| `quit` | Sets shutting-down, cancels/joins, and returns 0 without late protocol output. EOF follows the same cancellation path. |
| `debug on/off` | Toggles the debug log (stops/joins if changed). |
| Unknown command | Ignored. An exception during command handling is converted to `info string command failed: ...` and the loop continues. |

Behaviour details:

- **Exactly-once `bestmove`**: `CompletionGate` validates generation, root key,
  root FEN, native legality, shadow legality, `searchmoves`, PV legality, and
  ponder legality. Stale results are suppressed; an illegal result is replaced by
  a legal root move; a duplicate `CompletionOnce` claim is ignored. A nonterminal
  root with no common legal move emits an explanatory `info string` and
  `bestmove 0000`.
- **Terminal root**: no legal moves returns `bestmove 0000`.
- **`searchmoves`**: filters the root move list to the intersection of legal
  moves and the supplied coordinates. It bypasses the book and disables the
  ponder-move suffix.
- **`go mate N`**: searches to at most `2N - 1` plies; N is accepted in 1..64.
- **`go perft N`**: runs a cancellable perft outside the protocol loop, emits
  `info string <move>: <nodes>` for each root move plus
  `info string Nodes searched: <total>`, and emits **no** `bestmove`. N is
  accepted in 1..10.
- **`go infinite`**: no deadline; only `stop`/`quit`/EOF ends it.
- **Ponder**: `go ponder` never publishes `bestmove` on its own. A depth limit
  during pondering is treated as a target, not a cap. After `ponderhit` the
  search keeps its accumulated TT work.
- **WDL**: with `UCI_ShowWDL=true`, scored lines append a deterministic triplet.
  `mate` scores map to 1000/0/0 or 0/0/1000; ordinary scores use the heuristic
  `draw = max(0, 300 - |cp|/4)`, with the remaining mass split by score direction.
  A selective iteration with no proven score omits both score and WDL.
- **Depth/score reporting**: completed iterations emit
  `info depth seldepth multipv [currmove] score ... nodes nps hashfull time pv ...`.
  Interim lines may report the in-flight root move as a `currmove` token;
  `SearchInfo::currmovenumber` is populated by the runner but the UCI writer does
  not emit a `currmovenumber` token (see the inconsistency note in §10). A
  selective root iteration republishes the most recent proven
  score at the current depth while keeping its own PV; before any proven score it
  omits the score.
- **Ponder suffix**: `bestmove` includes `ponder <reply>` whenever the completed
  principal variation contains a reply, independent of the `Ponder` option.
- **No `UCI_LimitStrength` / `UCI_Elo`**: Koi does not advertise or implement
  them. A GUI that sends them gets normal full-strength play.
- **`Contempt`**: affects draw valuation inside search, not the decision to return
  a legal move. It does not change the UCI draw claim model.

## 8. Optional assets

### 8.1 Opening book

- Format: Polyglot `.bin` (`opening_book.cpp`). Records are 16-byte big-endian
  entries; moves are decoded into legal Koi moves; castling is translated from
  king-to-rook encodings.
- Auto-detection: a file beside the executable is used automatically when
  `OwnBook` has never been set explicitly. On first use the engine emits
  `info string book detected: <path> (OwnBook not set, using it)`. Setting
  `OwnBook` explicitly (true or false) disables auto-detection.
- `BookFile` resolves relative paths beside the executable. Missing, unreadable,
  malformed, allocation-failed, or larger than 16 MiB books are treated as
  unusable; search proceeds normally. A book must be at least 16 bytes to be
  considered present.
- `BookDepth` limits book use to root plies 0..N-1; 0 means unlimited.
- `BookRandom=false` selects the highest-weight legal move, with a deterministic
  coordinate tie-break. `BookRandom=true` performs weighted random selection;
  `RandomSeed` (0 default) makes the stream repeatable.
- `BookSafety=true` runs a shallow forcing-material probe with a 250 cp loss
  threshold and a 512-node budget before accepting a book move; a move that hangs
  material is rejected and search chooses instead. `BookSafetyDepth` 0..3 sets
  the probe horizon and 0 disables the probe while keeping legal filtering.
- Book selection is bypassed for analysis mode, `MultiPV > 1`, `go infinite`,
  `go ponder`, and any `searchmoves` command. On a hit the engine writes
  `info string book move <uci> depth <ply>` and one legal `bestmove`.
- Release packages may contain a CC0 Polyglot book at `book/koi.bin` with
  `book/COPYING.txt` and a SHA-256 sidecar; users copy it beside the binary as
  `book.bin`. No book is required, and unlicensed books must not be redistributed.

### 8.2 Syzygy tablebases

- `SyzygyPath` accepts a directory or platform path list. A relative single path
  resolves beside the executable. An absent or unusable path disables probing and
  leaves ordinary search intact.
- `SyzygyProbeLimit` 0..7 (default 7) and `SyzygyProbeDepth` 1..100 (default 1)
  gate table availability and the minimum search depth for the root probe.
- `Syzygy50MoveRule` honors the halfmove clock: WDL probes require a zero
  halfmove clock when enabled, and the root DTZ/WDL selection follows Fathom's
  rule-aware classification.
- Root probing is used only for eligible single-PV play searches; analysis mode,
  `MultiPV > 1`, ponder, `go infinite`, and `searchmoves` keep their normal
  search paths. Root probing can be disabled by `SyzygyProbeDepth` and by
  claimable/forced draw roots.
- `SyzygyInteriorDepth` 0..100 (default 0) enables experimental interior WDL
  cutoffs at or below the given remaining depth. Only decisive results (win or
  loss) apply; draw, cursed-win, and blessed-loss results never override search,
  and a cutoff still requires a zero halfmove clock when the 50-move rule is on.
  Interior probing is strength-unvalidated and inert unless a table or a test
  probe hook is installed.
- Score conversion (`syzygy_tablebase.cpp`):

| Result | Interior score | Root score | WDL permill |
| --- | --- | --- | --- |
| Win | +100,000 - 1, mate +1 | +90,000, no mate | 1000/0/0 |
| Loss | -100,000 + 1, mate -1 | -90,000, no mate | 0/0/1000 |
| Cursed win | +1, no mate | +1, no mate | 0/1000/0 |
| Blessed loss | -1, no mate | -1, no mate | 0/1000/0 |
| Draw | 0 | 0 | 0/1000/0 |

Successful probes may add `tbhits` to `info` lines. Root wins are reported with a
decisive centipawn score and exact WDL, never as `mate 1`. No tablebase data is
bundled or required.

### 8.3 NNUE assets

See §4.5. `KOI_NNUE_PATH` is an explicit boot path; `KOI_NNUE_AUTODETECT=0`
disables discovery; `EvalFile` is the runtime override. Networks are local
assets and are not committed to the repository.

## 9. Testing and verification

### 9.1 CTest suites and labels

The test layout is `tests/unit`, `tests/integration`, `tests/python`, `tests/data`,
and `tests/support`. Every C++ harness uses `tests/support/koi_test_support.hpp`
and reports `PASS/FAIL/XFAIL/XPASS/SKIP/XFAIL-UNSEEN` plus a summary line. Selection
is available through `--filter=<substring>`, `--shard=<i>/<n>`, `KOI_TEST_FILTER`,
and `KOI_TEST_SHARD`.

| Suite group | Examples |
| --- | --- |
| Rules and state | `koi_core_tests`, `koi_rules_tests`, `native_rule_state_tests`, `perft_tests`, `koi_shadow_diff_tests` |
| Evaluation | `evaluation_boundary_tests`, `evaluation_architecture_tests`, `nnue_boundary_tests`, `classical_evaluator_tests`, `evaluation_features_tests` |
| Search | `koi_search_tests_1of4`..`4of4`, `search_ordering_tests`, `search_architecture_tests`, `search_policy_tests`, `search_runtime_tests`, `search_service_tests`, `static_exchange_tests`, `time_manager_tests`, `transposition_table_tests`, `completion_gate_tests`, `koi_strength_tests`, `koi_soak_tests` |
| Runtime | `koi_cpu_features_tests`, `koi_cpu_variant_tests`, `gpu_nnue_tests`, `koi_module_tests`, `syzygy_tablebase_tests`, `opening_book_tests`, `uci_controller_tests` |
| Process (PowerShell) | `koi_engine_process`, `koi_engine_perft_responsiveness`, `koi_engine_variant_process`, `koi_engine_en_croissant_process`, `koi_engine_time_safety_process`, `koi_benchmark_process`, `koi_uci_match_process`, `koi_uci_match_clock`, `koi_stockfish_strength_option`, `cutechess_stability_diagnostics`, `cutechess_stability_smoke`, `hash_memory_stability`, `windows_ci_configuration`, `install_book_script`, `package_release_layout` |
| Python | `elo_*_python`, `release_*_python`, `strength_*_python`, `nnue_*_python`, `koi_*_python`, `measurement_*_python`, `bullet_*_python`, `tune_*_python` |

Labels: `unit`, `evaluation`, `search`, `runtime`, `rules`, `integration`,
`tools`, `process`, `uci`, `matches`, `packaging`, `python`, `heavy`. `ctest -L
unit -LE heavy` is the fast unit set. Search shards, `koi_strength_tests`,
`koi_engine_time_safety_process`, `koi_benchmark_process`, and
`cutechess_stability_smoke` are heavy; some declare `PROCESSORS 2` and
`koi_engine_process`/`koi_engine_perft_responsiveness` declare `RUN_SERIAL`.

The exact registered count depends on optional dependencies (`pwsh`, Python 3,
Cute Chess, `KOI_BUILD_SHADOW_DIFF`, modules). `tests/README.md` records 81 tests
for the Windows Release + Python configuration, while `docs/USER_GUIDE.md` still
mentions 67; treat `ctest -N` on the actual build as authoritative.

### 9.2 Sanitizer and differential legs

| Leg | Configuration | Scope / gap |
| --- | --- | --- |
| Windows ASan | Debug, `KOI_SANITIZE=ON`, MSVC `/fsanitize=address`, dynamic CRT, no `/RTC`, no LTO | Fast subset (`-LE heavy`). ASan is MSVC-only; Linux ASan and macOS sanitizer builds are not wired |
| Linux TSan | GCC 14, `-fsanitize=thread -fno-omit-frame-pointer -g`, `KOI_TSAN=ON`, static runtime off, modules off | Concurrency suites: TT, search service, controller, time manager, search shards; deadlock detection is disabled due to TSan's fixed detector table |
| Shadow diff | `KOI_BUILD_SHADOW_DIFF=ON` builds `koi_shadow_diff_tests` | Compares native `Position` legality, keys, rule state, and FEN against the vendored chess-library mirror; also enabled at explicit diagnostics boundaries and completion validation |
| GPU parity | `gpu_nnue_tests`, `koi_gpu_probe` | Bit-exact CPU/GPU parity plus the PTX variant table; parity cases skip without a CUDA device |
| Windows CI | Release full suite, Debug smoke (`-LE heavy`), ASan, shadow diff, package | Five independent jobs |
| Linux CI | GCC 14, Clang 18, modules-off, flake repetitions, Ubuntu 22.04 tarball, TSan | Full CTest suite per compiler; the tarball leg runs bounded UCI smokes with automatic and `KOI_CPU_VARIANT=generic` |
| macOS CI | Apple Silicon arm64 with Homebrew LLVM, `KOI_BUILD_MODULES=OFF`, GPU off, static runtime off; package leg | Unit/heavy/process/Python groups; x86-only AVX-512 and variant-process tests omitted |

Determinism rules: `Threads=1` is the deterministic configuration; threaded tests
assert legality, coverage, completed depth, ranked MultiPV lines, and coherent
bestmove/PV rather than byte equality. Timing-sensitive cases use the retry
mechanism (`KOI_TEST_RETRIES`); known-failure and intermittent registries are
empty for the 1.1.4 gate.

### 9.3 Benchmark and verification tools

| Tool | Purpose |
| --- | --- |
| `koi-bench` | 64-position fixed-depth tactical hard gate by default; `--optional` selects the 128-position positional corpus; `--threads`, `--speed`, `--timed`, `--warm-hash`, `--nodes`, `--warmup`, `--repeat`, `--depth`, `--depth-sweep`, `--fen-file`, `--nnue`, `--profile-json`, `--report` |
| `koi-perft` | Legal-node count from startpos at a non-negative depth |
| `koi-replay` | Replays coordinate moves from `startpos`/`fen` without exposing the vendored library; reports `legal`, `result`, `termination`, final FEN |
| `koi-eval-features` | Dump evaluator features; used by the classical tuner round trip |
| `koi-nnue-fixture` | Generates NNUE fixtures for boundary/parity tests |
| `koi-gpu-probe` | CUDA driver/kernel/device report; CPU-only builds report unavailable |
| `tools/build/speed_gate.ps1` | Repeated `koi-bench --report` baseline/candidate comparison; defaults 5 runs, 2% total NPS regression, 2% row regression, rows under 20 ms excluded as timer-quantized |
| `tools/build/performance_gate.ps1` | Older/alternate NPS comparison harness (threads 2 by default) |
| `tools/build/release_verify.ps1` | Fresh Debug and Release configure/build, full CTest/process checks, Threads 1/2/4 tactical gates, benchmark/UCI/replay/En Croissant-style artifacts under `artifacts/verification` |
| `tools/stability/uci_match.ps1` | Color-aware match harness; writes `koi-uci-match-v2` JSON + PGN with per-ply provenance |
| `tools/stability/sprt_compare.ps1` | Candidate-vs-baseline SPRT wrapper; exit 0 accept, 1 reject, 2 more games |
| `tools/measurement/elo_estimate.py` | Local anchor-ladder estimate with provenance checks |
| `tools/build/package_release.ps1` | Builds the package, manifest, archive, and checksums |

`koi-bench` default output is deterministic and CI-shaped; `--timed` adds
`elapsed_ms` and NPS, and untimed profiles report NPS as zero. Speed gates are
measurement commands, never CTest thresholds.

### 9.4 Release process

`tools/build/package_release.ps1`:

1. Requires a Release build directory containing the platform-required
   executables (`koi-engine`, `koi-bench`, `koi-replay`, `koi-perft`; plus
   AVX2/AVX-512 siblings on Windows/Linux).
2. Copies README, LICENSE, `licenses/chess-library-MIT.txt`,
   `licenses/fathom-MIT.txt`, and writes `INSTALL.txt`.
3. Optionally includes `nnue/koi.nnue` + `.sha256` + metadata and
   `book/koi.bin` + `.sha256` + `book/COPYING.txt`.
4. Writes `package.json` (`koi-engine-package-v1`) with per-file size/SHA-256,
   book/tablebase/NNUE inclusion flags, and provenance (`GITHUB_SHA` or
   `git rev-parse HEAD`, dirty flag computed with `core.autocrlf=false`).
5. Creates `.zip` on Windows or `.tar.gz` elsewhere and a `.sha256` sidecar.

No tablebase data is packaged. Version strings in the packaging script and
manifest are still `1.1.4` on this tree. Release titles use a readable product
name and version while the tag is the version alone.

## 10. Known limitations, experimental items, and deferred work

- **GPU NNUE is experimental, off by default, and scheduled for removal.** It is
  CUDA-only, unavailable on macOS, supports only v5 containers with hidden 1536
  and L1 32, and makes no speed or strength claim.
- **Threat-pair (`threat-pairs-v1`) networks were rejected.** The 36864-input v5
  form lost its matches (33.75% against the official net) and its export
  saturates the L1 head; `--arch v8` writes the halfka-only layout instead.
- **Wide networks were rejected at equal time.** A 1024-wide v8 candidate scored
  48.75-49.00% against the 512-wide official net at equal nodes but only 50.78%
  against classical at equal time, where the official net scores 62.50%.
- **Node-limited matches cannot promote a network on their own.** Equal-node
  results can hide an evaluation-cost regression; the equal-time gate is required.
- **No strength limiting.** `UCI_LimitStrength` and `UCI_Elo` are not advertised
  or implemented; `StrengthMode` is accepted but neutral.
- **No variants.** Standard chess only; Chess960 and other variants are out of
  scope.
- **`Threads>1` is nondeterministic.** Lazy SMP shares the TT and time budget; only
  `Threads=1` guarantees byte-identical search results for a fixed version.
- **Node-limited searches do not use Lazy SMP.** Single-PV node-limited searches
  use the serial driver; node-limited MultiPV searches use the root-parallel
  driver, both with exact node accounting.
- **Interior Syzygy probing is experimental and unvalidated.** It is off by
  default, only decisive WDL results apply, and no tablebase assets are bundled.
- **LMR is rule-based, not a precomputed reduction table.** Reductions come from
  the move-number/depth bands and adjustment classes in
  `SearchPolicy::dynamic_late_move`; any documentation that describes a "LMR
  table" is inaccurate for this tree.
- **Platform feature gaps:** macOS arm64 ships no AVX2/AVX-512 siblings, uses
  scalar NNUE (NEON only vectorizes the v4 CReLU clamp), disables named modules in
  the documented build, cannot use the static-runtime option, and has no GPU
  path. Windows requires MSVC; Linux requires x86-64.
- **Sanitizer coverage is partial:** ASan is MSVC-only and runs the fast subset;
  TSan exists only on Linux; macOS has no sanitizer leg.
- **PGO is MSVC-only.** `KOI_PGO` rejects non-MSVC configurations.
- **No version in the UCI handshake.** Build identity is only in benchmark JSON
  and package provenance; a GUI cannot read the engine version from `uci`.
- **NNUE v1 containers are not accepted** by the current loader (versions 2, 3,
  4, 5, 6, and 8 are).
- **Documentation/test-count drift:** `tests/README.md` and `docs/USER_GUIDE.md`
  disagree on the registered test count; `ctest -N` is authoritative.
- **Stale source comments:** `src/koi/cpu_variant.hpp` still says the selector is
  "Windows only" although Linux/macOS `execv` support exists,
  `src/koi/gpu/nnue_gpu_evaluator.hpp` describes the GPU option default as
  `auto` although the UCI default is `off`, and `src/koi/nnue.hpp` calls the
  version-8 container "the only container a GPU backend accepts" while
  `gpu_nnue_service.cpp` accepts version 5 only. The code and the UCI option
  table are authoritative.
- **Interim `currmovenumber`:** the v1.1.4 release notes say interim `info`
  lines report a one-based `currmovenumber`, but `write_search_info` emits only
  `currmove`. `SearchInfo::currmovenumber` is set to 1 by the runner and is not
  printed. The En Croissant transcript grammar accepts an optional
  `currmovenumber`, so both forms are protocol-valid.
- **GPU NNUE and CPU fallback:** an unsupported GPU network or missing driver
  leaves the CPU NNUE or classical evaluator active; the engine never fails to
  start because of the GPU path.
