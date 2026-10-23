# NNUE training and promotion loop

Koi's evaluator is optional but first-class: the release packages ship a gated
network. This page shows the loop that turns self-play data into a promoted
`.nnue` file, with the exact tools in this repository.

```mermaid
flowchart TD
    DATA["tools/measurement/gen_training_data.py<br/>Stockfish self-play + fixed-depth labels"]
    TEXT["text corpus<br/>FEN;cp;best_move"]
    ENCODE["tools/measurement/koi_dataset.py encode<br/>sparse koi-dataset-v2<br/>halfka-king-bucket (+ optional threat pairs)"]
    TRAIN["tools/measurement/train_nnue_koi.py<br/>PyTorch · v4/v5/v6/v8<br/>early stop · output-shift grid"]
    FLOAT["float checkpoint<br/>.pt"]
    EXPORT["tools/measurement/export_nnue.py<br/>int16/int8 quantization<br/>metadata + sha256"]
    NET["quantized network<br/>.nnue + .metadata.json"]
    UNIT["unit validation<br/>nnue_training / nnue_wrapper tests<br/>koi-bench --nnue nodes/sec"]
    AB["tools/nnue/net_match.ps1<br/>distinct-game SPRT<br/>candidate vs official"]
    ABS["tools/measurement/elo_estimate.py<br/>--koi-eval-file anchor ladder"]
    GATE{"promotion gate<br/>SPRT target met AND<br/>equal-time Elo >= classical"}
    OFFICIAL["artifacts/networks/<br/>koi-vX-official/<br/>.nnue + .metadata.json + .sha256"]
    RELEASE["release package<br/>nnue payload + provenance"]

    DATA --> TEXT --> ENCODE --> TRAIN --> FLOAT --> EXPORT --> NET
    NET --> UNIT --> AB --> GATE
    NET --> ABS --> GATE
    GATE -- "accepted" --> OFFICIAL --> RELEASE
    GATE -- "rejected" --> DATA
    GATE -- "rejected" --> TRAIN
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

## Stages

| Stage | Tool | Output |
| --- | --- | --- |
| Data generation | `tools/measurement/gen_training_data.py` | Text corpus (`FEN;cp;best_move`). |
| Corpus encoding | `tools/measurement/koi_dataset.py encode` | Sparse `koi-dataset-v2` binary. |
| Training | `tools/measurement/train_nnue_koi.py` | Float checkpoint (`.pt`) plus training metadata. |
| Quantization | `tools/measurement/export_nnue.py` | `int16/int8` `.nnue` file and metadata JSON. |
| Unit validation | `tests/python/evaluation/*`, `koi-bench --nnue` | Fast parity and throughput checks. |
| Relative gate | `tools/nnue/net_match.ps1` | Distinct-game SPRT report. |
| Absolute gate | `tools/measurement/elo_estimate.py --koi-eval-file` | Anchor-ladder Elo estimate. |
| Promotion | `artifacts/networks/koi-vX-official/` | Network, metadata, checksum. |

## Promotion rules

- A network is promoted only when the relative SPRT accepts **and** the
  absolute estimate shows it at least matches the classical evaluator at equal
  time. A network that is weaker is rejected even if it is faster.
- Evidence is hash-tied: the report records the network SHA-256, engine
  executable, options, openings, and time control.
- The official network directory carries the `.nnue`, its metadata, and a
  `.sha256` sidecar; release packages copy that payload with provenance.
- Training changes are one variable at a time (architecture, data size, or
  shift grid), so a rejected run points at a single cause.
- **Node-limited matches cannot promote a network on their own.** A wider hidden
  layer is equal at equal nodes and loses at equal time because it doubles eval
  cost: the 1024-wide v8 candidate measured 48.75-49.00% against the 512-wide
  official net at equal nodes but only 50.78% against the classical evaluator at
  equal time, where the official net scores 62.50%. Always run the equal-time
  gate before promoting.
- The version-8 container accepts both layouts: the halfka-only 9216-input
  payload (identical bytes to v4, written by `--arch v8`) and the 36864-input
  `threat-pairs-v1` form written by `--arch v6`. The input width selects the
  layout. The threat form lost every match it entered (33.75% against the
  official net) and its export saturates the L1 head, so it is not the default.
