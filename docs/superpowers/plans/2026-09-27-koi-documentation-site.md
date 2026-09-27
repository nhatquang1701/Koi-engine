# Koi Engine documentation site implementation plan

> **For agentic workers:** Use `superpowers:executing-plans` or
> `superpowers:subagent-driven-development` task by task. Track progress with
> checkboxes and preserve unrelated user files.

**Goal:** Publish all tracked Koi-authored Markdown as a searchable, developer-
focused GitHub Pages site while keeping the root README concise and providing
agents with a detailed, maintained `AGENTS.md`.

**Architecture:** Stage tracked Markdown into an ignored build directory while
preserving repository-relative paths. MkDocs Material renders a custom landing
page, user/developer navigation, and a historical archive; a GitHub Actions
workflow builds and deploys the static site to the existing project Pages URL.
The staging command copies the committed root `AGENTS.md` into the site corpus
so the site and agent guide stay aligned without maintaining separate copies.

**Tech Stack:** Markdown, Python, MkDocs Material, GitHub Actions Pages
deployment.

**Spec:** `docs/superpowers/specs/2026-09-27-documentation-site-design.md`

## Global Constraints

- Keep engine runtime, release package manifests, and release assets unchanged.
- Work on `koi-engine`; do not create a feature branch.
- Preserve the untracked `docs/superpowers/verification/2026-09-26-repository-exploration-working-summary.md`; never stage or publish it.
- Include all tracked Koi-authored Markdown, including the historical specs, plans, and verification archive; exclude vendored third-party docs, source code, build output, and generated artifacts.
- Pin the documentation generator version and make the staging script use `git ls-files`, not an unrestricted recursive walk.
- Use the project Pages base URL `https://nhatquang1701.github.io/Koi-engine/`.
- Deploy only after a strict local MkDocs build and corpus/link checks succeed.
- Commit the completed work on `koi-engine`; publish through Pages Actions as requested.

## Review Focus

- The project-site base path is `/Koi-engine/`; links, assets, and search must work at that prefix and in a local preview.
- The untracked exploration summary and ignored artifacts must not enter the staged Pages corpus.
- All tracked Koi-authored Markdown, especially historical specs/plans/verification files, must be present in the archive navigation.
- Relative links in root and nested Markdown must resolve after staging while preserving the source files.
- Pages uses legacy branch-root publishing today; the workflow must switch deployment to GitHub Actions with minimal permissions and no competing deploy source.

## Files and Responsibilities

- `README.md` — concise engine description, release link, and documentation links.
- `docs/USER_GUIDE.md` — detailed player, UCI, GUI, build, and advanced-feature reference moved from the old root README.
- `AGENTS.md` — detailed architecture, invariants, source map, build/test workflow, artifact policy, and contribution/release rules.
- `docs/index.md` and selected `docs/*.md` pages — landing page and focused user/developer overview pages.
- `docs/superpowers/**` — historical archive source, retained and surfaced in the site.
- `docs/superpowers/specs/README.md` and `docs/superpowers/plans/README.md` — keep document indexes current.
- `mkdocs.yml` and `requirements-docs.txt` — pinned renderer, theme, base URL, search, and navigation.
- `tools/docs/prepare_pages.py` — stages tracked project Markdown and generates a categorized navigation manifest under ignored `build/` output.
- `.github/workflows/docs.yml` — strict site build and Pages artifact deployment.

## Tasks

### Task 1: Establish the content structure

**Files:**
- Modify: `README.md`
- Create: `docs/USER_GUIDE.md`
- Create: `AGENTS.md`
- Create: `docs/index.md`
- Modify: `docs/README.md`
- Modify: `docs/superpowers/specs/README.md`
- Modify: `docs/superpowers/plans/README.md`

- [x] Move the long user-facing material from the current README into
  `docs/USER_GUIDE.md`, preserving GUI, UCI, NNUE, GPU, book, tablebase, build,
  and tooling guidance with correct relative links.
- [x] Replace `README.md` with a concise engine description, release/download
  links, short getting-started section, and links to the user guide and site.
- [x] Write `AGENTS.md` with stable architecture contracts, file map, build/test
  commands, data/artifact policies, and contributor/release guidance.
- [x] Add `docs/index.md` for the developer-focused site landing and navigation
  into user, developer, reference, and archive content.
- [x] Update documentation maps to expose the new user guide, agent guide, and
  site design/plan.

### Task 2: Build the tracked Markdown site corpus

**Files:**
- Create: `tools/docs/prepare_pages.py`
- Create: `mkdocs.yml`
- Create: `requirements-docs.txt`

- [x] Implement staging from `git ls-files '*.md'`, preserving source-relative
  paths, copying the root README and AGENTS guide, and excluding third-party
  Markdown and non-document assets.
- [x] Generate a stable MkDocs navigation grouped by Start Here, Using Koi,
  Developing Koi, Reference, and Project Archive; include every eligible tracked
  Markdown file in the archive/search corpus.
- [x] Configure MkDocs Material search, responsive sidebar, code-copy, light/dark
  palette, and the project Pages base URL.
- [x] Keep staged inputs and generated site output under ignored `build/` paths.

### Task 3: Deploy the site through GitHub Pages Actions

**Files:**
- Create: `.github/workflows/docs.yml`

- [x] Add a workflow for pushes to `koi-engine` and manual dispatch that installs
  pinned documentation dependencies, stages tracked Markdown, and runs
  `mkdocs build --strict`.
- [x] Upload only the generated static site and deploy it through the official
  GitHub Pages Actions flow with `contents: read`, `pages: write`, and
  `id-token: write` permissions.
- [x] Use a deployment concurrency group to prevent stale competing builds.

### Task 4: Review and deploy

- [x] Build the staged site locally in strict mode.
- [x] Check that the corpus includes every tracked Koi Markdown file intended by
  the spec and excludes third-party and untracked files.
- [x] Open the local build in a browser and review landing, sidebar, search,
  archive navigation, and a nested relative link.
- [ ] Verify the Pages workflow source is Actions, commit on `koi-engine`, push
  the requested site change, and confirm the project Pages URL serves the new
  landing page.

## Verification Notes

- The site build is the acceptance gate; no engine C++ tests are required because
  this plan does not modify runtime code.
- Generated `build/pages-src` and `build/pages-site` are disposable and must
  remain uncommitted.
- A deployment check must confirm the Pages URL uses the `/Koi-engine/` project
  prefix and that no untracked summary or artifacts are present in the built
  site.
