# Koi Engine Performance and UCI Speed Controls

## Goal

Improve Koi Engine search speed while preserving legal chess behavior,
deterministic best moves, cancellation, and Lucas Chess UCI compatibility.

## Global constraints

- Windows x64 and C++26/MSVC remain the release target.
- `Threads=1` is the reference path and the default.
- `Speed=100` preserves current timing behavior; lower values shorten
  time-based budgets only.
- Explicit `depth`, `nodes`, and `infinite` limits are unchanged by `Speed`.
- Root-parallel output is deterministic: stable root ordering and earliest
  root move on equal scores.
- One controller-owned search handle emits at most one completion result;
  inner workers never write UCI output.
- Hash is one shared table, default 16 MB, configurable from 1 to 4096 MB;
  it is not multiplied by thread count.
- Standard chess and portable MSVC optimization remain in scope; CPU-specific
  instruction-set requirements, lazy SMP, NNUE, books, tablebases, and
  variants remain deferred.

## Tasks

### Task 1: Options and time-budget plumbing

- Extend `SearchOptions` with `threads` and `speed_percent`.
- Scale movetime and clock-derived budgets in `TimeManager`.
- Advertise and validate `Threads` and `Speed` through UCI.
- Stop/join active search before applying either option.
- Add failing tests for defaults, validation, snapshots, and speed semantics.

### Task 2: Search and hot-path optimizations

- Add deterministic root-parallel search with shared cancellation and global
  node accounting.
- Keep the single-thread search behavior as the reference path.
- Use fixed-size internal PV lines, direct move conversion, and precomputed
  ordering keys/scores.
- Aggregate worker statistics and emit `info` only from the controller-owned
  worker.
- Add tests for deterministic threaded output, legal moves, cancellation,
  one completion callback, and global node limits.

### Task 3: Concurrent transposition table

- Replace the single TT mutex with striped probe/store locks.
- Retain exclusive synchronization for resize, clear, and generation changes.
- Preserve bounded 1–4096 MB configuration and deterministic replacement.
- Add concurrent stress and invalid-entry regression tests.

### Task 4: Build, benchmark, protocol, and documentation

- Enable portable MSVC Release IPO/LTO in CMake.
- Extend `koi-bench` with `--threads`, `--speed`, and optional `--timed`.
- Extend UCI/process/Lucas-style tests for options, output cleanliness, and
  multi-ply play.
- Update README with recommended Lucas Chess settings and benchmark usage.
- Run Debug/Release builds, CTest, UCI smoke, and deterministic/timed benches.

## Acceptance

All existing tests remain green. New tests cover the task bullets above, the
UCI handshake advertises valid options, malformed inputs do not crash or hang,
stdout remains protocol-clean, and every reported best move is legal or
`0000` for a terminal root.
