# Running Koi as a Lichess bot

[lichess-bot](https://github.com/lichess-bot-devs/lichess-bot) is the official
Python bridge that plays Lichess games with any UCI engine. Koi speaks the UCI
surface lichess-bot needs: clock-driven `go` commands, `stop`, in-place
`ponder`/`ponderhit` conversion, and `MultiPV`.

## Prerequisites

- A Lichess account upgraded to a bot account, plus an OAuth token with the
  `bot:play` scope (account menu: *Play with the Lichess API*).
- Python 3.9+ with `pip`.
- A Koi release package (Windows zip, Linux tarball, or macOS tarball); see the
  [repository README](../README.md) for downloads.

## Setup

1. Clone and install the bridge:

   ```bash
   git clone https://github.com/lichess-bot-devs/lichess-bot
   cd lichess-bot
   pip install -r requirements.txt
   ```

2. Copy `config.yml.default` to `config.yml` and set `token: "your-oauth-token"`.

3. Point the `engine:` section at the Koi binary and its options:

   ```yaml
   engine:
     name: "Koi Engine"
     dir: "C:/path/to/koi"        # folder that contains the engine binary
     working_dir: "C:/path/to/koi"
     protocol: "uci"
     ponder: true                  # Koi converts the running search on ponderhit
     engine_options:
       Hash: 512
       Threads: 4
       MultiPV: 1
       OwnBook: false
       EvalFile: ""                # optional NNUE file; empty keeps the classical evaluator
   ```

   Use the engine binary from the release package. On Windows
   `koi-engine.exe` selects the best CPU variant for the host automatically.

4. Allow matchmaking with the time controls you want:

   ```yaml
   matchmaking:
     allow_matchmaking: true
     challenge_timeout: 30
     time_controls: ["3+0", "3+2", "5+3", "10+0"]
     modes: ["rated", "casual"]
     variants: ["standard"]        # Koi plays standard chess only
   ```

5. Start the bot: `python lichess-bot.py`.

The `config.yml.default` shipped with lichess-bot is the source of truth for
key names in your installed version; the example above covers the stable
engine and matchmaking keys.

## Notes

- Koi plays standard chess only; leave the variants list at `standard`.
- Leave `ponder: true` unless you specifically want to disable it — Koi's
  ponderhit handling is part of its conformance test surface.
- Optional assets stay optional: set `SyzygyPath`, or `OwnBook: true` with
  `BookFile`, only when you have the files. No book or tablebase data ships
  with Koi.
- For analysis-style use (no clock, infinite search, MultiPV), a GUI is a
  better fit than a bot; see the [user guide](USER_GUIDE.md).
