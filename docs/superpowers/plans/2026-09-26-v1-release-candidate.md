# Koi Engine v1.0.0 release candidate

Status: implementing. Baseline commit: `14e4b990a4197871d73869e104f2fe48d8694930`.

## Outcome

Prepare, but do not publish, public Windows x64 and Linux x86-64 CPU release archives. The exact packaged binaries must pass correctness, reliability, protocol, and strength gates. Deliver checksums, installation guidance, release notes, and a provenance/evidence report.

## Tasks

1. Freeze the baseline binary and source commit; inventory known failures, existing tests, and release hazards. Record decisions and evidence in the dated working summary.
2. Set project and packages to v1.0.0. Add archive checksums and source/build provenance, extracted-archive smoke checks, and corresponding packaging tests. Preserve baseline/AVX2/AVX512 CPU fallback and required licenses.
3. Stop advertising uncalibrated `UCI_LimitStrength` and `UCI_Elo`; update UCI tests and user documentation. Keep other default classical CPU options stable.
4. Fix any discovered release-blocking crash, illegal move, protocol, time-control, cancellation, or package issue with a focused regression test first.
5. Run fresh Windows/Linux Release CTest; Windows Debug and ASan; native/shadow differential, Linux no-retry flake, release verification, and extracted package UCI/GUI checks. Any unexplained failure blocks the candidate.
6. Run at least 600 recorded games across both platforms, colors, short/longer clocks, and Threads 1/4. Require zero engine crash, illegal move, missing/duplicate bestmove, or protocol timeout. Compare release against frozen baseline with tactical suite and paired matches; require a 95% bound excluding a loss worse than 10 Elo.
7. Produce v1.0.0 archives, SHA-256 checksums, release notes, and an evidence report keyed to exact archive/binary hashes. Stop before publication.

## Scope decisions

- Classical CPU engine is the release-grade default. NNUE, GPU, books, and Syzygy remain optional and are documented with requirements.
- Known search behavior XFAILs do not independently block release unless they imply illegal play, instability, protocol failure, or measured regression.
- Publication, tagging, pushing, and merging are outside this candidate-preparation task.
