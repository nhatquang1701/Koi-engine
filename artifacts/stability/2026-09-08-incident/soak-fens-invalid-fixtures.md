# Stability soak fixture exclusion

The original `soak-fens.txt` is retained unchanged as historical evidence.

The `kiwipete` record was excluded from the valid Cutechess soak because Stockfish 19 rejects it during `position fen` with:

`CRITICAL ERROR: Command ... failed. Reason: Invalid FEN. More than 32 pieces on the board.`

The position contains 33 pieces (two black queens), so it is not a valid standard-FIDE game start for an engine-vs-engine soak. Koi accepted it and produced a legal internal root, but Cutechess terminated Stockfish before any move and recorded a zero-ply disconnect. This is an external fixture failure, not accepted evidence of a Koi move or protocol failure.

The replacement soak input is `soak-fens-valid.txt`, which excludes only that invalid record. The original failed run remains at `soak-t1-12`.
