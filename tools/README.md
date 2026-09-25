# Koi developer tools

The tools are grouped by the job they perform. They resolve the repository root
from their own location, so commands are safe to run from any working directory.
Durable reports, match logs, and packages default under `artifacts/`; temporary
test scratch space may use the operating-system temporary directory and must be
cleaned up by the caller.

Directory map:

| Directory | Purpose |
| --- | --- |
| `tools/build/` | build, package, book-install, and verification scripts |
| `tools/engine/` | small C++ command-line tools built with Koi |
| `tools/measurement/` | PGN, oracle, corpus, tuning, and NNUE tooling |
| `tools/nnue/` | NNUE Studio GUI, trainer backends, and the bullet/Rust crate and wrapper |
| `tools/stability/` | UCI, Stockfish, and Cutechess process harnesses |
| `tools/test/` | CTest wrapper that captures JUnit and LastTest logs |
| `build/` | ignored canonical Debug/Release build trees |
| `artifacts/` | ignored reports, matches, manifests, packages, and evidence |

The checked-in fixtures are under `tests/data/games`, `tests/data/openings`,
and `tests/data/positions`. Stockfish and other installed dependencies may be
selected explicitly; they are never copied into generated report directories.

The tooling runs on Windows and Linux. Python tools use the in-tree
`koi_chess` package, PowerShell tools run under `pwsh`, and binary names pick
up the platform suffix automatically (`.exe` on Windows, no suffix on Linux).
The Studio GUI starts from `Koi NNUE Studio.cmd` on Windows and
`./koi-nnue-studio.sh` on Linux. `tools/build/package_release.ps1` writes the
Windows `.zip` or, on Linux, the portable `koi-engine-v1.1-linux-x86_64.tar.gz`
built in an Ubuntu 22.04 container by CI.

## Curating legacy evidence

`tools/build/curate_artifacts.ps1` is a one-time, allowlisted importer for
previous Koi result bundles. It copies only approved report/evidence file types,
verifies every destination hash, and writes
`artifacts/manifests/external-evidence-manifest.json`. The source is explicit so
normal commands never create or depend on an external results directory:

```powershell
$approvedLegacyEvidenceRoot = Read-Host 'Path to the approved legacy evidence directory'
powershell -NoProfile -ExecutionPolicy Bypass `
  -File .\tools\build\curate_artifacts.ps1 `
  -SourceRoot $approvedLegacyEvidenceRoot `
  -DestinationRoot .\artifacts
```

Review the manifest before optionally repeating the command with `-PruneSource`;
that switch removes only the selected, verified source bundles.

## Generated-tree organization

`tools/build/prune_generated_trees.ps1` previews stale direct children of
`build/` and the historical `out/` root. It preserves only
`build/debug`, `build/release`, `build/ci-debug`, and `build/ci-release`
(an additional local `build/asan` tree is not on the preserve list and will be
previewed as stale):

```powershell
& .\tools\build\prune_generated_trees.ps1
& .\tools\build\prune_generated_trees.ps1 -Apply
```

After a verified build and Release CTest run, write the ignored provenance
manifest with:

```powershell
& .\tools\build\write_organization_manifest.ps1
```

It records tracked moves, canonical build/executable hashes, toolchain versions,
the verification result, and every hash in the curated evidence manifest.

## Fixed-depth performance gate

`performance_gate.ps1` compares two explicitly supplied `koi-bench` executables
using repeated cold-hash, fixed-depth, timed runs of the 64-position strength
suite. It reports the median total elapsed time for each executable and fails
only when the candidate median is more than 5% slower. Profiles and logs belong
under `artifacts/verification/`, and this is intentionally a standalone
measurement command rather than a CTest threshold:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\build\performance_gate.ps1 `
  -BaselineExecutable .\build\release\koi-bench.exe `
  -CandidateExecutable .\build\release\koi-bench.exe `
  -Runs 5 -Threads 4 -Speed 100 `
  -OutputDirectory .\artifacts\verification\performance-gate
```

## Speed program gate

`speed_gate.ps1` compares two `koi-bench` executables that both include the
Phase 0 measurement upgrade (`--report`). It alternates timed passes between the
executables, consumes the `koi-bench-speed-v1` artifacts, and compares per-row
and total NPS medians plus the time-to-depth table. It fails when total NPS
regresses past `-MaxNpsRegressionPercent` (default 2%) or when the median row
NPS regresses past `-MaxRowNpsRegressionPercent`. The run artifacts and a
`speed-gate.json` summary land under `artifacts/verification/`; like the
fixed-depth gate this is a standalone measurement command, never a CTest
threshold:

```powershell
pwsh -NoProfile -File .\tools\build\speed_gate.ps1 `
  -BaselineExecutable .\artifacts\verification\speed-program\phase-0\koi-bench-phase0.exe `
  -CandidateExecutable .\build\release\koi-bench.exe `
  -Runs 5 -Threads 1 -DepthSweep 2..7 `
  -FenFile .\tests\data\positions\evaluation-positions.txt `
  -OutputDirectory .\artifacts\verification\speed-program\speed-gate\phase-1
```

Install the measurement dependencies from the repository root:

```powershell
python -m pip install -r .\tools\measurement\requirements.txt
python -m unittest .\tests\python\measurement\elo_oracle_test.py -v
```

## Classical evaluation term tuning

`koi-eval-features` (built as `build/release/koi-eval-features.exe`) reads FEN
lines (optionally `FEN;cp;best_move`) and writes the classical evaluator's term
breakdown as CSV. `tune_classical.py` fits the twelve term columns to the label
`cp` (or to the evaluator's `total`) with a centered ridge solve, then writes a
candidate parameter header and a JSON report under `artifacts/`:

```powershell
Get-Content .\artifacts\training\labels.txt -TotalCount 50000 |
  .\build\release\koi-eval-features.exe --output .\artifacts\verification\classical-features-sample.csv
python .\tools\measurement\tune_classical.py --input .\artifacts\verification\classical-features-sample.csv `
  --header-out .\artifacts\verification\tuned-classical.h `
  --report-out .\artifacts\verification\tuned-classical-report.json
```

The emitted header is a report, not an adopted parameter set: the evaluator
never reads it, and tuned scales are only adopted after the tactical, suite,
and match gates pass.

## PGN extraction and oracle analysis

`elo_oracle.py` reads standard SAN PGN mainlines. Comments, NAGs, and recursive
variations are ignored by the mainline walk; castling, promotion, FEN setup
headers, side-to-move, move number, sequential ply, pre-move FEN, SAN, and UCI
move are retained in each position record. Mainline comments, NAGs, and
recognized `%eval`/`%clk` tokens are retained under `annotations` when present.

Extract positions without starting either engine:

```powershell
python .\tools\measurement\elo_oracle.py `
  --pgn .\tests\data\games\2026-09-08-koi-vs-stockfish-19.pgn --extract-only
```

The command prints the generated `report <path>` and writes a `koi-elo-oracle`
JSON report. Use `--output .\artifacts\verification\extraction.json` when a
stable path is needed. For Stockfish comparison, keep the report in the same
repository-local verification tree:

```powershell
python .\tools\measurement\elo_oracle.py `
  --pgn .\tests\data\games\2026-09-08-koi-vs-stockfish-19.pgn `
  --koi .\build\release\koi-engine.exe `
  --stockfish .\third_party\stockfish-19\stockfish-windows-x86-64-universal\stockfish\stockfish-windows-x86-64-universal.exe `
  --output .\artifacts\verification\elo-oracle.json `
  --movetime-ms 250 --threads 4
```

The report records engine identity, handshake/options, input SHA-256, root FEN,
actual SAN/UCI move, Koi suggestion, Stockfish scores, timings, and separate CPL
for the actual and suggested move. Scores are normalized to White's perspective;
mate is represented as +/-100000 cp. Do not turn a CPL report into an Elo claim
without a comparable baseline and color-balanced match evidence.

Forensic extraction of every PGN below the checked-in user-test directory uses
the deterministic corpus walker. It preserves the same per-position records,
adds one SHA-256 for each input file, and writes a timestamp-independent
`content_sha256` for reproducibility:

```powershell
python .\tools\measurement\pgn_forensics.py `
  --pgn-dir ".\tests\data\games"
```

The default `koi-pgn-forensics-v1` report is written below
`artifacts/verification/`. Use `--output` when a stable report name is needed.

## Read-only GigaBase sampling

`gigabase_extract.py` accepts a SQLite-backed GigaBase export, opens it with
`mode=ro`, enables and verifies `PRAGMA query_only`, and never copies or writes
the source database. It uses seed `240906` by default, caps the sample at
200,000 games and 2,000,000 positions, and emits separate train, validation,
and holdout manifests plus a summary under `artifacts/manifests/gigabase`:

```powershell
python .\tools\measurement\gigabase_extract.py `
  --database C:\Data\gigabase.sqlite `
  --output-dir .\artifacts\manifests\gigabase
```

The adapter discovers a game/position table and records only source IDs,
position counts, and content hashes in the `koi-gigabase-manifest-v1` outputs.
Use `--table`, `--id-column`, `--pgn-column`, or
`--position-count-column` when a GigaBase export uses nonstandard names.
Generated manifests belong under `artifacts/manifests/`; the source database is
opened read-only and is never copied or modified.

## Licensed-book audit

Install the pinned CC0 opening book beside a specific engine executable with
the one-time installer:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\build\install_book.ps1 `
  -EnginePath .\build\release\koi-engine.exe
```

`-EnginePath` is mandatory and must identify an existing local `.exe` file.
The installer downloads the pinned `lichess_1900_rapid_2026-05.bin` asset from
the `books-2026-05-v1` release, verifies SHA-256
`56abc70e5291b4338356009d380e565fd85eab8067f6bf34927b5807ff231370`, and
installs it as `book.bin` in the executable's directory. It refuses to replace
a different existing book unless `-Force` is supplied. Temporary downloads are
cleaned up, and Koi has no runtime network dependency.

`book_audit.py` is the named entrypoint for the existing separate book-audit mode.
It keeps book hits and book-move CPL out of ordinary search metrics and uses
`OwnBook=true`, `BookDepth=16`, and `BookRandom=false`:

```powershell
python .\tools\measurement\book_audit.py `
  --pgn .\tests\data\games\2026-09-08-koi-vs-stockfish-19.pgn `
  --koi .\build\release\koi-engine.exe `
  --stockfish .\third_party\stockfish-19\stockfish-windows-x86-64-universal\stockfish\stockfish-windows-x86-64-universal.exe `
  --book C:\LicensedBooks\book.bin
```

Use `--output .\artifacts\verification\book-audit.json` for a stable path. The
book is an external licensed input; do not commit or redistribute it.

## Stockfish matches

`stockfish_match.py` is a safe Python entrypoint for the Windows
PowerShell UCI match harness. It uses `subprocess.run` with an argument list and
never invokes a shell. `--help` only parses arguments and does not start engines.
The default output directory is `artifacts/matches`.

```powershell
python .\tools\measurement\stockfish_match.py --help
python .\tools\measurement\stockfish_match.py `
  --koi .\build\release\koi-engine.exe `
  --stockfish .\third_party\stockfish-19\stockfish-windows-x86-64-universal\stockfish\stockfish-windows-x86-64-universal.exe `
  --opening-file .\tests\data\openings\openings-basic.txt `
  --time-control 1+0 --games 20 --koi-color white `
  --own-book false --threads 4 --output-directory .\artifacts\matches\match-1p0-white
```

For a reproducible Stockfish strength anchor, use Python's `--opponent-elo`
alias `--stockfish-elo`, or invoke the PowerShell harness directly with
`-OpponentElo` alias `-StockfishElo`. An explicit value is clamped to
1320..3190, enables `UCI_LimitStrength`, and sends `UCI_Elo` only to the
opponent. Omit the strength option to leave strength limiting disabled.

`--timeout-ms` / `-TimeoutMilliseconds` is a protocol read, readiness, and
shutdown bound. In a clocked match, the `--time-control` / `-TimeControl`
chess clock remains the deadline for `bestmove`.

Use `--depth`, `--movetime-ms`, or `--nodes` for fixed limits, and use
`--fen-file name | six-field FEN` through the existing harness for named FEN
positions. Run balanced conditions with Koi White and Koi Black, and record the
exact engine paths, options, time control, opening/FEN input, and generated JSON
and PGN together in the repository-local `artifacts/matches` directory.

## Cutechess stability campaign

`cutechess_stability.ps1` captures a single reproducible Cutechess run without
using Cutechess's global `-debug` switch. It records executable hashes, the
exact argument array, streamed manager output, Koi JSONL diagnostics, per-ply
records, and partial failure artifacts. `-KoiColor white|black` fixes the Koi
side for a one-game run; the default `auto` preserves Cutechess's normal
round-based color assignment.

`cutechess_stability_campaign.ps1` schedules independent one-game runs so each
game starts fresh engine processes. Its default 500-game plan contains 400
Koi-Stockfish games, 50 Koi-Koi games, and 50 book-audit games. It rotates
Threads 1/2/4, balances Koi colors, samples start positions plus curated
opening and FEN inputs, writes `schedule.json` before execution, and appends
`games.jsonl` after every game. A missing book is recorded as
`book_unavailable` and those games safely run no-book.

Generate and inspect the deterministic schedule without launching engines:

```powershell
& .\tools\stability\cutechess_stability_campaign.ps1 `
  -KoiPath .\build\release\koi-engine.exe `
  -StockfishPath '.\third_party\stockfish-19\stockfish-windows-x86-64-universal\stockfish\stockfish-windows-x86-64-universal.exe' `
  -OutputDirectory .\artifacts\stability\campaign-plan `
  -PlanOnly
```

Run the campaign only after the focused and full stability tests are green.
The campaign produces no Elo or rating report; it is a reliability gate.

All three tools fail with an actionable stderr message and nonzero exit code for
missing inputs or engine/configuration errors. The C++ engine remains independent
of Python; the measurement tooling uses the in-tree `koi_chess` package.

## Rough local Elo estimation

`tools/measurement/elo_estimate.py` is standard-library-only and consumes the existing
`koi-uci-match-v2` JSON/PGN reports. The primary measurement is no-book at 1+0
with Koi `Hash=512`, `Threads=4`, and `Speed=100`, using
`tests/data/openings/openings-curated-32.txt` once in each Koi color for every anchor. Two
anchors produce 128 initial games (32 openings x 2 colors x 2 anchors); adaptive
64-game batches can extend the schedule through 320 games. The fit is Koi's
perspective with 2,000 paired-opening bootstrap samples and a documented anchor
bracket, not a universal rating claim.

The required external anchor manifest has this shape:

```json
{
  "schema": "koi-elo-anchor-manifest-v1",
  "stockfish": {
    "path": ".\\third_party\\stockfish-19\\stockfish-windows-x86-64-universal\\stockfish\\stockfish-windows-x86-64-universal.exe",
    "elos": [1400, 1600, 1800],
    "rating_source": "Stockfish 19 UCI_LimitStrength"
  },
  "lower_anchors": []
}
```

`stockfish.path` must be an existing file matching `--stockfish`; `elos` must
contain at least two unique Stockfish `UCI_Elo` values in 1320..3190; and
`rating_source` is required. A lower anchor entry requires `id`, an existing
`path`, positive `rating`, and `rating_source`, and is required to bracket a
`--prior-elo` below the Stockfish floor. All anchors must bracket the prior.

Use this exact no-book dry-run CLI from the repository root:

```powershell
python .\tools\measurement\elo_estimate.py `
  --koi .\build\release\koi-engine.exe `
  --replay .\build\release\koi-replay.exe `
  --stockfish .\third_party\stockfish-19\stockfish-windows-x86-64-universal\stockfish\stockfish-windows-x86-64-universal.exe `
  --anchors .\artifacts\manifests\elo-anchors.json `
  --openings .\tests\data\openings\openings-curated-32.txt `
  --time-control 1+0 `
  --threads 4 --hash 512 --speed 100 `
  --min-games 128 --max-games 320 `
  --prior-elo 1600 --mode no-book `
  --output .\artifacts\matches\rough-elo-no-book.json `
  --dry-run
```

Dry-run validates all inputs and writes the full deterministic schedule without
starting Koi, replay, Stockfish, or PowerShell. Remove `--dry-run` only when the
selected executable, anchor, corpus, and repository-local artifact paths are
ready. Raw JSON and matching PGN files are preserved below the report's
`<stem>-artifacts` directory under `artifacts/matches`; do not commit generated
contents.

Book mode is a separate run, with a separate report and external licensed book:

```powershell
python .\tools\measurement\elo_estimate.py `
  --koi .\build\release\koi-engine.exe `
  --replay .\build\release\koi-replay.exe `
  --stockfish .\third_party\stockfish-19\stockfish-windows-x86-64-universal\stockfish\stockfish-windows-x86-64-universal.exe `
  --anchors .\artifacts\manifests\elo-anchors.json `
  --openings .\tests\data\openings\openings-curated-32.txt `
  --time-control 1+0 `
  --threads 4 --hash 512 --speed 100 `
  --min-games 128 --max-games 320 `
  --prior-elo 1600 --mode book --book C:\LicensedBooks\book.bin `
  --output .\artifacts\matches\rough-elo-book.json
```

Book mode records `OwnBook=true`, `BookFile`, `BookDepth=16`, and
`BookRandom=false`; it must not be merged with the no-book result. CTest covers
the estimator CLI logic, match-option forwarding, and all 32 corpus lines, but
real matches are not a CI job or a fixed Elo gate because runtime depends on the
recorded hardware, engine options, external executables, and time control. Any
reported value must be labeled “local Stockfish-equivalent Elo at recorded
hardware/options/time control” and must not be presented as a universal Elo
claim.

## NNUE training and the Studio

The Studio (`tools/nnue/`) is the training front end. `tools/nnue/koi_nnue_studio.py`
is a tkinter GUI with Data, Train, Validate and install, and Runs tabs; the same
file exposes a headless CLI (`--list-backends`, `--dry-run`, `--run`, `--selftest`).
`tools/nnue/studio_core.py` owns run directories under `artifacts/training/runs/`,
progress parsing, validation and install helpers, and the detached run launcher.

Backends live under `tools/nnue/backends/`: `koi` trains the version 5 network
by default with `tools/measurement/train_nnue_koi.py --arch v5` on CPU PyTorch
(group A `halfka-king-bucket-v1` with 9216 inputs plus symmetric group B
`threat-pairs-v1` with 27648 inputs, one shared feature transformer forwarded
from both perspectives into full-width cross pairs `p[j] = own[j] * opp[j]`, a
32-unit CReLU layer, and eight piece-count buckets; hidden 1536 and l1 32 are
the defaults), while `--arch v4` trains the earlier within-perspective
`halfka-king-bucket-v1` version 4 network (hidden 1024). `torch` drives the
legacy `train_nnue_sf.py` v3 trainer, and `bullet` drives the pinned Rust/CUDA
crate under `tools/nnue/bullet_train/` through `tools/nnue/run_bullet.py` when
cargo and a CUDA 12.x toolkit are installed, defaulting to version 5 with
`--arch v4` keeping the earlier pair-product network. The bullet path converts
text labels with `tools/nnue/to_bullet.py`, trains, measures validation MAE from
each saved checkpoint, and exports the matching container with
`tools/measurement/export_bullet_v5.py` (v5) or `export_bullet_v4.py` (v4).
The converter carries the corpus game result through when the labels record
one, and the trainer's `--wdl` weight (default `0.5`) blends it with the
centipawn target; `--wdl 0` reproduces the pure evaluation blend.
`koi-dataset-v2` encodes four index groups (`A_stm`, `B_stm`, `A_opp`, `B_opp`)
per record.

Typical commands:

```powershell
python .\tools\nnue\koi_nnue_studio.py --list-backends
python .\tools\nnue\koi_nnue_studio.py --dry-run --preset quick
python .\tools\nnue\koi_nnue_studio.py --run standard --detach
```

Validation runs the 64-position gate (`koi-bench --nnue`) and a node-limited,
colour-split A/B match (`tools/nnue/ab_match.ps1` versus the classical evaluator;
`tools/nnue/net_match.ps1` for network versus network). Those results are local
reports: the classical evaluator remains the engine default and no reported
number is an Elo claim.

GPU NNUE inference is optional at runtime. When `nvcc` is available
(CMake option `KOI_ENABLE_GPU_NNUE`, on by default), the build compiles
`src/koi/gpu/koi_nnue_v5.cu` to one PTX module per compute capability listed in
`KOI_GPU_PTX_ARCHS` (default `61;75;86;89;120`) and embeds them; at runtime the
engine loads `nvcuda.dll` dynamically, uses the driver API only, and JITs the
newest embedded module the device can run, so a CPU-only build or a GPU older
than sm_61 simply stays on the CPU path. Set `KOI_GPU_NNUE=1` to use the GPU
for `Threads > 1`; `KOI_GPU_FORCE_FAIL=1` forces the CPU fallback for testing
and `KOI_GPU_PTX_ARCH=75` forces one embedded module.
`tools/engine/koi_gpu_probe.cpp` verifies the device path and
`tests/unit/runtime/gpu_nnue_tests.cpp` checks bit-exact parity against the CPU
scalar evaluation plus the PTX selection rules. No strength or speed claim is
attached to this first pass.
