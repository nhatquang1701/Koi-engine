import json
import random
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
TOOL = ROOT / "tools" / "measurement" / "koi_dataset.py"
sys.path.insert(0, str(TOOL.parent))
import koi_chess as chess  # noqa: E402
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

# The Italian midgame owns two attack relations: the knight on f3 attacks the
# e5 pawn and the bishop on c4 attacks the f7 pawn.  Both land in bucket 0,
# with the knight on the victim-type/victim-square layout (384 + 0 * 64 + 36)
# and the bishop on the slider layout (768 + 0 * 384 + 26 * 6 + 0).
MIDGAME_WHITE_THREATS = [9636, 10140]
# The symmetric black view mirrors the same two relations.
MIDGAME_BLACK_THREATS = [9628, 10188]

# The same midgame with white to move after 2.Nc3 Nf6 and 3.Bc5: both sides
# own attacks, so the symmetric threat group has four inputs.
MIDGAME_SYMMETRIC_THREATS = [9628, 9636, 10140, 10188]

# The merged v5 list is the halfka group followed by the four threat inputs.
MIDGAME_V5_INDICES = [
    8, 9, 10, 13, 14, 15, 19, 28, 82, 85, 130, 154, 192, 199, 259, 324,
    420, 432, 433, 434, 435, 437, 438, 439, 490, 493, 546, 570, 632, 639, 699, 764,
    9628, 9636, 10140, 10188,
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
        counts = struct.unpack_from("<4H", data, offset)
        offset += 8
        blocks = []
        for count in counts:
            blocks.append(list(struct.unpack_from(f"<{count}H", data, offset)))
            offset += 2 * count
        score = struct.unpack_from("<i", data, offset)[0]
        offset += 4
        records.append((blocks, score))
    return header, records, offset


class KoiDatasetTest(unittest.TestCase):
    def test_startpos_indices_match_pinned_golden_list(self):
        indices = koi_dataset.halfka_king_bucket_indices(chess.Board())
        self.assertEqual(indices, STARTPOS_INDICES)
        self.assertEqual(len(indices), len(set(indices)))
        self.assertTrue(all(0 <= index < koi_dataset.INPUT_UNITS for index in indices))
        for perspective in (chess.WHITE, chess.BLACK):
            self.assertEqual(koi_dataset.threat_pairs_indices(chess.Board(), perspective), [])

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

    def test_threat_golden_vectors(self):
        black_to_move = chess.Board(
            "r1bqkbnr/pppp1ppp/2n5/4p3/2B1P3/5N2/PPPP1PPP/RNBQK2R b KQkq - 4 4"
        )
        self.assertEqual(
            koi_dataset.threat_pairs_indices(black_to_move, chess.WHITE),
            MIDGAME_WHITE_THREATS,
        )
        self.assertEqual(
            koi_dataset.threat_pairs_indices(black_to_move, chess.BLACK),
            MIDGAME_BLACK_THREATS,
        )

        white_to_move = chess.Board(
            "r1bqk2r/pppp1ppp/2n2n2/2b1p3/2B1P3/2NP1N2/PPP2PPP/R1BQK2R w KQkq - 0 1"
        )
        self.assertEqual(
            koi_dataset.threat_pairs_indices(white_to_move, chess.WHITE),
            MIDGAME_SYMMETRIC_THREATS,
        )
        self.assertEqual(
            koi_dataset.threat_pairs_indices(white_to_move, chess.BLACK),
            MIDGAME_SYMMETRIC_THREATS,
        )

    def test_threat_dedupe_and_family_offsets(self):
        # Two white pawns attack the black d6 pawn: one deduplicated input; the
        # symmetric view also encodes the black pawn's attacks on both pawns.
        dedupe = chess.Board("4k3/8/3p4/2P1P3/8/8/8/4K3 w - - 0 1")
        self.assertEqual(
            koi_dataset.threat_pairs_indices(dedupe, chess.WHITE), [9250, 9252, 9259]
        )
        self.assertEqual(
            koi_dataset.threat_pairs_indices(dedupe, chess.BLACK), [9235, 9242, 9244]
        )

        families = chess.Board("1r2k3/8/2b5/8/3p4/5N2/8/1R2K3 w - - 0 1")
        self.assertEqual(
            koi_dataset.threat_pairs_indices(families, chess.WHITE), [9627, 10237, 10377, 10713]
        )
        self.assertEqual(
            koi_dataset.threat_pairs_indices(families, chess.BLACK), [9635, 10093, 10377, 10713]
        )

        king_attacks = chess.Board("1r2k3/8/2b5/8/3p4/8/3n4/1R2K3 w - - 0 1")
        self.assertEqual(
            koi_dataset.threat_pairs_indices(king_attacks, chess.WHITE), [9793, 10377, 10713, 11211]
        )
        self.assertEqual(
            koi_dataset.threat_pairs_indices(king_attacks, chess.BLACK), [9849, 10377, 10713, 11251]
        )

        queens = chess.Board("3qk3/8/8/8/8/8/8/3QK3 w - - 0 1")
        for perspective in (chess.WHITE, chess.BLACK):
            self.assertEqual(
                koi_dataset.threat_pairs_indices(queens, perspective), [10774, 11110]
            )

    def test_combined_v5_merges_both_groups(self):
        board = chess.Board(
            "r1bqk2r/pppp1ppp/2n2n2/2b1p3/2B1P3/2NP1N2/PPP2PPP/R1BQK2R w KQkq - 0 1"
        )
        self.assertEqual(koi_dataset.halfka_threat_v5_indices(board), MIDGAME_V5_INDICES)
        self.assertLessEqual(len(MIDGAME_V5_INDICES), koi_dataset.V5_SPARSE_CAPACITY)
        self.assertTrue(
            all(0 <= index < koi_dataset.TOTAL_INPUT_UNITS for index in MIDGAME_V5_INDICES)
        )

    def test_capacity_bound_holds_over_random_games(self):
        rng = random.Random(20260919)
        max_a = max_b = max_v5 = 0
        for _ in range(20):
            board = chess.Board()
            for _ in range(60):
                moves = list(board.legal_moves)
                if not moves:
                    break
                board.push(rng.choice(moves))
                for perspective in (chess.WHITE, chess.BLACK):
                    group_a = koi_dataset.halfka_king_bucket_indices_for(board, perspective)
                    group_b = koi_dataset.threat_pairs_indices(board, perspective)
                    merged = koi_dataset.halfka_threat_v5_indices(board, perspective)
                    self.assertEqual(merged, sorted(set(group_a) | set(group_b)))
                    self.assertEqual(len(merged), len(set(merged)))
                    max_a = max(max_a, len(group_a))
                    max_b = max(max_b, len(group_b))
                    max_v5 = max(max_v5, len(merged))
        self.assertLessEqual(max_a, koi_dataset.SPARSE_CAPACITY)
        self.assertLessEqual(max_b, koi_dataset.THREAT_SPARSE_CAPACITY)
        self.assertLessEqual(max_v5, koi_dataset.V5_SPARSE_CAPACITY)

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
            self.assertEqual(header["version"], 2)
            self.assertEqual(
                header["groups"], ["halfka-king-bucket-v1", "threat-pairs-v1"]
            )
            self.assertEqual(header["records"], len(ROWS))
            self.assertEqual(consumed, len(data))
            self.assertEqual(koi_dataset.HEADER_BYTES, koi_dataset.header_bytes())
            for (blocks, score), (fen, cp) in zip(records, ROWS):
                board = chess.Board(fen)
                self.assertEqual(blocks, list(koi_dataset.encode_record(board)))
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
            self.assertEqual(info["version"], 2)
            self.assertEqual(
                info["groups"], ["halfka-king-bucket-v1", "threat-pairs-v1"]
            )
            self.assertEqual(info["records"], len(ROWS))
            self.assertTrue(info["complete"])

    def test_v1_reader_retains_the_legacy_header(self):
        feature = b"halfka-king-bucket-v1"
        data = (
            koi_dataset.MAGIC
            + struct.pack("<I", 1)
            + struct.pack("<H", len(feature))
            + feature
            + struct.pack("<Q", 7)
        )
        header = koi_dataset.DatasetEncoder.read_header_bytes(data)
        self.assertEqual(header["version"], 1)
        self.assertEqual(header["feature_set"], "halfka-king-bucket-v1")
        self.assertEqual(header["records"], 7)


if __name__ == "__main__":
    unittest.main()
