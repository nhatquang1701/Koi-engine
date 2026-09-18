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
