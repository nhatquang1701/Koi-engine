#pragma once

#include <string_view>

namespace koi {

struct ClassicalEvaluationParameters {
    std::string_view version = "classical-eval-v2";
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
    int bishop_pair_bonus = 30;
    int king_shield_bonus = 14;
    int king_open_file_penalty = 10;
    int king_zone_attack_penalty = 8;
    int king_safety_phase_offset = 8;
    int king_safety_phase_divisor = 32;
    int king_activity_weight = 1;
    int passed_pawn_king_support_bonus = 12;
    int passed_pawn_king_proximity_weight = 3;
    int passed_pawn_promotion_weight = 8;
    int tempo_bonus = 10;
};

inline constexpr ClassicalEvaluationParameters kClassicalEvaluationParameters{};

} // namespace koi
