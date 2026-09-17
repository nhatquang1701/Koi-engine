# Strength program (2026-09-16)

A 24-hour autonomous strength program for Koi, executed on top of the UCI
controller improvement (`db1ff43`). The goal was a very large overall step:
speed up the hot path, build a real NNUE pipeline end to end, and record every
result honestly.

## Global constraints

- Windows x64, MSVC only; build through the repository CMake/Ninja trees.
- stdout stays protocol clean with exactly one `bestmove` per search.
- The 64-position tactical gate must stay 64/64; perft and shadow-diff parity
  are the rules oracle.
- Exact-value search tests may only change with shadow-diff/perft evidence.
- Elo, CPL, and NPS are reports, not CI thresholds (repository policy).
- Commit at every phase boundary with a descriptive message.

## Phase A - measurement

- [x] Capture `koi-bench --timed` T1/T4 baselines under
      `artifacts/verification/2026-09-16-strength-program/`.
- [x] Add a raw stdin/stdout NPS probe for deep single-thread measurements
      (the shallow 64-position suite is dominated by per-position startup and
      says nothing about midgame throughput).
- [x] Build a cutechess-cli A/B harness through a space-free directory junction
      (`C:\koi`) with `key=value` engine specs.
- [x] Stockfish 19 oracle labeling (depth 10) for training data.
- Deferred: SPRT/LLR and an Elo anchor manifest. The repository deliberately
  has no Elo/SPRT gate; the rough-Elo estimator still lacks its anchor
  manifest and licensed book.

## Phase B - hot path

- [x] B1: en-passant legality probe no longer copies the whole `NativeState`
      (about 75 KiB) per move; it applies the trial capture in place.
- [x] B2: `GameState` copies no longer clone the 256-slot feature cache.
- [x] B3: search detaches the vendored compatibility mirror so no search node
      pays for the shadow board.
- [x] B4: precomputed bitboard attack tables
      (`src/koi/detail/attack_tables.{hpp,cpp}`): knight/king/pawn attacks plus
      ray-based sliding attacks. `square_attacked`, `attacker_count` and
      `king_square` were rewritten on top of them. A latent en-passant
      occupancy bug found by `native_rule_state_tests` was fixed in the same
      commit.
- [x] B5: `native_move_gives_check` is now O(1): a switch on the moved piece
      answers the direct check from the destination square, and discovered
      checks are found by tracing one rook ray and one bishop ray from the
      enemy king through the post-move occupancy. Vacated squares (source,
      en-passant square, castling rook square) are removed from the friendly
      slider masks.
- Deferred: bitboard move generation, fully incremental legality (pins and
  checkers instead of apply/undo), power-of-two TT indexing and prefetch,
  parallel-root `GameState` copy elimination, persistent worker pool and lazy
  SMP.

## Phase C - NNUE

- [x] C1: `tools/measurement/gen_training_data.py` generates positions from
      Stockfish 19 self-play with noise games and labels them at depth 10.
      Final corpus: 1,461,259 positions and 1,200,002 labeled rows.
- [x] C2: `tools/measurement/train_nnue_sf.py` trains the v2 960-256-32-1
      network with an embedding-bag sparse first layer and exports a
      fixed-point network (float checkpoint + `.nnue`).
- [x] C3: the feature encoder became side-to-move relative (piece planes,
      king context, pawn flags, perspective sign) and inference gained a
      sparse AVX2 first layer. Without this the network could not learn whose
      turn it is and validation error stayed near the constant predictor.
- [x] C4: NNUE v3 container with explicit fixed-point shifts (`s1`, `s2`,
      `k3`), C++ load/serialize/inference, `NnueLoader` validation, a v3
      boundary test, `koi-bench --nnue`, UCI `EvalFile`, and automatic
      `koi.nnue`/`KOI_NNUE_PATH` loading at startup with classical fallback.
- Verdict: the trained net loads, passes the loader boundary tests and the
  64/64 tactical gate, and runs at roughly 85% of classical NPS, but it lost
  the equal-node A/B match 0-4-5 in the games that finished. Classical
  evaluation therefore remains the default; NNUE stays a fully wired opt-in.
- Deferred: accumulator updates on make/unmake, king buckets, halfka/threat
  features, larger corpora, and training on GPU hardware.

## Phase D - search modernization

Deferred by design. Correction history, a true IID, LMP tables, and qsearch TT
cutoffs all change the search tree and would invalidate the exact-value and
node-count tests that currently pin the engine's behaviour. That work belongs
in a dedicated pass with its own acceptance campaign.

## Phase E - tablebases and book

Deferred. No Syzygy files or licensed `book.bin` exist in this checkout, so
interior probing and book auditing cannot be validated here.

## Phase F - verification and docs

- [x] README updated for the opt-in NNUE path (`EvalFile`, startup loading,
      `koi-bench --nnue`) and the option/default lists.
- [x] Plan and verification records added under `docs/superpowers/`.
- [x] Full Release CTest run recorded in the verification record.
- [x] Phase commits: `7e9d96c`, `16c8ce7`, `e46e4c0`, `d21da4d`, `03db674`,
      `0b7cdb4`.
