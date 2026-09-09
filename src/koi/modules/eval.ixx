module;
#include <cstdint>

export module koi:eval;

export import :position;

export namespace koi::module_api {
inline constexpr unsigned evaluation_boundary_version = 1;

struct EvaluationContract {
    std::int32_t score_cp = 0;
    std::uint8_t game_phase = 0;
    bool insufficient_material = false;
    std::int16_t material_cp = 0;
    std::int16_t mobility_cp = 0;
    std::int16_t pawn_structure_cp = 0;
    std::int16_t king_safety_cp = 0;
    std::int16_t initiative_cp = 0;
};
}
