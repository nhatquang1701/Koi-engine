#pragma once

#include <string_view>

#include "koi/evaluation_parameters_generated.hpp"
#include "koi/piece_values.hpp"

namespace koi {

struct ClassicalEvaluationParameters {
    std::string_view version = "classical-eval-v8-endgame-scaling";
    int pawn_value = 100;
    int knight_value = 320;
    int bishop_value = 330;
    int rook_value = 500;
    int queen_value = 900;
    int maximum_phase = 24;
    int mobility_weight = 4;
    int doubled_pawn_penalty = 12;
    int isolated_pawn_penalty = 10;
    int pawn_island_penalty = 4;
    int passed_pawn_base = 20;
    int passed_pawn_advance_weight = 4;
    int passed_pawn_phase_base = 32;
    int passed_pawn_protection_bonus = 5;
    int direct_passed_pawn_blockade_penalty = 14;
    int connected_pawn_bonus = 6;
    int attacked_pawn_penalty = 6;
    int backward_pawn_penalty = 8;
    int bishop_mobility_weight = 2;
    int knight_outpost_bonus = 12;
    int rook_semi_open_file_bonus = 10;
    int rook_open_file_bonus = 20;
    int rook_seventh_rank_bonus = 15;
    int queen_mobility_weight = 1;
    int development_bonus = 10;
    int castling_readiness_bonus = 8;
    int early_queen_development_penalty = 14;
    int central_queen_development_penalty = 6;
    int center_control_weight = 7;
    int center_control_phase_offset = 8;
    int pawn_break_bonus = 12;
    int attacked_piece_pressure_weight = 2;
    int hanging_piece_penalty = 5;
    int trapped_piece_penalty = 12;
    int bishop_pair_bonus = 30;
    int king_shield_bonus = 14;
    int king_open_file_penalty = 10;
    int king_zone_attack_penalty = 8;
    int king_attacker_weight = 6;
    int king_safety_phase_offset = 8;
    int king_safety_phase_divisor = 32;
    int king_activity_weight = 1;
    // Endgame-phase depth at which king activity starts to fade in.  Below the
    // threshold (i.e. while the position still has enough material) the term is
    // exactly zero; above it the term ramps linearly to the full weight at a
    // bare-kings phase.  This replaces the old hard `phase <= 2` cliff so rook
    // and minor-piece endgames keep a king-centralization signal.
    int king_activity_endgame_threshold = 12;
    int passed_pawn_king_support_bonus = 12;
    int passed_pawn_king_proximity_weight = 3;
    // A passed pawn the enemy king still controls is worth less than one it
    // cannot catch; the penalty ramps in as the hostile king closes in.
    int passed_pawn_enemy_king_penalty_weight = 2;
    int passed_pawn_promotion_weight = 8;
    int tempo_bonus = 10;
    // Endgame drawishness scaling.  `total` is multiplied by the selected
    // factor and divided by 64, so 64 (the default) leaves the score untouched.
    // Scaling only applies while the phase is at or below the start phase, and
    // the patterns are deliberately narrow to avoid discounting won endings.
    int endgame_scale_start_phase = 8;
    // Exactly one minor piece each and no pawns: bishop vs knight, knight vs
    // knight, and opposite-colored-bishop pairs are almost always drawn.  Two
    // minors on one side (KBB/KBN) are excluded so those wins keep full value.
    int endgame_scale_minor_only = 32;
    // Exactly one bishop each on opposite square colors with no other non-pawn
    // pieces: the standard opposite-colored-bishop drawing factor.
    int endgame_scale_opposite_bishops = 32;
};

inline constexpr ClassicalEvaluationParameters kClassicalEvaluationParameters{};

// The evaluator's material fields are a tuning-facing view of the single
// material table in piece_values.hpp. Bind them at compile time so a change to
// one source cannot silently desynchronize evaluation from SEE, ordering, and
// book scoring.
static_assert(kClassicalEvaluationParameters.pawn_value == kPawnMaterialValue);
static_assert(kClassicalEvaluationParameters.knight_value == kKnightMaterialValue);
static_assert(kClassicalEvaluationParameters.bishop_value == kBishopMaterialValue);
static_assert(kClassicalEvaluationParameters.rook_value == kRookMaterialValue);
static_assert(kClassicalEvaluationParameters.queen_value == kQueenMaterialValue);

} // namespace koi
