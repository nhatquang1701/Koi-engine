# NNUE training and promotion loop

Koi's evaluator is optional but first-class: the release packages ship a gated
network. This page shows the loop that turns self-play data into a promoted
`.nnue` file, with the exact tools in this repository.

```mermaid
flowchart TD
    DATA["tools/measurement/gen_training_data.py<br/>Stockfish self-play + fixed-depth labels"]
    TEXT["text corpus<br/>FEN;cp;best_move"]
    ENCODE["tools/measurement/koi_dataset.py encode<br/>sparse koi-dataset-v2<br/>halfka-king-bucket + threat pairs"]
    TRAIN["tools/measurement/train_nnue_koi.py<br/>PyTorch · v4/v5/v6<br/>early stop · output-shift grid"]
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
