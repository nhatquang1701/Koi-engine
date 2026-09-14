# Stockfish 19 (vendored reference)

This directory vendors the upstream Stockfish 19 source tree for two
non-production purposes:

- **Search guidance.** `docs/superpowers/plans/` plans reference the Stockfish 19
  source under
  `stockfish-windows-x86-64-universal/stockfish/src` when comparing search
  heuristics. Koi does not compile, link, or include any of this code.
- **Measurement oracle.** The Windows x64 executable
  `stockfish-windows-x86-64-universal/stockfish/stockfish-windows-x86-64-universal.exe`
  is used by the optional match and Elo tooling (`tools/measurement/`) as an
  external opponent. It is a local, untracked build artifact (ignored by
  `.gitignore` via `*.exe`); do not commit it.

The source tree is unmodified upstream material. Its version is identified by
the directory name (`stockfish-19`); see `stockfish-windows-x86-64-universal/stockfish/AUTHORS`
and `CITATION.cff` for upstream attribution.

Stockfish is licensed under the GNU General Public License, version 3. See
`stockfish-windows-x86-64-universal/stockfish/Copying.txt`. Because none of this
code is linked into `koi-engine`, the Koi binaries do not inherit that license,
but any redistribution of this reference tree must retain the GPLv3 terms.
