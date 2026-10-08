# Final Whole-Branch Review Fix Report

Date: 2026-09-06
Reviewed head: `88bcd65`

## Findings addressed

- The Polyglot loader now rejects record-aligned files larger than 16 MiB before
  allocating its cache. The 16 MiB cap is intentionally conservative because
  each 16-byte input record expands into an unordered-map node and vector
  capacity. Filesystem, stream, and cache allocation/load failures now produce
  an unusable cache or no-book result while the existing executable-relative
  resolution, valid-book parsing, deterministic selection, and mutex protection
  remain unchanged.
- The README configuration summary now lists every default advertised by the
  UCI handshake, including the newer analysis, book-safety, strength, and
  Syzygy options.

## TDD evidence

### RED

Command:

```powershell
& 'C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\amd64\MSBuild.exe' .\out\resume-task5-vs\opening_book_tests.vcxproj /t:Build /p:Configuration=Debug /p:Platform=x64 /m:2 /nologo
& .\out\resume-task5-vs\Debug\opening_book_tests.exe
```

Result: build succeeded; the new test failed as expected:
`FAIL oversized book fallback: an oversized book must be rejected before selection`.
The pre-fix loader parsed the oversized file and selected its valid first record.

### GREEN

Command:

```powershell
& 'C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\amd64\MSBuild.exe' .\out\resume-task5-vs\opening_book_tests.vcxproj /t:Build /p:Configuration=Debug /p:Platform=x64 /m:2 /nologo
& .\out\resume-task5-vs\Debug\opening_book_tests.exe
```

Result: build succeeded with 0 warnings and 0 errors; all 7 opening-book
tests passed, including `PASS oversized book fallback`.

No direct allocation-failure test was added because the existing architecture
does not provide a practical deterministic allocation-failure injection point;
the bounded rejection test covers the required safety path and existing missing
and malformed fallback cases remain in the same test surface.

## Relevant verification

Commands:

```powershell
& 'C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\amd64\MSBuild.exe' .\out\resume-task5-vs\uci_controller_tests.vcxproj /t:Build /p:Configuration=Debug /p:Platform=x64 /m:2 /nologo
& 'C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\amd64\MSBuild.exe' .\out\resume-task5-vs\koi_engine.vcxproj /t:Build /p:Configuration=Debug /p:Platform=x64 /m:2 /nologo
& 'C:\msys64\ucrt64\bin\ctest.exe' --test-dir .\out\resume-task5-vs -C Debug -R 'opening_book_tests|uci_controller_tests|koi_engine_process|koi_engine_en_croissant_process' --output-on-failure
git diff --check
```

Results: both MSBuild targets succeeded with 0 warnings and 0 errors. CTest
reported `100% tests passed out of 4`:

- `opening_book_tests`
- `uci_controller_tests`
- `koi_engine_process`
- `koi_engine_en_croissant_process`

`git diff --check` passed. The protected untracked user-input paths were not
modified.
