import json
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

try:
    import chess
except ImportError:  # pragma: no cover - optional dependency
    chess = None

if chess is None:  # pragma: no cover - optional dependency
    raise unittest.SkipTest("python-chess is unavailable")

ROOT = Path(__file__).resolve().parents[3]
TOOL = ROOT / "tools" / "measurement" / "koi_dataset.py"
sys.path.insert(0, str(TOOL.parent))
import koi_dataset  # noqa: E402

# Pinned golden list for the white-to-move start position (bucket 0).
STARTPOS_INDICES = [
    8, 9, 10, 11, 12, 13, 14, 15,
    65, 70,
    130, 133,
    192, 199,
    259,
    324,
    432, 433, 434, 435, 436, 437, 438, 439,
    505, 510,
    570, 573,
    632, 639,
    699,
    764,
]

ROWS = [
    ("4k3/8/8/8/8/8/P7/4K3 w - - 0 1", 12),
    ("4k3/8/8/8/8/8/8/4K3 b - - 0 1", -8),
    ("r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1", 25),
]


def write_corpus(path: Path, rows) -> None:
    path.write_text("".join(f"{fen};{cp};e2e4\n" for fen, cp in rows), encoding="utf-8")


def run_tool(*arguments: str):
    return subprocess.run(
        [sys.executable, str(TOOL), *arguments],
        cwd=ROOT,
        text=True,
        capture_output=True,
        check=False,
    )


def read_records(data: bytes):
    header = koi_dataset.DatasetEncoder.read_header_bytes(data)
    records = []
    offset = koi_dataset.HEADER_BYTES
    for _ in range(header["records"]):
        count = struct.unpack_from("<H", data, offset)[0]
        offset += 2
        indices = list(struct.unpack_from(f"<{count}H", data, offset))
        offset += 2 * count
        score = struct.unpack_from("<i", data, offset)[0]
        offset += 4
        records.append((indices, score))
    return header, records, offset


class KoiDatasetTest(unittest.TestCase):
    def test_startpos_indices_match_pinned_golden_list(self):
        indices = koi_dataset.halfka_king_bucket_indices(chess.Board())
        self.assertEqual(indices, STARTPOS_INDICES)
        self.assertEqual(len(indices), len(set(indices)))
        self.assertTrue(all(0 <= index < koi_dataset.INPUT_UNITS for index in indices))

    def test_black_startpos_mirrors_to_the_same_indices(self):
        board = chess.Board("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR b KQkq - 0 1")
        self.assertEqual(koi_dataset.halfka_king_bucket_indices(board), STARTPOS_INDICES)

    def test_king_bucket_uses_mirrored_file_and_rank_zone(self):
        cases = {
            "7k/8/8/8/8/8/8/K7 w - - 0 1": 0,
            "7k/8/8/8/8/8/8/7K w - - 0 1": 3,
            "7k/8/8/8/K7/8/8/8 w - - 0 1": 4,
            "7k/4K3/8/8/8/8/8/8 w - - 0 1": 8,
        }
        for fen, expected in cases.items():
            with self.subTest(fen=fen):
                self.assertEqual(koi_dataset.king_bucket(chess.Board(fen)), expected)

    def test_output_bucket_uses_piece_count(self):
        self.assertEqual(koi_dataset.output_bucket(chess.Board()), 0)
        self.assertEqual(
            koi_dataset.output_bucket(chess.Board("4k3/8/8/8/8/8/8/4K2R w - - 0 1")), 7
        )

    def test_encode_round_trip_is_deterministic(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            corpus = directory / "labels.txt"
            write_corpus(corpus, ROWS)
            first = directory / "first.bin"
            second = directory / "second.bin"
            for output in (first, second):
                result = run_tool("encode", "--input", str(corpus), "--output", str(output))
                self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(first.read_bytes(), second.read_bytes())

            data = first.read_bytes()
            header, records, consumed = read_records(data)
            self.assertEqual(header["magic"], "KOI-DATA")
            self.assertEqual(header["version"], 1)
            self.assertEqual(header["feature_set"], "halfka-king-bucket-v1")
            self.assertEqual(header["records"], len(ROWS))
            self.assertEqual(consumed, len(data))
            for (indices, score), (fen, cp) in zip(records, ROWS):
                self.assertEqual(indices, koi_dataset.halfka_king_bucket_indices(chess.Board(fen)))
                self.assertEqual(score, cp)

    def test_encode_skips_malformed_and_out_of_range_rows(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            corpus = directory / "labels.txt"
            corpus.write_text(
                "not-a-fen;5;e2e4\n"
                "4k3/8/8/8/8/8/P7/4K3 w - - 0 1;12;e2e4\n"
                "4k3/8/8/8/8/8/P7/4K3 w - - 0 1;99999;e2e4\n"
                "4k3/8/8/8/8/8/P7/4K3 w - - 0 1;missing;e2e4\n",
                encoding="utf-8",
            )
            output = directory / "dataset.bin"
            result = run_tool("encode", "--input", str(corpus), "--output", str(output))
            self.assertEqual(result.returncode, 0, result.stderr)
            header, records, _ = read_records(output.read_bytes())
            self.assertEqual(header["records"], 1)
            self.assertEqual(records[0][1], 12)

    def test_resume_checkpoint_finishes_with_identical_bytes(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            corpus = directory / "labels.txt"
            write_corpus(corpus, ROWS)
            reference = directory / "reference.bin"
            self.assertEqual(
                run_tool("encode", "--input", str(corpus), "--output", str(reference)).returncode, 0
            )

            chunked = directory / "chunked.bin"
            for _ in range(len(ROWS)):
                result = run_tool(
                    "encode", "--input", str(corpus), "--output", str(chunked),
                    "--resume", "--limit", "1",
                )
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertTrue(chunked.exists())
                self.assertTrue(Path(str(chunked) + ".state.json").exists())

            result = run_tool("encode", "--input", str(corpus), "--output", str(chunked), "--resume")
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertFalse(Path(str(chunked) + ".state.json").exists())
            self.assertEqual(chunked.read_bytes(), reference.read_bytes())

    def test_existing_output_requires_resume(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            corpus = directory / "labels.txt"
            write_corpus(corpus, ROWS)
            output = directory / "dataset.bin"
            self.assertEqual(
                run_tool("encode", "--input", str(corpus), "--output", str(output)).returncode, 0
            )
            result = run_tool("encode", "--input", str(corpus), "--output", str(output))
            self.assertEqual(result.returncode, 2)
            self.assertIn("already exists", result.stderr)
            result = run_tool("encode", "--input", str(corpus), "--output", str(output), "--resume")
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("already complete", result.stdout)

    def test_info_reports_the_header(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            corpus = directory / "labels.txt"
            write_corpus(corpus, ROWS)
            output = directory / "dataset.bin"
            self.assertEqual(
                run_tool("encode", "--input", str(corpus), "--output", str(output)).returncode, 0
            )
            result = run_tool("info", "--input", str(output))
            self.assertEqual(result.returncode, 0, result.stderr)
            info = json.loads(result.stdout)
            self.assertEqual(info["magic"], "KOI-DATA")
            self.assertEqual(info["version"], 1)
            self.assertEqual(info["feature_set"], "halfka-king-bucket-v1")
            self.assertEqual(info["records"], len(ROWS))
            self.assertTrue(info["complete"])


if __name__ == "__main__":
    unittest.main()
