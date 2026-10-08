# Official trained Koi v5 NNUE nets

These files are REAL trained nets exported by tools/measurement/train_nnue_koi.py
(low output shifts 4..8). They are NOT the synthetic fixtures that live in
artifacts/networks/full-release/ - never confuse the two.

- koi-v5-official-*.nnue - exported container, validated material-aware in-engine
  (up a pawn scored >= 100 cp with build/roadmap-visit-win-release/koi-engine.exe).
- *.sha256 - integrity sidecar.
- *.metadata.json - training/quantization metadata from the exporter.
