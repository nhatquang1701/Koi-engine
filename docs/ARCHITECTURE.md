# System architecture

This page maps Koi's runtime modules to the files under `src/koi`. It is a
module map, not a call graph: solid arrows show the main data and control
direction, and every node names a real translation unit or header.

```mermaid
flowchart LR
    GUI["UCI GUI · Arena<br/>Cute Chess · lichess-bot"] -- "UCI protocol" --> UCI
    UCI["uci_controller.cpp<br/>options · protocol output<br/>search lifecycle"]
    UCI --> TIME["time_manager.cpp<br/>soft/hard budgets<br/>move overhead"]
    UCI --> SERVICE["search_service.cpp<br/>request snapshot<br/>cancellation · exactly-once"]
    SERVICE --> RUNNER["detail/search_runner.cpp<br/>iterative deepening<br/>aspiration · Lazy SMP root split"]
    RUNNER --> CONTEXT["detail/search_context.*<br/>negamax loop · quiescence"]
    RUNNER --> POLICY["detail/search_policy.hpp<br/>null move · LMR · futility<br/>probcut · extensions"]
    CONTEXT --> POSITION["position.cpp<br/>rules · move generation<br/>keys · rule state"]
    POSITION --> STATE["game_state.cpp<br/>compatibility facade<br/>Polyglot · legacy adapters"]
    CONTEXT --> TT["transposition_table.cpp<br/>24-byte entries<br/>hazard lease · generations"]
    CONTEXT --> SEE["static_exchange.cpp<br/>bitboard SEE"]
    CONTEXT --> ORDER["search_ordering.cpp<br/>history · killers · counters"]
    CONTEXT --> EVAL["classical_evaluator.cpp<br/>term model (default)"]
    EVAL --> NNUE["nnue.cpp<br/>int16 accumulators<br/>AVX2 kernels · 8 buckets"]
    UCI --> BOOK["opening_book.cpp<br/>optional Polyglot book"]
    UCI --> TB["syzygy_tablebase.cpp<br/>optional Syzygy probing"]
```

<style>
  .mermaid-viewport {
    overflow: auto;
    max-height: 78vh;
    padding: 8px;
    border: 1px solid var(--md-default-fg-color--lightest);
    border-radius: 6px;
  }
  .mermaid-viewport svg {
    max-width: none !important;
    height: auto;
    display: block;
  }
  .mermaid-controls {
    display: flex;
    gap: 8px;
    flex-wrap: wrap;
    margin: 6px 0 14px;
  }
  .mermaid-controls button {
    cursor: pointer;
    padding: 4px 12px;
    border-radius: 4px;
    border: 1px solid var(--md-default-fg-color--light);
    background: var(--md-default-bg-color);
    color: var(--md-default-fg-color);
  }
  .mermaid-controls button:hover {
    border-color: var(--md-accent-fg-color);
    color: var(--md-accent-fg-color);
  }
</style>
<script>
(function () {
  if (window.__koiMermaidControls) { return; }
  window.__koiMermaidControls = true;

  function makeButton(label, title, action) {
    var button = document.createElement('button');
    button.type = 'button';
    button.textContent = label;
    button.title = title;
    button.addEventListener('click', action);
    return button;
  }

  function wrap(block) {
    if (block.closest('.mermaid-viewport')) { return; }
    var svg = block.querySelector('svg');
    if (!svg) { return; }
    var viewport = document.createElement('div');
    viewport.className = 'mermaid-viewport';
    block.parentNode.insertBefore(viewport, block);
    viewport.appendChild(block);

    var box = svg.viewBox && svg.viewBox.baseVal;
    var baseWidth = (box && box.width) || svg.getBoundingClientRect().width || 800;
    var baseHeight = (box && box.height) || svg.getBoundingClientRect().height || 600;
    var zoom = 1;

    function apply() {
      svg.style.width = (baseWidth * zoom) + 'px';
      svg.style.height = (baseHeight * zoom) + 'px';
    }

    var bar = document.createElement('div');
    bar.className = 'mermaid-controls';
    bar.appendChild(makeButton('\u2190', 'Scroll left', function () {
      viewport.scrollLeft -= Math.max(240, viewport.clientWidth * 0.6);
    }));
    bar.appendChild(makeButton('\u2192', 'Scroll right', function () {
      viewport.scrollLeft += Math.max(240, viewport.clientWidth * 0.6);
    }));
    bar.appendChild(makeButton('Zoom +', 'Zoom in', function () {
      zoom = Math.min(4, zoom * 1.25);
      apply();
    }));
    bar.appendChild(makeButton('Zoom \u2212', 'Zoom out', function () {
      zoom = Math.max(0.2, zoom * 0.8);
      apply();
    }));
    bar.appendChild(makeButton('Fit', 'Fit to width', function () {
      zoom = Math.max(0.2, (viewport.clientWidth - 24) / baseWidth);
      apply();
      viewport.scrollLeft = 0;
      viewport.scrollTop = 0;
    }));
    bar.appendChild(makeButton('Reset', 'Reset zoom and scroll', function () {
      zoom = 1;
      apply();
      viewport.scrollLeft = 0;
      viewport.scrollTop = 0;
    }));

    viewport.parentNode.insertBefore(bar, viewport);
    apply();
  }

  function enhance() {
    document.querySelectorAll('pre.mermaid, div.mermaid').forEach(function (block) {
      if (block.querySelector('svg')) { wrap(block); }
    });
  }

  function start() {
    enhance();
    var attempts = 0;
    var timer = setInterval(function () {
      enhance();
      if (++attempts > 150) { clearInterval(timer); }
    }, 200);
  }

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', start);
  } else {
    start();
  }
})();
</script>

## Components

| File | Responsibility |
| --- | --- |
| `uci_controller.cpp` | Owns the current position and the search-worker lifecycle; the only layer that writes UCI output. |
| `search_service.cpp` | Public search lifecycle boundary: request snapshot, cancellation, exactly-once completion. |
| `detail/search_runner.cpp` | Backend dispatch, iterative deepening, aspiration windows, Lazy SMP and root-parallel scheduling. |
| `detail/search_context.*` | Search-local recursive state: move loop, quiescence, pruning call sites, fixed-capacity stack. |
| `detail/search_policy.hpp` | Centralized scalar gates: null move, late-move reductions, futility, probcut, check extensions. |
| `position.cpp` | Production authority for legality, position keys, rule state, and move generation. |
| `game_state.cpp` | Compatibility facade used by the evaluator, search, and UCI APIs; keeps its private mirror synchronized. |
| `transposition_table.cpp` | Physical storage, locking, generations, clearing, resizing, and mate-score normalization. |
| `static_exchange.cpp` | Bitboard static-exchange evaluation used by pruning and ordering. |
| `search_ordering.cpp` | History, killers, and continuation counters for move ordering. |
| `classical_evaluator.cpp` | Default evaluator: material and positional term model. |
| `nnue.cpp` | Optional Koi-native NNUE: integer forward pass, AVX2 kernels, king-bucket features. |
| `opening_book.cpp` | Optional Polyglot opening book; no book is bundled. |
| `syzygy_tablebase.cpp` | Optional Syzygy root and interior probing; no tablebase data is bundled. |
| `time_manager.cpp` | Soft/hard time budgets, move-overhead subtraction, and flag-proof ceilings. |

## Optional layers

- `src/koi/gpu/` — CUDA NNUE inference; optional, with the CPU evaluator as
  fallback.
- `src/koi/modules/` — the optional C++26 named-module partitions (`types`,
  `position`, `eval`, `tablebase`, `search`, `runtime`).
- `src/koi/detail/` — private implementation headers and units that do not
  establish a public ABI.

## Data flow

1. The UCI controller validates a `position`/`go` pair and starts a search
   session.
2. The search service snapshots the request and hands it to the runner.
3. The runner drives iterative deepening over the shared transposition table
   and, with `Threads > 1`, over Lazy SMP workers.
4. Every node evaluates through the classical evaluator or a loaded NNUE
   network, and orders moves through the shared ordering tables.
5. Completed root iterations are published back through the UCI controller,
   which is the only component that writes protocol output.
