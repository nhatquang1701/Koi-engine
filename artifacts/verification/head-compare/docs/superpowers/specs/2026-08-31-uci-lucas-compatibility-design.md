# Koi Engine Lucas Chess UCI Compatibility Design

Date: 2026-08-31

## Context

Koi already implements the core UCI handshake, asynchronous search, legal
standard-chess move handling, hash/thread/speed options, and clean protocol
output. Lucas Chess can use the engine for basic play, but common analysis and
tournament workflows expose gaps: the engine does not advertise analysis,
MultiPV, or pondering support; `searchmoves` is ignored; and `go ponder`
completes immediately instead of holding the search open.

This change is limited to UCI protocol behavior and the boundaries needed to
support those workflows. It does not add a new evaluation model, chess variant,
GUI, or engine-specific Lucas Chess integration.

## Goals and compatibility contract

- Keep the Windows x64 C++26 build and portable Release configuration.
- Keep stdout protocol-clean and keep diagnostics on stderr or valid `info
  string` lines.
- Preserve the existing deterministic `Threads=1`, `MultiPV=1` reference path.
- Keep one controller-owned asynchronous search and exactly one final
  `bestmove` for every non-suppressed search.
- Support normal play, Lucas Chess analysis, tutor-style MultiPV consumers,
  ponder commands, and legal root move restriction.
- Accept unknown or unsupported options without crashing or emitting invalid
  protocol text.

The handshake will add these supported options:

```text
option name UCI_AnalyseMode type check default false
option name MultiPV type spin default 1 min 1 max 16
option name Ponder type check default false
```

The existing `RandomSeed`, `Hash`, `Threads`, `Speed`, and `Clear Hash` options
remain available with their current ranges and semantics. Koi will not
advertise Chess960, NNUE, tablebases, or Elo-limiting options that it does not
implement.

## Architecture

### Protocol parsing and controller state

`uci::parse_go_limits` remains responsible for tokenizing numeric `go`
arguments. `SearchLimits` gains a `ponder` flag and an ordered list of parsed
UCI root moves. The parser recognizes `ponder` and `searchmoves`; unknown
tokens are ignored as permitted by the existing defensive parser. A
`searchmoves` list is passed through even when a coordinate is syntactically
valid but illegal in the current position; the search service filters it at the
root. If the list contains no legal root move, the result is `bestmove 0000`.

The controller stores `UCI_AnalyseMode`, `MultiPV`, and `Ponder` option values.
Option changes that affect an active search stop and join it before applying
the new snapshot. `MultiPV` is clamped to the advertised range and is copied
into `SearchOptions` at `go` time. `UCI_AnalyseMode` is accepted and retained
for Lucas compatibility; it does not change evaluation in this release.

The controller recognizes `ponderhit`. On a ponder search it suppresses and
joins the current result, then starts a normal search from the same root and
the same underlying limits with pondering disabled. This intentionally gives
up the speculative search tree in v1, but gives Lucas Chess a safe, simple
protocol transition and guarantees one final result for the resumed search.
`stop` on a ponder search suppresses no result: it joins the search and emits
the current legal best move exactly once.

### Search service

`SearchOptions` gains `multi_pv` and `analyse_mode`; `SearchLimits` owns the
ponder search-mode flag, so the controller does not need to copy a second
ponder value into the options snapshot. `SearchInfo` gains a `multipv`
number.
The search service filters the generated legal root metadata by
`SearchLimits.search_moves` while preserving the original legal move order.
Every root result remains a Koi `Move`, so legality and special moves do not
depend on repeated UCI-string conversion.

For `MultiPV=1`, the existing single-thread search and deterministic threaded
root search remain the reference behaviors. For a larger MultiPV value, each
completed iteration sorts completed root lines by descending score, retaining
stable root order as the equal-score tie-break, and emits one `SearchInfo` per
selected line with `multipv 1..N`. The first line determines `SearchResult` and
the final `bestmove`. PVs are copied from fixed-capacity internal lines into
the public vector only at the event boundary; selected moves are distinct and
legal.

Pondering is a lifecycle mode, not a protocol-output mode. A ponder search
continues iterative search without completing on its requested depth, node, or
clock boundary; cancellation is still checked at every search node. The
controller owns the transition back to a bounded normal search on
`ponderhit`, so no worker writes a final `bestmove` during the speculative
phase. `go infinite` remains a live analysis search and continues until
`stop`.

### Output and lifecycle invariants

- Only the controller callback writes UCI output.
- Stale callbacks are rejected with the existing generation counter.
- A normal completed or stopped search emits one `bestmove`.
- A search replaced by `position`, `ucinewgame`, an option change, or `quit`
  is suppressed and emits no late output.
- `isready` can be answered while a search is active.
- A terminal root or an empty legal `searchmoves` filter emits `bestmove 0000`.
- Invalid FENs, malformed moves, malformed option values, and incomplete
  `go` fields do not crash or hang the process.

## Data flow

```text
UCI line
  -> controller token parser
  -> SearchLimits/SearchOptions snapshot
  -> SearchService root filtering and iterative search
  -> SearchInfo(multipv) callbacks
  -> controller generation check and UCI info lines
  -> one SearchResult callback and bestmove
```

The ponder transition is:

```text
go ponder -> active speculative search
stop      -> join -> one bestmove
ponderhit -> suppress/join -> restart same root/limits normally -> one bestmove
```

## Error handling and non-goals

Malformed or unsupported input is ignored or reported with a valid `info
string`; it must never place arbitrary diagnostics on stdout. No attempt is
made to infer a Lucas Chess version or to emulate GUI-specific menu behavior.
The implementation does not promise retained ponder work, exact engine
analysis scores, or a particular MultiPV ordering when scores differ by less
than the search's integer score resolution.

## Verification

Add unit and process coverage for option handshake/state, `searchmoves`
filtering, MultiPV formatting and distinct legal PVs, ponder lifecycle,
`ponderhit`, cancellation, terminal/empty-root behavior, and unchanged
single-thread deterministic search. Run existing rules, search, UCI, benchmark,
and Lucas-style process tests in both Release and Debug configurations. A
manual Lucas Chess check remains the final GUI acceptance step: configure Koi
as an external UCI engine, run analysis and tutor workflows with MultiPV,
start/stop a ponder-capable game, and confirm that no duplicate or malformed
responses appear.
