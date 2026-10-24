# Running Koi as a Lichess bot

[lichess-bot](https://github.com/lichess-bot-devs/lichess-bot) is the official
Python bridge that plays Lichess games with any UCI engine. Koi speaks the UCI
surface lichess-bot needs: clock-driven `go` commands, `stop`, in-place
`ponder`/`ponderhit` conversion, and `MultiPV`.

`tools/bot/koi-bot.sh` wraps the whole setup so a bot can be installed, started,
stopped, and updated with single commands. It is written for macOS and is
verified on Apple Silicon; the same layout works on Linux with a different
release asset.

## Prerequisites

- A Lichess account upgraded to a **bot account**, plus an OAuth token with the
  `bot:play` scope (account menu: *Play with the Lichess API*). A token that is
  only allowed to play bot games is fine — that is the normal bot scope.
- **Python 3.11 or newer** (lichess-bot refuses older interpreters). macOS ships
  3.9/3.10, so build or install a newer one first; the script records whichever
  interpreter it used in `~/koi-bot/python-bin`.
- `git`, `curl`, and a Koi release for the host platform.

## Install

```bash
# 1. put the control script and the token in place (the token never leaves the host)
install -d -m 700 ~/koi-bot
cp tools/bot/koi-bot.sh ~/koi-bot/ && chmod +x ~/koi-bot/koi-bot.sh
printf '%s' 'lip_...' > ~/koi-bot/token
chmod 600 ~/koi-bot/token

# 2. install (clones the bridge, makes the venv, installs the newest Koi release)
~/koi-bot/koi-bot.sh install

# 3. start / stop / inspect
~/koi-bot/koi-bot.sh start
~/koi-bot/koi-bot.sh status
~/koi-bot/koi-bot.sh stop
```

`install` verifies the release package against its `.sha256` sidecar, installs
`koi-engine` plus the shipped `.nnue` under `~/koi-bot/engines`, and writes
`~/koi-bot/config.yml` (mode 600) pointing `EvalFile` at that net.

## Commands

| Command | Effect |
| --- | --- |
| `koi-bot.sh install [--token T]` | clone/refresh the bridge, build the venv, install the newest engine, write the config |
| `koi-bot.sh start` | run the bot (launchd when installed, otherwise a pidfile) |
| `koi-bot.sh stop` | stop the bot and its engine processes |
| `koi-bot.sh restart` | stop, then start |
| `koi-bot.sh status` | engine version, net, service state, last log lines |
| `koi-bot.sh logs [N]` | tail the bot log |
| `koi-bot.sh update [--force]` | install the newest release and restart only when the tag changed |
| `koi-bot.sh install-launchd` | start at login, restart on crash, check for a new release hourly |
| `koi-bot.sh uninstall-launchd` | remove those agents |

## Staying on the newest release

`update` asks GitHub for `releases/latest`, compares the tag with the installed
`~/koi-bot/engines/VERSION`, and does nothing when they match. When a newer
release exists it downloads the macOS arm64 package, verifies the sidecar hash,
swaps the binary and net, rewrites the config, and restarts the bot.

`install-launchd` registers two agents:

- `org.koi.lichess-bot` — runs the bot at login and restarts it after a crash;
- `org.koi.lichess-bot-update` — runs `update` every hour.

So every minor update or patch published on the repository reaches the bot
within the hour, with no manual step. Run `koi-bot.sh uninstall-launchd` to go
back to manual control.

## Layout

```
~/koi-bot/
  koi-bot.sh          # this control script
  token               # OAuth token, mode 600
  config.yml          # generated, mode 600 - edits are overwritten
  python-bin          # interpreter used for the venv
  engines/koi-engine  # release binary
  engines/koi.nnue    # release network
  engines/VERSION     # installed release tag
  lichess-bot/        # upstream clone + .venv
  logs/bot.log        # bot output
  logs/update.log     # updater output
  games/              # PGN records of the bot's games
```

## Engine options used

The generated config sends only options Koi defines:

```yaml
uci_options:
  Threads: 4
  Hash: 512
  UCI_ShowWDL: true
  EvalFile: "/Users/<you>/koi-bot/engines/koi.nnue"
```

`EvalFile` is an absolute path on purpose: the engine resolves the network
against its own working directory, and a relative path silently falls back to
the classical evaluator. `move_overhead: 2000` in the config leaves the engine a
buffer on the Lichess clock.

## Opening book

Release packages from 1.1.4 carry a Polyglot book generated from Lichess's
open-source opening data ([lichess-org/chess-openings](https://github.com/lichess-org/chess-openings),
CC0 1.0). Copy it next to the engine binary as `book.bin`:

```bash
cp book/koi.bin ~/koi-bot/engines/book.bin
```

A book found beside the executable is used **automatically** — the engine
reports `info string book detected: …` and plays the book move without any
option. Setting `OwnBook=false` from a GUI disables it again, and `BookFile`
points at a different book. `BookDepth` (default 16), `BookRandom`, and
`BookSafety` tune how it is used.

Because the book is used before the search starts, it also saves clock time in
the opening, which matters most in short games.

## Notes and troubleshooting

- **`Unexpected engine output: 'info string NNUE enabled from …'`** — harmless.
  Koi reports the loaded network on `setoption`; lichess-bot logs unknown
  `info string` lines as a warning and keeps playing.
- **The bot plays without the net** — check `EvalFile` in `config.yml` is an
  absolute path that exists, then `koi-bot.sh logs` for the engine's own
  `info string EvalFile rejected: …`.
- **`unrecognized arguments: -c`** — older/newer lichess-bot versions spell the
  config flag `--config`; the script uses the current spelling.
- Koi plays standard chess only; leave the variants list at `standard`.
- Optional assets stay optional: only set `SyzygyPath`, or `OwnBook: true` with
  `BookFile`, when you have the files. Release packages carry the CC0 opening book `book/koi.bin`; Syzygy tablebase data is never bundled.
- For analysis-style use (no clock, infinite search, MultiPV), a GUI is a better
  fit than a bot; see the [user guide](USER_GUIDE.md).
- The bridge is AGPLv3; it is cloned at setup time and never vendored here.
