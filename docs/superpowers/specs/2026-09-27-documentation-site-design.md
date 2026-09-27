# Koi Engine documentation site design

**Status:** Approved direction; written for review before implementation planning.

## Purpose

Build a developer-focused, human-readable documentation site for Koi Engine at
`https://nhatquang1701.github.io/Koi-engine/`. Keep the repository root
`README.md` as a concise description of the engine, and make the site the main
place to browse usage, developer guidance, and project history.

The user approved MkDocs Material with GitHub Actions and asked to include
tracked historical specifications, plans, and verification records in a
separate archive section.

## References reviewed

- [Stockfish website](https://stockfishchess.org/) separates the engine
  introduction and downloads from its documentation, support, and contribution
  paths.
- [Stockfish documentation](https://official-stockfish.github.io/docs/stockfish-wiki/Home.html)
  uses a searchable sidebar with user guides and developer references grouped
  by project.
- [Lc0 website](https://lczero.org/) presents a short engine description with
  distinct Play, Watch, Contribute, Development, and Blog paths.
- [Lc0 developer documentation](https://lczero.org/dev/) groups architecture
  and search-algorithm material and separates outdated documents from current
  references.

Use these navigation patterns as references without copying their text, images,
or branding.

## Goals

- Publish all tracked Koi-authored Markdown documentation, including the
  historical `docs/superpowers/specs`, `plans`, and `verification` records.
- Give players a clear entry point for downloads, installation, UCI setup,
  optional assets, and troubleshooting.
- Give developers a navigable guide for architecture, building, tests, tools,
  and contributing.
- Keep the landing page compact and readable on mobile and desktop.
- Provide page search and a grouped sidebar.
- Keep `AGENTS.md` as a longer, explicit continuation guide aligned with the
  documentation site, with additional source-level contracts and workflow
  details for coding agents.
- Preserve existing Markdown as the content source where practical so normal
  repository edits remain the way documentation is maintained.

## Information architecture

The site has a custom landing page and these navigation groups:

1. **Start here:** engine overview, downloads, installation, and first UCI
   session.
2. **Using Koi:** configuration, UCI behavior, opening books, NNUE/GPU, and
   Syzygy.
3. **Developing Koi:** architecture, build prerequisites, testing, developer
   tools, artifacts, and contributing.
4. **Reference:** release notes and repository documentation maps.
5. **Project archive:** tracked specifications, implementation plans, and
   verification records, organized by category and date.

The main site navigation should prioritize the first three groups. Historical
records remain searchable and reachable under the archive group without
dominating the landing page.

The root `README.md` remains an engine description and fast repository entry
point. It links to the Pages site and release downloads. `AGENTS.md` is the more
detailed agent/maintainer reference for the same concepts: source map,
architecture and runtime invariants, build/test workflow, artifact handling,
and release conventions. The Pages site should also expose the agent guide as a
developer reference.

## Content corpus and privacy boundary

Create the Pages content staging directory from `git ls-files` rather than a
recursive filesystem walk. Include project-authored Markdown such as the root
README, AGENTS guide, contributing guide, `docs/`, and the README maps under
`tests/`, `tools/`, and `artifacts/`. Exclude vendored third-party Markdown,
source code, binaries, build output, generated artifacts, and any untracked
local files.

The user's untracked repository-exploration summary must remain untracked and
must never enter the staged Pages corpus. This same tracked-file allowlist
keeps local match reports, networks, books, tablebases, and other artifacts out
of the published site.

Preserve repository-relative paths in the staging tree where possible so
Markdown links continue to resolve. The staging step may rewrite link targets
when moving root documents into the site, but it must not modify source
documents just to satisfy the site generator.

## Build and deployment

- Use MkDocs Material for Markdown rendering, responsive navigation, search,
  code-copy controls, and light/dark palettes.
- Pin the documentation build dependency and use the repository's current
  GitHub Actions conventions.
- Add a documentation preparation command that stages tracked Markdown into an
  ignored build directory and generates the navigation structure.
- Build strictly so missing pages or broken references fail the docs job.
- Deploy with the GitHub Pages Actions workflow and least-privilege
  `contents: read`, `pages: write`, and `id-token: write` permissions.
- Use a concurrency group to prevent stale concurrent deployments.
- Keep the Pages base URL set to the project site path and ensure local preview
  works at `/` without hard-coded production-only links.

GitHub Pages is enabled on the `koi-engine` branch using legacy root publishing
at design time. The implementation will move the Pages build source to GitHub
Actions, as selected. The site URL remains
`https://nhatquang1701.github.io/Koi-engine/`.

## Agent guide

`AGENTS.md` is a standard, clearly named repository context file, not an
obfuscated prompt. Keep it longer than the landing page and more detailed than
the relevant developer pages. It documents stable contracts and contributor
workflows; it does not contain one-time release progress, secrets, or randomized
text.

The site presents the AGENTS guide as a developer reference. The build should
stage the same committed file rather than maintaining an unrelated copy.

## Non-goals

- Do not change engine behavior, release binaries, package manifests, or runtime
  defaults as part of the docs site.
- Do not copy third-party engine documentation into the Koi site; link to
  upstream projects where useful.
- Do not publish untracked local summaries, generated game evidence, CI logs,
  training networks, book files, or tablebase data.
- Do not reproduce Stockfish or Lc0 text, artwork, logos, colors, or branding.

## Acceptance criteria

- The Pages build contains all tracked Koi-authored Markdown, including the
  historical archive, and excludes untracked files and generated artifacts.
- Navigation groups user, developer, reference, and archive material; search
  returns pages from each group.
- The landing page describes Koi accurately, links downloads and the user
  guide, and reads well at mobile and desktop widths.
- `README.md` focuses on the engine description and links to the documentation
  site.
- `AGENTS.md` gives agents deeper context than the site while remaining aligned
  with the published developer documentation.
- The static build succeeds in strict mode and the GitHub Actions deployment
  publishes to the project Pages URL.
