# Koi Engine documentation map

Start with the [documentation landing page](index.md) for installation, GUI
setup, UCI use, developer guidance, and release notes. The
repository [README](../README.md) is the concise project entry point; the
[user guide](USER_GUIDE.md) contains the detailed build, UCI, GUI, optional
asset, and tooling reference.

- [AGENTS.md](../AGENTS.md) gives contributors and coding agents the stable
  architecture contracts, source map, test workflow, artifact policy, and
  release guidance.
- [lichess-bot.md](lichess-bot.md) covers running Koi as a Lichess bot.
- [mac-worker.md](mac-worker.md) documents the Apple Silicon build, test, and
  measurement worker setup.
- [ARCHITECTURE.md](ARCHITECTURE.md) maps the runtime modules to the files under
  `src/koi`.
- [NNUE_PIPELINE.md](NNUE_PIPELINE.md) shows the training and promotion loop for
  the optional network.
- [releases/](releases/v1.1.4.md) contains published release notes.
- [history/](history/removed-nnue-studio.md) records tools that were removed and
  why.
- `../tests/README.md` explains test categories and fixture locations.
- `../tools/README.md` explains developer tools and their artifact policy.

Generated builds belong under `../build/`; generated reports belong under
`../artifacts/`. Neither directory is committed.
