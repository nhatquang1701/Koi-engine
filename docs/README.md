# Koi Engine documentation map

Start with the [documentation landing page](index.md) for installation, GUI
setup, UCI use, developer guidance, releases, and the project archive. The
repository [README](../README.md) is the concise project entry point; the
[user guide](USER_GUIDE.md) contains the detailed build, UCI, GUI, optional
asset, and tooling reference.

- [AGENTS.md](../AGENTS.md) gives contributors and coding agents the stable
  architecture contracts, source map, test workflow, artifact policy, and
  release guidance.
- [releases/](releases/v1.0.0.md) contains published release notes.
- The historical project archive contains [specifications](superpowers/specs/README.md),
  [plans](superpowers/plans/README.md), and
  [verification records](superpowers/verification/README.md).

- [superpowers/plans/](superpowers/plans/README.md) contains implementation
  plans and historical execution records. Its paths are intentionally stable.
- [superpowers/specs/](superpowers/specs/README.md) contains design specifications.
- [superpowers/verification/](superpowers/verification/README.md) contains stage
  verification and final-review records.
- `../tests/README.md` explains test categories and fixture locations.
- `../tools/README.md` explains developer tools and their artifact policy.
- `../artifacts/README.md` explains generated evidence stored in the checkout.

The [documentation site design](superpowers/specs/2026-09-27-documentation-site-design.md)
and [implementation plan](superpowers/plans/2026-09-27-koi-documentation-site.md)
describe the tracked Markdown Pages corpus and navigation.

Generated builds belong under `../build/`; generated reports belong under
`../artifacts/`. Neither directory is committed.
