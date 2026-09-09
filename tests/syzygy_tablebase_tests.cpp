#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

#include "koi/game_state.hpp"
#include "koi/syzygy_tablebase.hpp"

extern "C" unsigned TB_LARGEST;

namespace {

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

koi::GameState state_from_fen(std::string_view fen) {
    const auto state = koi::GameState::from_fen(fen);
    require(state.has_value(), std::string("test FEN must be accepted: ") + std::string(fen));
    return *state;
}

void test_snapshot_converts_rule_metadata_and_piece_bitboards() {
    const koi::GameState state = state_from_fen(
        "r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 17 9");
    const koi::TablebaseSnapshot snapshot = state.tablebase_snapshot();

    require(snapshot.side_to_move == koi::Color::white, "snapshot must retain side to move");
    require(snapshot.castling_rights == koi::kAllCastlingRights,
            "snapshot must retain all castling-right flags");
    require(snapshot.halfmove_clock == 17, "snapshot must retain the halfmove clock");
    require((snapshot.piece_bitboards[0][static_cast<std::size_t>(koi::PieceType::king) - 1] &
             (std::uint64_t{1} << koi::Square::parse("e1")->index())) != 0,
            "snapshot must convert the white king bitboard");
    require((snapshot.piece_bitboards[1][static_cast<std::size_t>(koi::PieceType::rook) - 1] &
             (std::uint64_t{1} << koi::Square::parse("h8")->index())) != 0,
            "snapshot must convert the black rook bitboard");

    const koi::TablebaseSnapshot en_passant_snapshot = state_from_fen(
        "4k3/8/8/3pP3/8/8/8/4K3 w - d6 0 9").tablebase_snapshot();
    require(en_passant_snapshot.en_passant_square == *koi::Square::parse("d6"),
            "snapshot must retain the en-passant square");
}

void test_snapshot_uses_native_rule_state_after_a_pinned_double_push() {
    koi::GameState state = state_from_fen("7k/5p2/8/r5PK/8/8/8/8 b - - 0 1");
    const auto double_push = koi::Move::parse_uci("f7f5");
    require(double_push.has_value() && state.make_move(*double_push),
            "the pinned en-passant fixture must apply the pawn double push");
    require(state.en_passant_square().index() == koi::Square::kInvalid,
            "the native position must discard an unusable en-passant target");

    const koi::TablebaseSnapshot snapshot = state.tablebase_snapshot();
    require(snapshot.en_passant_square == state.en_passant_square() &&
                snapshot.castling_rights == state.castling_rights() &&
                snapshot.halfmove_clock == state.halfmove_clock(),
            "the tablebase snapshot must be sourced from native rule state rather than the shadow board");
}

void test_absent_and_malformed_paths_disable_probing_safely() {
    const koi::TablebaseSnapshot snapshot = state_from_fen(
        "4k3/8/8/8/8/8/8/4K3 w - - 0 1").tablebase_snapshot();
    const koi::SyzygyTablebase absent("definitely-missing-syzygy-path", 5, 1, true);
    const koi::SyzygyTablebase malformed(std::filesystem::path{"?not-a-tablebase-path?"}, 5, 1, true);

    require(!absent.enabled() && !malformed.enabled(),
            "absent and malformed paths must disable probing");
    require(!absent.probe_wdl(snapshot).has_value(), "disabled WDL probing must be empty");
    require(!malformed.probe_root(state_from_fen(
        "4k3/8/8/8/8/8/8/4K3 w - - 0 1")).has_value(),
            "disabled root probing must be empty");
}

void test_empty_and_valid_paths_preserve_optional_fallback() {
    const std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("koi-task-2-syzygy-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    const std::filesystem::path empty = root / "empty";
    const std::filesystem::path valid = root / "valid";
    std::filesystem::create_directories(empty);
    std::filesystem::create_directories(valid);
    {
        std::ofstream tablebase(valid / "KQvK.rtbw", std::ios::binary);
        tablebase.write(std::string(80, '\0').data(), 80);
    }

    const koi::GameState state = state_from_fen(
        "4k3/8/8/8/8/8/8/3QK3 w - - 0 1");
    const koi::SyzygyTablebase empty_tablebase(empty, 5, 1, true);
    require(!empty_tablebase.enabled(), "an empty readable directory must fall back to search");
    require(!empty_tablebase.probe_wdl(state.tablebase_snapshot()).has_value(),
            "an empty readable directory must not probe as a tablebase");

    const koi::SyzygyTablebase valid_tablebase(valid, 5, 1, true);
    require(valid_tablebase.enabled(), "a readable tablebase path must acquire Fathom ownership");
    require(TB_LARGEST > 0, "Fathom must report the registered valid-path table");

    std::filesystem::remove_all(root);
}

void test_wrong_sized_fixture_disables_probing_safely() {
    const std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("koi-task-2-syzygy-malformed-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    {
        std::ofstream tablebase(root / "KQvK.rtbw", std::ios::binary);
        tablebase.write(std::string(79, '\0').data(), 79);
    }

    const koi::GameState state = state_from_fen(
        "4k3/8/8/8/8/8/8/3QK3 w - - 0 1");
    const koi::SyzygyTablebase tablebase(root, 5, 1, true);
    require(!tablebase.enabled(), "a wrong-sized Syzygy file must disable probing");
    require(!tablebase.probe_wdl(state.tablebase_snapshot()).has_value(),
            "a wrong-sized Syzygy file must fall back from WDL probing");
    require(!tablebase.probe_root(state).has_value(),
            "a wrong-sized Syzygy file must fall back from root probing");

    std::filesystem::remove_all(root);
}

void test_tablebase_instances_share_fathom_lifetime_in_both_clear_orders() {
    const std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("koi-task-2-syzygy-lifetime-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    {
        std::ofstream tablebase(root / "KQvK.rtbw", std::ios::binary);
        tablebase.write(std::string(80, '\0').data(), 80);
    }

    {
        auto first = std::make_unique<koi::SyzygyTablebase>(root, 5, 1, true);
        auto second = std::make_unique<koi::SyzygyTablebase>(root, 5, 1, true);
        require(first->enabled() && second->enabled(),
                "two users of one valid tablebase path must both be enabled");
        second.reset();
        require(first->enabled() && TB_LARGEST > 0,
                "clearing the second user must not invalidate the first user");
        first.reset();
        require(TB_LARGEST == 0, "clearing the final tablebase user must free Fathom");
    }

    {
        auto first = std::make_unique<koi::SyzygyTablebase>(root, 5, 1, true);
        auto second = std::make_unique<koi::SyzygyTablebase>(root, 5, 1, true);
        require(first->enabled() && second->enabled(),
                "Fathom must be reacquirable after the final user is cleared");
        first.reset();
        require(second->enabled() && TB_LARGEST > 0,
                "clearing the first user must not invalidate the second user");
        second.reset();
        require(TB_LARGEST == 0, "the second clear order must release Fathom at the end");
    }

    std::filesystem::remove_all(root);
}

void test_piece_count_and_castling_gate_probing() {
    const koi::SyzygyTablebase tablebase({}, 5, 1, true);
    require(tablebase.supports(state_from_fen(
        "4k3/8/8/8/8/8/8/4K3 w - - 0 1").tablebase_snapshot()),
            "two kings must be within the configured piece limit");
    require(!tablebase.supports(state_from_fen(
        "r3k2r/8/8/8/8/3p4/4P3/R3K2R w KQkq - 0 1").tablebase_snapshot()),
            "positions with unsupported castling must be gated");
    require(!tablebase.supports(state_from_fen(
        "r3k2r/8/8/8/8/3p4/4P3/R3K2R w - - 0 1").tablebase_snapshot()),
            "positions above five pieces must be gated");
}

void test_malformed_snapshots_are_rejected_before_probing() {
    const koi::SyzygyTablebase tablebase({}, 7, 1, true);
    koi::TablebaseSnapshot missing_black_king;
    missing_black_king.piece_bitboards[0][static_cast<std::size_t>(koi::PieceType::king) - 1] =
        std::uint64_t{1} << koi::Square::parse("e1")->index();
    require(!tablebase.supports(missing_black_king),
            "a malformed snapshot without both kings must never be passed to Fathom");

    koi::TablebaseSnapshot overlapping = state_from_fen(
        "4k3/8/8/8/8/8/8/4K3 w - - 0 1").tablebase_snapshot();
    overlapping.piece_bitboards[1][static_cast<std::size_t>(koi::PieceType::queen) - 1] |=
        std::uint64_t{1} << koi::Square::parse("e1")->index();
    require(!tablebase.supports(overlapping),
            "a snapshot with overlapping pieces must never be passed to Fathom");

    koi::TablebaseSnapshot impossible_en_passant = state_from_fen(
        "4k3/8/8/8/8/8/8/4K3 w - - 0 1").tablebase_snapshot();
    impossible_en_passant.en_passant_square = *koi::Square::parse("e4");
    require(!tablebase.supports(impossible_en_passant),
            "a snapshot with an impossible en-passant square must never be passed to Fathom");
}

void test_wdl_conversion_and_concurrent_disabled_probes() {
    require(koi::syzygy_score(koi::SyzygyWdl::win).score_cp > 0,
            "winning WDL must convert to a positive score");
    require(koi::syzygy_score(koi::SyzygyWdl::draw).score_cp == 0,
            "draw WDL must convert to a zero score");
    require(koi::syzygy_score(koi::SyzygyWdl::loss).score_cp < 0,
            "losing WDL must convert to a negative score");
    require(koi::syzygy_score(koi::SyzygyWdl::cursed_win).score_cp > 0 &&
                !koi::syzygy_score(koi::SyzygyWdl::cursed_win).mate.has_value(),
            "cursed wins must be positive non-mate scores");
    require(koi::syzygy_score(koi::SyzygyWdl::blessed_loss).score_cp < 0 &&
                !koi::syzygy_score(koi::SyzygyWdl::blessed_loss).mate.has_value(),
            "blessed losses must be negative non-mate scores");

    require(koi::syzygy_wdl_from_rank(1000) == koi::SyzygyWdl::win,
            "rank 1000 must remain a true win");
    require(koi::syzygy_wdl_from_rank(899) == koi::SyzygyWdl::cursed_win,
            "rank 899 must remain a cursed win");
    require(koi::syzygy_wdl_from_rank(-899) == koi::SyzygyWdl::blessed_loss,
            "rank -899 must remain a blessed loss");
    require(koi::syzygy_wdl_from_rank(-1000) == koi::SyzygyWdl::loss,
            "rank -1000 must remain a true loss");

    const koi::SyzygyTablebase tablebase("missing-syzygy-path", 5, 1, true);
    const koi::TablebaseSnapshot snapshot = state_from_fen(
        "4k3/8/8/8/8/8/8/4K3 w - - 0 1").tablebase_snapshot();
    std::atomic<bool> failed = false;
    std::vector<std::thread> workers;
    for (int index = 0; index < 8; ++index) {
        workers.emplace_back([&] {
            if (tablebase.probe_wdl(snapshot).has_value()) {
                failed.store(true, std::memory_order_relaxed);
            }
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }
    require(!failed.load(std::memory_order_relaxed),
            "concurrent WDL probes must be safe when probing is disabled");
}

void test_50_move_rule_selects_clock_aware_root_probe() {
    const koi::SyzygyTablebase with_rule({}, 5, 1, true);
    const koi::SyzygyTablebase without_rule({}, 5, 1, false);
    require(with_rule.uses_clock_aware_root_probe(),
            "the 50-move rule must select Fathom's DTZ root path");
    require(!without_rule.uses_clock_aware_root_probe(),
            "disabling the 50-move rule must select the WDL root path");
}

void test_seven_piece_probe_limit_is_preserved() {
    const koi::SyzygyTablebase tablebase({}, 7, 1, true);
    require(tablebase.probe_limit() == 7,
            "the tablebase adapter must retain the configured seven-piece ceiling");
}

} // namespace

int main() {
    try {
        test_snapshot_converts_rule_metadata_and_piece_bitboards();
        test_snapshot_uses_native_rule_state_after_a_pinned_double_push();
        test_absent_and_malformed_paths_disable_probing_safely();
        test_empty_and_valid_paths_preserve_optional_fallback();
        test_wrong_sized_fixture_disables_probing_safely();
        test_tablebase_instances_share_fathom_lifetime_in_both_clear_orders();
        test_piece_count_and_castling_gate_probing();
        test_malformed_snapshots_are_rejected_before_probing();
        test_wdl_conversion_and_concurrent_disabled_probes();
        test_50_move_rule_selects_clock_aware_root_probe();
        test_seven_piece_probe_limit_is_preserved();
        std::cout << "PASS syzygy tablebase tests\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL syzygy tablebase tests: " << error.what() << '\n';
        return 1;
    }
}
