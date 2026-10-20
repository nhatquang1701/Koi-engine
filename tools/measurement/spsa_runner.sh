#!/bin/bash
# SPSA variant runner (macOS): rebuild a staged variant and play a short
# node-limited match against the reference binary, then write score.json.
#
# Usage: spsa_runner.sh <dir> <tag> <params-json> [games]
# Environment:
#   SPSA_REFERENCE  reference engine binary (default ~/koi-run/koi-engine-spsa-base)
#   SPSA_OPENINGS   opening corpus (default tests/data/openings/openings-release-strength-160.txt)
#   SPSA_NODES      nodes per move (default 10000)
set -euo pipefail
if [ "$#" -lt 3 ]; then
    echo "usage: spsa_runner.sh <dir> <tag> <params-json> [games]" >&2
    exit 2
fi
dir=$1
tag=$2
games=${4:-50}
root=$(cd "$(dirname "$0")/../.." && pwd)
cd "$root"

reference=${SPSA_REFERENCE:-$HOME/koi-run/koi-engine-spsa-base}
openings=${SPSA_OPENINGS:-tests/data/openings/openings-release-strength-160.txt}
nodes=${SPSA_NODES:-10000}
half=$(( games / 2 ))
if [ "$half" -lt 1 ]; then
    half=1
fi
if [ ! -x "$reference" ]; then
    echo "reference engine is missing: $reference" >&2
    exit 3
fi

git checkout -- src/ 2>/dev/null || true
if [ -d "$dir/src" ]; then
    cp -R "$dir/src/." src/
fi
cmake --build build/release -j 6 > "$dir/build.log" 2>&1

head -n "$half" "$openings" > "$dir/openings.txt"

for color in white black; do
    pwsh -NoProfile -File tools/stability/uci_match.ps1 \
        -KoiPath build/release/koi-engine \
        -OpponentPath "$reference" \
        -KoiColor "$color" \
        -Games 1 \
        -Nodes "$nodes" \
        -CompactSearchInfo \
        -ReplayPath build/release/koi-replay \
        -OpeningFile "$dir/openings.txt" \
        -OutputDirectory "$dir/match-$color" > "$dir/match-$color.log" 2>&1
done

python3 - "$dir/match-white" "$dir/match-black" "$dir/score.json" <<'PY'
import json
import pathlib
import sys

scores = {"1-0": 1.0, "0-1": 0.0, "1/2-1/2": 0.5}
total = 0.0
count = 0
for match_dir in sys.argv[1:3]:
    files = sorted(pathlib.Path(match_dir).glob("*.json"))
    if not files:
        continue
    document = json.loads(files[-1].read_text())
    color = str(document.get("configuration", {}).get("koi_color", "white")).lower()
    for game in document.get("games", []):
        result = game.get("result")
        if result in scores:
            value = scores[result]
            total += value if color == "white" else 1.0 - value
            count += 1
pathlib.Path(sys.argv[3]).write_text(
    json.dumps({"score": total / count if count else 0.5, "games": count})
)
PY
echo "VARIANT-SCORE $tag $(cat "$dir/score.json")"
