# Koi Engine v1

Koi Engine v1 is a Windows x64 UCI chess engine for standard chess. It is
written in C++26 and currently uses a random legal-move placeholder strategy:
it chooses uniformly from the legal moves in the current position. It is not a
search-strength engine.

## Build prerequisites

- A C++26-capable x64 MSVC toolchain (the current CMake configuration selects
  Visual Studio's `/std:c++latest` compiler mode). Run CMake from an x64
  Native Tools Command Prompt or x64 Developer PowerShell for Visual Studio,
  so `cl.exe` is selected; the currently available MinGW/GCC/Clang toolchains
  do not meet the C++26 requirement.
- CMake 3.31 or newer.
- Ninja.

From an x64 Visual Studio developer shell in the repository root:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=cl
cmake --build build --config Release
```

The resulting executable is `build\koi-engine.exe`.

## UCI smoke test

Run this PowerShell transcript after building:

```powershell
$engine = (Resolve-Path .\build\koi-engine.exe).Path
@(
    'uci'
    'isready'
    'position startpos'
    'go'
    'quit'
) | & $engine
```

The output should include, in order, the engine identification lines, the
`RandomSeed` option declaration, `uciok`, `readyok`, and one legal coordinate
notation `bestmove` from the starting position (for example, `bestmove e2e4`).

## Supported UCI behavior

- `uci` reports the engine identity and the `RandomSeed` spin option.
- `isready` responds immediately with `readyok`.
- `ucinewgame` resets the position; `position startpos` and `position fen ...`
  set a position, optionally followed by legal UCI moves.
- `setoption name RandomSeed value 0` uses runtime randomness (`RandomSeed 0`).
  A nonzero seed is repeatable, so the same position and seed produce the same
  choice.
- `go` chooses and reports one legal move immediately; search limits are not
  used by this v1 placeholder.
- `stop` is accepted as a no-op because `go` completes immediately in v1.
- A terminal position with no legal moves returns `bestmove 0000`.
- `quit` exits the process.

## Register in Lucas Chess

1. Build the engine and resolve the path to `koi-engine.exe`.
2. In Lucas Chess, open the engine-management or configuration dialog and add
   the built executable as an external UCI engine.
3. Save the engine configuration, then select Koi Engine for play.

Menu labels can vary by Lucas Chess version. Use an absolute executable path,
or another path that remains valid when Lucas Chess starts the engine.

Before registering it, check the process transcript: stdout should contain
only valid UCI responses, with no logging or diagnostics mixed into it. For
the smoke test, also confirm that stderr is empty for a valid transcript. If
Lucas Chess cannot start the engine, verify the executable path, that the
Windows x64 build exists, and that the process can complete the `uci` / `isready`
handshake from PowerShell.

## References

- [Lucas Chess](https://lucaschess.pythonanywhere.com/)
- [Universal Chess Interface (UCI) reference](https://www.shredderchess.com/chess-info/features/uci-universal-chess-interface.html)
- [Disservin/chess-library](https://github.com/Disservin/chess-library)
- [C++ compiler support for C++26](https://en.cppreference.com/w/cpp/compiler_support/26)
