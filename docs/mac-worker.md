# Apple Silicon (macOS arm64) worker setup

How to build, test, and measure Koi on an Apple Silicon Mac — the class of
machine the project uses for its speed gates (benchmarks, node-count
measurements, and self-play A/B runs).

## Toolchain

- macOS 14+ on Apple Silicon.
- Xcode command line tools: `xcode-select --install` (AppleClang 18+ works).
- Homebrew LLVM is optional; the documented configuration uses the default
  AppleClang with C++26 modules disabled.
- CMake 3.31+ and Ninja: `brew install cmake ninja`.

## Build

```bash
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DKOI_BUILD_MODULES=OFF -DKOI_STATIC_RUNTIME=OFF \
  -DKOI_ENABLE_GPU_NNUE=OFF -DPython3_FIND_FRAMEWORK=LAST
cmake --build build/release -j 6
```

Notes:

- `KOI_BUILD_MODULES=OFF` — AppleClang cannot build the C++26 named modules.
- `KOI_STATIC_RUNTIME=OFF` — the static runtime policy is Linux-only.
- `KOI_ENABLE_GPU_NNUE=OFF` — GPU NNUE is experimental, off by default,
  CUDA-only, and unavailable on macOS.
- `-DPython3_FIND_FRAMEWORK=LAST` avoids picking the system Python framework.
- After changing the compiler or flags, delete `build/release` before
  reconfiguring: CMake caches the compiler choice.

## Test

```bash
ctest --test-dir build/release -j 8 --output-on-failure
```

## Measuring (the speed gates)

- Fixed-depth node/time profile:

  ```bash
  build/release/koi-bench --depth 6 --threads 1 --timed --fen-file <fens.txt>
  ```

- Depth sweep with per-depth rows plus JSON reports:

  ```bash
  build/release/koi-bench --depth-sweep 1..8 --threads 1 --timed \
    --fen-file <fens.txt> --report out.json --profile-json profile.json
  ```

  `--warmup` and `--repeat` default to 0/1; `--depth` and `--depth-sweep` are
  mutually exclusive. The fen-file line format is `[name|]<FEN or move list>`.

- Self-play A/B (node-limited, engine vs engine): `tools/stability/uci_match.ps1`
  with `-KoiPath` / `-OpponentPath`; install PowerShell 7 on macOS with
  `brew install --cask powershell`.

- Reference numbers (M4, classical evaluator, Threads=1): about 1.05M
  visited nodes/s at depth 6 from the start position. The visited-node count is
  quiescence-dominated (about 98% at middlegame depth 7), so per-ply costs grow
  quickly with depth: budget minutes per middlegame position at depth 7, and
  hours for a ten-position depth-8 sweep.

## Remote worker pattern

The repository's measurement scripts drive the Mac over SSH:

1. `scp` the script and its inputs, then start it detached with a log:

   ```bash
   ssh <host> "cd ~/koi-run && nohup bash <script> > <script>.log 2>&1 &"
   ```

2. The script echoes a marker line (for example `C44-FAST-DONE`) as its last
   action.
3. A watcher on the driving side polls for the marker, then `scp`s the results
   back. Keep poll windows generous — long sweeps outlive short ones.

Gotchas:

- Non-login SSH shells do not see Homebrew on `PATH`; use the default compiler
  or absolute paths in scripts.
- macOS has no GNU `timeout`; avoid it in scripts.
- Run benches and builds on separate cores when measuring: scheduling a bench
  onto an efficiency core slows it noticeably.
- `koi-bench` buffers stdout to a file until exit; use the JSON reports or the
  marker line to detect completion rather than reading partial logs.
