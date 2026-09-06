# Koi Measurement and Forensic Corpus Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add bounded, reproducible PGN/oracle/GigaBase measurement tooling and legal forensic fixtures without changing the Koi engine interfaces.

**Architecture:** Keep `tools/elo_oracle.py` as the compatibility-preserving engine oracle and extend its PGN records and metadata in place. Add `tools/pgn_forensics.py` as a read-only corpus walker that reuses the oracle extractor, and add `tools/gigabase_extract.py` as a standard-library-only SQLite read-only sampler that emits external split manifests. Keep fixtures data-only and validate them from focused Python tests.

**Tech Stack:** Python 3 standard library for filesystem, JSON, hashing, SQLite, and deterministic sampling; the repository's existing `python-chess==1.11.2` measurement dependency for legal PGN extraction and FEN validation; `unittest` subprocess tests.

**Spec:** `C:/Users/ntATh/AI test/Koi engine/.superpowers/sdd/koi-24-hour-strength-rules-flexibility/task-1-brief.md`

## Global Constraints

- Parse every available PGN under `third_party/User tests` and preserve mainline FEN, ply, side, SAN, actual move, and annotations when present.
- Extend the existing oracle tools without adding a production Python dependency.
- The GigaBase sampler opens the known database read-only, enables query-only mode, uses a fixed seed, caps output at 200,000 games / 2,000,000 positions, and emits train/validation/holdout manifests.
- Do not copy or modify the database; generated reports stay outside the repository.
- Add legal data-only fixtures for opening drift, missed development, pawn breaks, queen/rook shuffling, poisoned captures, forks, and king attacks.
- Do not modify `src/koi` search, position, evaluator, UCI, or CMake interfaces; do not tune or generate an opening book; do not add external runtime dependencies.

---

### Task 1: Make PGN extraction and oracle metadata forensic-complete

**Files:**
- Modify: `tools/elo_oracle.py`
- Test: `tests/elo_oracle_test.py`

**Interfaces:**
- Preserve `extract_games(pgn_text) -> list[dict]` and all existing report keys.
- Add `classify_cpl(cpl: int | None) -> str | None` with documented thresholds and use it only for derived move records.
- Add executable SHA-256 metadata below `engines.<engine>.hashes.executable_sha256`; retain existing identity, version, options, timestamps, FEN, move, depth, and node fields.
- Add a position `annotations` object containing the original comment (when any), sorted NAGs, and recognized `%eval`/`%clk` tokens without removing unknown comment text.

- [ ] **Step 1: Write failing tests for annotations, engine hashes, and move classifications**

  Extend the existing extraction fixture with a `%eval`/`%clk` comment and a NAG, assert those fields survive mainline extraction, assert a fake-engine analysis report contains the SHA-256 of each executable, and assert CPL thresholds produce stable classification labels.

- [ ] **Step 2: Run the focused tests and verify they fail for the missing fields**

  Run: `python -m unittest tests.elo_oracle_test -v`

  Expected: failures are limited to the new annotation/hash/classification assertions; existing oracle behavior remains green.

- [ ] **Step 3: Implement the minimal forensic fields**

  Add a chunked file-hash helper, parse only structured `%eval` and `%clk` tokens while preserving the complete comment, include NAGs on each mainline node, hash the executable in `UciEngine.metadata`, and attach classifications to actual and suggested move records. Do not change the engine process protocol or the existing schema names.

- [ ] **Step 4: Run the focused tests and verify they pass**

  Run: `python -m unittest tests.elo_oracle_test -v`

  Expected: all oracle tests pass with zero failures and no stderr diagnostics.

- [ ] **Step 5: Commit the self-contained oracle extension**

  Run: `git add tools/elo_oracle.py tests/elo_oracle_test.py && git commit -m "feat: add forensic oracle metadata"`

### Task 2: Add deterministic PGN corpus extraction

**Files:**
- Create: `tools/pgn_forensics.py`
- Create: `tests/measurement_forensics_test.py`
- Modify: `tools/README.md`

**Interfaces:**
- `discover_pgns(directory: Path) -> list[Path]` returns recursively discovered `.pgn` files sorted by normalized relative path.
- `build_corpus_report(directory: Path) -> dict` returns schema `koi-pgn-forensics-v1`, one record per PGN, all parsed games/positions, SHA-256 input hashes, counts, and a timestamp-independent `content_sha256`.
- CLI: `python tools/pgn_forensics.py [--pgn-dir DIR] [--output PATH]`; default input is `third_party/User tests`, default output is an external temporary report, and an explicit repository-local output is rejected.

- [ ] **Step 1: Write failing tests for recursive discovery, deterministic schema, and external output**

  Create two temporary PGNs with comments, NAGs, and a variation; invoke the CLI twice; assert the same ordered file list, `content_sha256`, counts, and preserved position fields; assert `--output` inside the repository fails.

- [ ] **Step 2: Run the new focused test and verify it fails because the entrypoint is absent**

  Run: `python -m unittest tests.measurement_forensics_test.MeasurementForensicsTests.test_pgn_corpus_is_deterministic -v`

  Expected: import or subprocess failure indicating `tools/pgn_forensics.py` does not yet exist.

- [ ] **Step 3: Implement deterministic corpus extraction**

  Reuse `elo_oracle.extract_games` and its existing `python-chess` loader, sort paths, hash raw UTF-8 bytes, use relative POSIX paths in the canonical content, calculate the hash over data excluding timestamps, and write JSON only to an external path.

- [ ] **Step 4: Run the focused test and verify it passes**

  Run: `python -m unittest tests.measurement_forensics_test.MeasurementForensicsTests.test_pgn_corpus_is_deterministic -v`

  Expected: PASS with identical deterministic hashes across both runs.

- [ ] **Step 5: Run the corpus tool against all committed user PGNs and document its usage**

  Run: `python tools/pgn_forensics.py --pgn-dir "third_party/User tests"`

  Expected: one external `report <path>` line, all available PGNs parsed, and no generated file under the checkout. Update `tools/README.md` with the command, schema, and external-artifact rule.

- [ ] **Step 6: Commit the corpus extractor and tests**

  Run: `git add tools/pgn_forensics.py tests/measurement_forensics_test.py tools/README.md && git commit -m "feat: add deterministic PGN forensic corpus"`

### Task 3: Add bounded read-only GigaBase sampling

**Files:**
- Create: `tools/gigabase_extract.py`
- Modify: `tests/measurement_forensics_test.py`
- Modify: `tools/README.md`

**Interfaces:**
- `sample_database(database: Path, seed: int = 240906, max_games: int = 200000, max_positions: int = 2000000, table: str | None = None) -> dict` reads a SQLite GigaBase export through a read-only URI and returns deterministic selected records and split metadata.
- CLI: `python tools/gigabase_extract.py --database PATH [--output-dir DIR] [--seed N] [--max-games N] [--max-positions N] [--table NAME]`.
- Output files are `train.json`, `validation.json`, `holdout.json`, and `summary.json` in an external directory; each split has schema `koi-gigabase-manifest-v1`, source hash, seed, caps, counts, and records.

- [ ] **Step 1: Write failing tests for read-only/query-only operation, caps, deterministic splits, and schema**

  Build a temporary SQLite database with a small `games(id, pgn)` table, run the CLI twice with small caps, assert the database SHA-256 is unchanged, the summary reports `read_only=true` and `query_only=true`, no split exceeds either cap, every selected record appears in exactly one split, and `content_sha256` values match.

- [ ] **Step 2: Run the focused GigaBase test and verify it fails because the sampler is absent**

  Run: `python -m unittest tests.measurement_forensics_test.MeasurementForensicsTests.test_gigabase_sampling_is_read_only_deterministic_and_bounded -v`

  Expected: subprocess failure because `tools/gigabase_extract.py` does not yet exist.

- [ ] **Step 3: Implement the minimal read-only SQLite adapter and sampler**

  Resolve the database path without copying it, connect with a `file:` URI using `mode=ro`, set and verify `PRAGMA query_only=ON`, introspect table/column names safely, select a PGN or position-count column, use deterministic reservoir sampling with the fixed seed, enforce both caps, shuffle only the selected records, and assign stable train/validation/holdout splits. Hash canonical records for deterministic manifests and write only outside the repository.

- [ ] **Step 4: Run the focused GigaBase test and verify it passes**

  Run: `python -m unittest tests.measurement_forensics_test.MeasurementForensicsTests.test_gigabase_sampling_is_read_only_deterministic_and_bounded -v`

  Expected: PASS with unchanged database bytes and identical split hashes.

- [ ] **Step 5: Add explicit unsupported-schema and invalid-output-path coverage**

  Assert a database with no recognizable game/position table produces an actionable nonzero error, and an output directory inside the repository is rejected before any report is written.

- [ ] **Step 6: Commit the bounded sampler**

  Run: `git add tools/gigabase_extract.py tests/measurement_forensics_test.py tools/README.md && git commit -m "feat: add bounded read-only GigaBase sampler"`

### Task 4: Add legal data-only forensic fixtures and finish verification

**Files:**
- Create: `tests/data/measurement-forensics.json`
- Modify: `tests/measurement_forensics_test.py`
- Create: `.superpowers/sdd/koi-24-hour-strength-rules-flexibility/task-1-report.md`

**Interfaces:**
- The fixture schema is `koi-measurement-forensics-v1` with exactly the seven categories `opening_drift`, `missed_development`, `pawn_break`, `queen_rook_shuffling`, `poisoned_capture`, `fork`, and `king_attack`.
- Each fixture record has `id`, `category`, `fen`, `side`, and a short `purpose`; no engine output or generated report is committed.
- The report records changed paths, exact tests/results, external artifact locations, decoding limitations, and concerns.

- [ ] **Step 1: Write the fixture legality test before adding fixture data**

  Load the JSON with the standard library, assert the exact category set and unique IDs, construct `chess.Board(fen)` for each record, and assert `is_valid()` plus agreement between the FEN side field and fixture `side`.

- [ ] **Step 2: Run the legality test and verify it fails because the fixture is absent**

  Run: `python -m unittest tests.measurement_forensics_test.MeasurementForensicsTests.test_forensic_fixture_categories_are_legal -v`

  Expected: file-not-found or missing-fixture failure.

- [ ] **Step 3: Add seven legal FEN records and the schema assertion**

  Use positions reached from legal standard-chess move sequences, keep one record per requested category, and do not add opening-book or engine tuning data.

- [ ] **Step 4: Run all focused measurement tests**

  Run: `python -m unittest tests.measurement_forensics_test tests.elo_oracle_test -v`

  Expected: all focused tests pass with zero failures.

- [ ] **Step 5: Run the broader Python regression suite and verify the worktree diff**

  Run: `python -m unittest discover -s tests -p '*_test.py' -v` and `git diff --check`.

  Expected: all available Python tests pass, `git diff --check` is clean, and only measurement tools/tests/docs/fixtures/report paths are changed.

- [ ] **Step 6: Write the full report and commit the final implementation**

  Record exact command output counts, report paths under the system temporary directory, the absent/proprietary GigaBase decoding limitation, and any non-blocking concerns. Commit with `git add tools tests/data tests/measurement_forensics_test.py tools/README.md .superpowers/sdd/koi-24-hour-strength-rules-flexibility/task-1-report.md && git commit -m "test: add Koi measurement forensic corpus"`.

