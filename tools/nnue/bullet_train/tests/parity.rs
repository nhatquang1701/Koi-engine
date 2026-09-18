//! Feature-parity tests against the C++ `halfka-king-bucket-v1` golden index
//! lists pinned in `tests/unit/evaluation/evaluation_features_tests.cpp`.

use bullet_lib::game::inputs::SparseInputType;
use koi_bullet_train::{king_bucket, KoiHalfkaKingBucket, KOI_INPUTS};

fn features(fen: &str) -> Vec<usize> {
    let line = format!("{fen} | 0 | 0.5");
    let pos: bulletformat::ChessBoard = line.parse().expect("fixture parses");
    let mut indices = Vec::new();
    KoiHalfkaKingBucket.map_features(&pos, |index, _| indices.push(index));
    indices.sort_unstable();
    indices
}

fn startpos() -> Vec<usize> {
    vec![
        8, 9, 10, 11, 12, 13, 14, 15, 65, 70, 130, 133, 192, 199, 259, 324, 432, 433, 434, 435,
        436, 437, 438, 439, 505, 510, 570, 573, 632, 639, 699, 764,
    ]
}

#[test]
fn startpos_white_and_black_match_the_golden_list() {
    let white = features("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    let black = features("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR b KQkq - 0 1");
    assert_eq!(white, startpos());
    assert_eq!(black, startpos());
}

#[test]
fn midgame_matches_the_golden_list() {
    let expected = vec![
        8, 9, 10, 11, 13, 14, 15, 28, 70, 82, 130, 133, 192, 199, 259, 324, 420, 432, 433, 434,
        435, 437, 438, 439, 493, 505, 546, 570, 632, 639, 699, 764,
    ];
    let midgame = features("r1bqkbnr/pppp1ppp/2n5/4p3/2B1P3/5N2/PPPP1PPP/RNBQK2R b KQkq - 4 4");
    assert_eq!(midgame, expected);
}

#[test]
fn features_stay_inside_the_input_range_and_capacity() {
    let indices = features("r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1");
    assert!(!indices.is_empty());
    assert!(indices.iter().all(|&index| index < KOI_INPUTS));
    assert!(indices.len() <= 32);
}

#[test]
fn king_bucket_table_is_stable() {
    assert_eq!(king_bucket(4), 0); // e1
    assert_eq!(king_bucket(3), 3); // d1
    assert_eq!(king_bucket(28), 4); // e4 -> zone 1, mirrored file e -> 4
    assert_eq!(king_bucket(59), 11); // d8 -> zone 2, mirrored file d -> 7
}
