# Generated artifacts

This directory is the repository-local home for reports, match logs, packages,
and verification evidence. Only small text evidence is tracked in Git; large
generated files stay untracked and local.

## What is tracked

Small text evidence, up to **256 KiB per file**, with extensions `.md`,
`.json`, `.txt`, `.log`, `.csv`, and `.sha256`. This covers README/navigation
files, manifests and hashes, training metadata and logs, speed-gate and
verification reports, Elo reports, and similar provenance.

The directory itself is still ignored by Git (`/artifacts/*` in `.gitignore`),
so new evidence is added deliberately with `git add -f`. Add a file only when
it fits the policy above; keep everything else local.

## What stays untracked

Networks (`*.nnue`, `*.pt`), corpora and datasets, PGN game archives, release
packages, executables, and any file larger than the 256 KiB cap. These are
reproducible or too large for review; their provenance lives in the tracked
metadata, manifests, and `.sha256` sidecars instead.

## Layout

| Directory | Contents |
| --- | --- |
| `baselines/` | reproducible benchmark and test baselines |
| `elo-*/` | anchor-sweep Elo runs and their reports |
| `manifests/` | hashes, provenance, and migration inventories |
| `matches/` | UCI and engine-match reports (PGNs stay local) |
| `networks/` | promoted networks and their metadata sidecars |
| `packages/` | locally built release packages |
| `probes/` | engine probe batteries and raw outputs |
| `stability/` | match runs and incident bundles |
| `training/` | corpora, trained networks, and run logs (binaries stay local) |
| `verification/` | release, performance, and compatibility reports |

Tools create these directories automatically. Do not place build trees or
third-party databases here.
