//! KOI-specific bullet input/output definitions.
//!
//! These match the C++ `halfka-king-bucket-v1` feature set and the
//! `min(7, (32 - pieces) / 4)` piece-count output bucket exactly, so a network
//! trained here can be exported to the `KOI-NNUE` version 4 container.

use bulletformat::ChessBoard;
use bullet_lib::game::inputs::SparseInputType;
use bullet_lib::game::outputs::OutputBuckets;

/// Number of sparse input features (`12 king buckets * 12 planes * 64 squares`).
pub const KOI_INPUTS: usize = 9216;

/// Number of output buckets.
pub const KOI_OUTPUT_BUCKETS: usize = 8;

/// Maximum active features for a legal chess position (32 pieces).
pub const KOI_MAX_ACTIVE: usize = 32;

/// Features per king bucket: `12 planes * 64 squares`.
pub const KOI_FEATURES_PER_BUCKET: usize = 768;

/// Maps a king square (in side-to-move-normalized coordinates) to its bucket.
///
/// Mirrors `king_bucket()` in `src/koi/evaluation_features.cpp`: rank zones
/// `0..=2 -> 0`, `3..=5 -> 1`, `6..=7 -> 2`, and files are mirrored towards the
/// e-h half so that `a/e`, `b/f`, `c/g`, `d/h` share a bucket.
#[inline]
pub fn king_bucket(king_square: usize) -> usize {
    let file = king_square % 8;
    let rank = king_square / 8;
    let zone = if rank <= 2 {
        0
    } else if rank <= 5 {
        1
    } else {
        2
    };
    let mirrored_file = if file < 4 { file + 4 } else { file };
    zone * 4 + (mirrored_file - 4)
}

/// Piece-count output bucket: `min(7, (32 - pieces) / 4)`.
#[inline]
pub fn piece_count_bucket(pieces: usize) -> usize {
    ((32usize.saturating_sub(pieces)) / 4).min(KOI_OUTPUT_BUCKETS - 1)
}

/// The `halfka-king-bucket-v1` sparse input.
#[derive(Clone, Copy, Debug, Default)]
pub struct KoiHalfkaKingBucket;

impl SparseInputType for KoiHalfkaKingBucket {
    type RequiredDataType = ChessBoard;

    fn num_inputs(&self) -> usize {
        KOI_INPUTS
    }

    fn max_active(&self) -> usize {
        KOI_MAX_ACTIVE
    }

    fn map_features<F: FnMut(usize, usize)>(&self, pos: &ChessBoard, mut f: F) {
        let base = king_bucket(pos.our_ksq() as usize) * KOI_FEATURES_PER_BUCKET;
        for (piece, square) in *pos {
            let color = usize::from(piece & 8 != 0);
            let kind = usize::from(piece & 7);
            let index = base + (color * 6 + kind) * 64 + usize::from(square);
            f(index, index);
        }
    }

    fn shorthand(&self) -> String {
        "9216".to_string()
    }

    fn description(&self) -> String {
        "Koi halfka-king-bucket-v1 (12 king buckets, 12 planes, 64 squares)".to_string()
    }
}

/// Piece-count output buckets used by the v4 container.
#[derive(Clone, Copy, Debug, Default)]
pub struct KoiOutputBuckets;

impl OutputBuckets<ChessBoard> for KoiOutputBuckets {
    const BUCKETS: usize = KOI_OUTPUT_BUCKETS;

    fn bucket(&self, pos: &ChessBoard) -> u8 {
        piece_count_bucket(pos.occ().count_ones() as usize) as u8
    }
}

/// Threat-pairs bucket width: four attacker families over 576 offsets each.
pub const KOI_THREAT_FEATURES_PER_BUCKET: usize = 2304;
/// Total threat inputs: twelve king buckets times 2304 offsets.
pub const KOI_THREAT_INPUTS: usize = 12 * KOI_THREAT_FEATURES_PER_BUCKET;
/// Combined v5 inputs: group A (halfka-king-bucket-v1) plus group B (threat-pairs-v1).
pub const KOI_V5_INPUTS: usize = KOI_INPUTS + KOI_THREAT_INPUTS;
/// Sparse capacity of the combined v5 encoder (group A <= 32, group B <= 128).
pub const KOI_V5_MAX_ACTIVE: usize = 160;

const THREAT_PAWN_OFFSET: usize = 0;
const THREAT_KNIGHT_OFFSET: usize = 384;
const THREAT_SLIDER_OFFSET: usize = 768;
const THREAT_SLIDER_STRIDE: usize = 384;
const THREAT_KING_OFFSET: usize = 1920;

const KNIGHT_STEPS: [(i32, i32); 8] = [
    (1, 2),
    (2, 1),
    (2, -1),
    (1, -2),
    (-1, -2),
    (-2, -1),
    (-2, 1),
    (-1, 2),
];
const KING_STEPS: [(i32, i32); 8] = [
    (1, 0),
    (1, 1),
    (0, 1),
    (-1, 1),
    (-1, 0),
    (-1, -1),
    (0, -1),
    (1, -1),
];
const BISHOP_DIRS: [(i32, i32); 4] = [(1, 1), (1, -1), (-1, 1), (-1, -1)];
const ROOK_DIRS: [(i32, i32); 4] = [(1, 0), (-1, 0), (0, 1), (0, -1)];

fn step_attacks(square: usize, steps: &[(i32, i32)]) -> u64 {
    let file = (square % 8) as i32;
    let rank = (square / 8) as i32;
    let mut attacks = 0u64;
    for &(df, dr) in steps {
        let f = file + df;
        let r = rank + dr;
        if (0..8).contains(&f) && (0..8).contains(&r) {
            attacks |= 1u64 << ((r * 8 + f) as usize);
        }
    }
    attacks
}

fn ray_attacks(square: usize, occupancy: u64, dirs: &[(i32, i32)]) -> u64 {
    let file = (square % 8) as i32;
    let rank = (square / 8) as i32;
    let mut attacks = 0u64;
    for &(df, dr) in dirs {
        let mut f = file + df;
        let mut r = rank + dr;
        while (0..8).contains(&f) && (0..8).contains(&r) {
            let target = (r * 8 + f) as usize;
            attacks |= 1u64 << target;
            if occupancy & (1u64 << target) != 0 {
                break;
            }
            f += df;
            r += dr;
        }
    }
    attacks
}

fn pawn_attacks(square: usize, white: bool) -> u64 {
    let file = square % 8;
    let mut attacks = 0u64;
    if white {
        if file > 0 {
            attacks |= 1u64 << (square + 7);
        }
        if file < 7 {
            attacks |= 1u64 << (square + 9);
        }
    } else {
        if file < 7 && square >= 7 {
            attacks |= 1u64 << (square - 7);
        }
        if file > 0 && square >= 9 {
            attacks |= 1u64 << (square - 9);
        }
    }
    attacks
}

fn threat_offset(
    attacker_kind: usize,
    attacker_square: usize,
    victim_square: usize,
    victim_kind: usize,
) -> usize {
    match attacker_kind {
        0 => THREAT_PAWN_OFFSET + victim_kind * 64 + victim_square,
        1 => THREAT_KNIGHT_OFFSET + victim_kind * 64 + victim_square,
        2 => THREAT_SLIDER_OFFSET + attacker_square * 6 + victim_kind,
        3 => THREAT_SLIDER_OFFSET + THREAT_SLIDER_STRIDE + attacker_square * 6 + victim_kind,
        4 => {
            THREAT_SLIDER_OFFSET + 2 * THREAT_SLIDER_STRIDE + attacker_square * 6 + victim_kind
        }
        _ => THREAT_KING_OFFSET + victim_kind * 64 + victim_square,
    }
}

/// Combined v5 input: group A `halfka-king-bucket-v1` plus group B
/// `threat-pairs-v1`.  Every attack relation in the position is encoded for
/// both perspectives (the relation set is mirrored, not filtered), which keeps
/// the side-to-move and non-side-to-move feature counts identical.
#[derive(Clone, Copy, Debug, Default)]
pub struct KoiHalfkaThreat;

impl SparseInputType for KoiHalfkaThreat {
    type RequiredDataType = ChessBoard;

    fn num_inputs(&self) -> usize {
        KOI_V5_INPUTS
    }

    fn max_active(&self) -> usize {
        KOI_V5_MAX_ACTIVE
    }

    fn map_features<F: FnMut(usize, usize)>(&self, pos: &ChessBoard, mut f: F) {
        let our_king = pos.our_ksq() as usize;
        let their_king = pos.opp_ksq() as usize;
        let stm_bucket = king_bucket(our_king);
        let ntm_bucket = king_bucket(their_king);
        let stm_base = stm_bucket * KOI_FEATURES_PER_BUCKET;
        let ntm_base = ntm_bucket * KOI_FEATURES_PER_BUCKET;
        let stm_threat_base = KOI_INPUTS + stm_bucket * KOI_THREAT_FEATURES_PER_BUCKET;
        let ntm_threat_base = KOI_INPUTS + ntm_bucket * KOI_THREAT_FEATURES_PER_BUCKET;

        let mut own_occupancy = 0u64;
        let mut enemy_occupancy = 0u64;
        let mut kinds = [0u8; 64];
        let mut pieces: Vec<(u8, usize)> = Vec::with_capacity(32);
        for (piece, square) in *pos {
            let square = square as usize;
            if piece & 8 == 0 {
                own_occupancy |= 1u64 << square;
            } else {
                enemy_occupancy |= 1u64 << square;
            }
            kinds[square] = piece & 7;
            pieces.push((piece, square));
        }
        let occupancy = pos.occ();

        // Group A: one pair per piece, colour planes swapped in the ntm view.
        for &(piece, square) in &pieces {
            let color = usize::from(piece & 8 != 0);
            let kind = usize::from(piece & 7);
            let stm = stm_base + (color * 6 + kind) * 64 + square;
            let ntm = ntm_base + ((1 - color) * 6 + kind) * 64 + (square ^ 56);
            f(stm, ntm);
        }

        // Group B: one pair per attack relation, squares mirrored in the ntm view.
        let mut pairs: Vec<(usize, usize)> = Vec::new();
        for &(piece, square) in &pieces {
            let kind = usize::from(piece & 7);
            let white = piece & 8 == 0;
            let attacks = match kind {
                0 => pawn_attacks(square, white),
                1 => step_attacks(square, &KNIGHT_STEPS),
                2 => ray_attacks(square, occupancy, &BISHOP_DIRS),
                3 => ray_attacks(square, occupancy, &ROOK_DIRS),
                4 => ray_attacks(square, occupancy, &BISHOP_DIRS)
                    | ray_attacks(square, occupancy, &ROOK_DIRS),
                _ => step_attacks(square, &KING_STEPS),
            };
            let targets = if white { enemy_occupancy } else { own_occupancy };
            let mut victims = attacks & targets;
            while victims != 0 {
                let victim_square = victims.trailing_zeros() as usize;
                victims &= victims - 1;
                let victim_kind = usize::from(kinds[victim_square]);
                let stm = stm_threat_base
                    + threat_offset(kind, square, victim_square, victim_kind);
                let ntm = ntm_threat_base
                    + threat_offset(kind, square ^ 56, victim_square ^ 56, victim_kind);
                pairs.push((stm, ntm));
            }
        }
        pairs.sort_unstable();
        pairs.dedup();
        for (stm, ntm) in pairs {
            f(stm, ntm);
        }
    }

    fn shorthand(&self) -> String {
        "36864".to_string()
    }

    fn description(&self) -> String {
        "Koi halfka-king-bucket-v1 + threat-pairs-v1 (36864 inputs)".to_string()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn king_bucket_matches_the_cpp_table() {
        assert_eq!(king_bucket(0), 0); // a1
        assert_eq!(king_bucket(7), 3); // h1
        assert_eq!(king_bucket(24), 4); // a4
        assert_eq!(king_bucket(52), 8); // e7
        assert_eq!(king_bucket(63), 11); // h8
    }

    #[test]
    fn output_buckets_follow_the_piece_count() {
        assert_eq!(piece_count_bucket(32), 0);
        assert_eq!(piece_count_bucket(16), 4);
        assert_eq!(piece_count_bucket(8), 6);
        assert_eq!(piece_count_bucket(4), 7);
        assert_eq!(piece_count_bucket(2), 7);
    }
}
