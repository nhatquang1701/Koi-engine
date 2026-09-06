import os
import subprocess
import unittest
from pathlib import Path


REPOSITORY_ROOT = Path(__file__).resolve().parents[1]
CORPUS_PATH = REPOSITORY_ROOT / "tests" / "data" / "elo-openings-32.txt"
REPLAY_CANDIDATES = (
    REPOSITORY_ROOT / "build" / "task3" / "koi-replay.exe",
    REPOSITORY_ROOT / "build" / "task4" / "koi-replay.exe",
    REPOSITORY_ROOT / "build" / "koi-replay.exe",
)
REPLAY_TIMEOUT_SECONDS = 10


def read_openings():
    records = []
    for line_number, raw_line in enumerate(CORPUS_PATH.read_text(encoding="utf-8").splitlines(), 1):
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        fields = [field.strip() for field in line.split("|", 1)]
        if len(fields) != 2 or not fields[0] or not fields[1]:
            raise AssertionError(f"line {line_number} must use 'name | UCI moves' format")
        records.append((fields[0], fields[1].split()))
    return records


def replay_path():
    configured_path = os.environ.get("KOI_REPLAY_PATH")
    if configured_path:
        candidate = Path(configured_path).expanduser()
        if not candidate.is_absolute():
            candidate = REPOSITORY_ROOT / candidate
        if candidate.is_file():
            return candidate
        raise AssertionError(f"KOI_REPLAY_PATH does not point to a file: {candidate}")

    candidates = list(REPLAY_CANDIDATES)
    for build_root in (REPOSITORY_ROOT / "build", REPOSITORY_ROOT / "out"):
        if build_root.is_dir():
            candidates.extend(sorted(build_root.rglob("koi-replay.exe")))
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    raise AssertionError("koi-replay executable not found; build the koi_replay target first")


class EloOpeningCorpusTests(unittest.TestCase):
    def test_corpus_has_32_legal_named_opening_lines(self):
        records = read_openings()
        self.assertEqual(len(records), 32)
        names = [name for name, _moves in records]
        self.assertEqual(len(set(names)), len(names))
        move_sequences = [tuple(moves) for _name, moves in records]
        self.assertEqual(len(set(move_sequences)), len(move_sequences))

        replay = replay_path()
        for name, moves in records:
            try:
                completed = subprocess.run(
                    [str(replay), "startpos", "moves", *moves],
                    capture_output=True,
                    text=True,
                    check=False,
                    timeout=REPLAY_TIMEOUT_SECONDS,
                )
            except subprocess.TimeoutExpired:
                self.fail(f"{name}: koi-replay timed out after {REPLAY_TIMEOUT_SECONDS} seconds")
            self.assertEqual(completed.returncode, 0, f"{name}: {completed.stderr}")
            fields = dict(
                line.split(" ", 1)
                for line in completed.stdout.splitlines()
                if " " in line
            )
            self.assertEqual(fields.get("legal"), "1", f"{name}: {completed.stdout}")


if __name__ == "__main__":
    unittest.main()
