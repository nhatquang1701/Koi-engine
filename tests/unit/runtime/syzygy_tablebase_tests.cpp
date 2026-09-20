#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

#include "koi/game_state.hpp"
#include "koi/syzygy_tablebase.hpp"

#include "koi_test_support.hpp"

extern "C" unsigned TB_LARGEST;

namespace {

using koi::test::require;

koi::GameState state_from_fen(std::string_view fen) {
    return koi::test::require_value(koi::GameState::from_fen(fen),
                                    "test FEN must construct a game state");
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
    const koi::test::TempDirectory root;
    const std::filesystem::path empty = root.file("empty");
    const std::filesystem::path valid = root.file("valid");
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
}

void test_wrong_sized_fixture_disables_probing_safely() {
    const koi::test::TempDirectory root;
    {
        std::ofstream tablebase(root.file("KQvK.rtbw"), std::ios::binary);
        tablebase.write(std::string(79, '\0').data(), 79);
    }

    const koi::GameState state = state_from_fen(
        "4k3/8/8/8/8/8/8/3QK3 w - - 0 1");
    const koi::SyzygyTablebase tablebase(root.path(), 5, 1, true);
    require(!tablebase.enabled(), "a wrong-sized Syzygy file must disable probing");
    require(!tablebase.probe_wdl(state.tablebase_snapshot()).has_value(),
            "a wrong-sized Syzygy file must fall back from WDL probing");
    require(!tablebase.probe_root(state).has_value(),
            "a wrong-sized Syzygy file must fall back from root probing");
}

void test_tablebase_instances_share_fathom_lifetime_in_both_clear_orders() {
    const koi::test::TempDirectory root;
    {
        std::ofstream tablebase(root.file("KQvK.rtbw"), std::ios::binary);
        tablebase.write(std::string(80, '\0').data(), 80);
    }

    {
        auto first = std::make_unique<koi::SyzygyTablebase>(root.path(), 5, 1, true);
        auto second = std::make_unique<koi::SyzygyTablebase>(root.path(), 5, 1, true);
        require(first->enabled() && second->enabled(),
                "two users of one valid tablebase path must both be enabled");
        second.reset();
        require(first->enabled() && TB_LARGEST > 0,
                "clearing the second user must not invalidate the first user");
        first.reset();
        require(TB_LARGEST == 0, "clearing the final tablebase user must free Fathom");
    }

    {
        auto first = std::make_unique<koi::SyzygyTablebase>(root.path(), 5, 1, true);
        auto second = std::make_unique<koi::SyzygyTablebase>(root.path(), 5, 1, true);
        require(first->enabled() && second->enabled(),
                "Fathom must be reacquirable after the final user is cleared");
        first.reset();
        require(second->enabled() && TB_LARGEST > 0,
                "clearing the first user must not invalidate the second user");
        second.reset();
        require(TB_LARGEST == 0, "the second clear order must release Fathom at the end");
    }
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
    require(koi::syzygy_wdl_from_rank(900) == koi::SyzygyWdl::win &&
                koi::syzygy_wdl_from_rank(999) == koi::SyzygyWdl::win,
            "ranks 900..999 must classify as wins (Fathom's 50-move-aware boundary)");
    require(koi::syzygy_wdl_from_rank(899) == koi::SyzygyWdl::cursed_win,
            "rank 899 must remain a cursed win");
    require(koi::syzygy_wdl_from_rank(-899) == koi::SyzygyWdl::blessed_loss,
            "rank -899 must remain a blessed loss");
    require(koi::syzygy_wdl_from_rank(-900) == koi::SyzygyWdl::loss &&
                koi::syzygy_wdl_from_rank(-1000) == koi::SyzygyWdl::loss,
            "ranks -1000..-900 must classify as losses");

    const koi::SyzygyScore root_win = koi::syzygy_root_score(koi::SyzygyWdl::win);
    const koi::SyzygyScore root_loss = koi::syzygy_root_score(koi::SyzygyWdl::loss);
    require(root_win.score_cp == 90'000 && !root_win.mate.has_value(),
            "a root tablebase win must report a decisive score without a mate distance");
    require(root_loss.score_cp == -90'000 && !root_loss.mate.has_value(),
            "a root tablebase loss must report a decisive score without a mate distance");
    require(koi::syzygy_root_score(koi::SyzygyWdl::cursed_win).score_cp == 1 &&
                !koi::syzygy_root_score(koi::SyzygyWdl::cursed_win).mate.has_value(),
            "root cursed wins must keep their near-zero non-mate score");

    require(koi::syzygy_wdl_permill(koi::SyzygyWdl::win) == std::array<int, 3>{1'000, 0, 0},
            "a tablebase win must report an exact winning WDL triplet");
    require(koi::syzygy_wdl_permill(koi::SyzygyWdl::loss) == std::array<int, 3>{0, 0, 1'000},
            "a tablebase loss must report an exact losing WDL triplet");
    require(koi::syzygy_wdl_permill(koi::SyzygyWdl::cursed_win) == std::array<int, 3>{0, 1'000, 0} &&
                koi::syzygy_wdl_permill(koi::SyzygyWdl::blessed_loss) ==
                    std::array<int, 3>{0, 1'000, 0},
            "cursed wins and blessed losses must report as draws under the 50-move rule");

    const koi::SyzygyTablebase depth_gate({}, 7, 1, true);
    const koi::SyzygyTablebase deep_gate({}, 7, 3, true);
    require(depth_gate.allows_depth(1) && depth_gate.allows_depth(10),
            "the default probe depth must allow probing at any search depth");
    require(!deep_gate.allows_depth(2) && deep_gate.allows_depth(3) && deep_gate.allows_depth(9),
            "a raised probe depth must only allow probing at or above that depth");

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

void write_dummy_kqvk(const std::filesystem::path& directory) {
    std::ofstream tablebase(directory / "KQvK.rtbw", std::ios::binary);
    tablebase.write(std::string(80, '\0').data(), 80);
}

void test_large_table_limit_reflects_the_loaded_directory() {
    const koi::SyzygyTablebase disabled("definitely-missing-syzygy-path", 5, 1, true);
    require(disabled.large_table_limit() == 0,
            "a disabled tablebase must report no registered tables");

    const koi::test::TempDirectory root;
    write_dummy_kqvk(root.path());
    const koi::SyzygyTablebase enabled(root.path(), 5, 1, true);
    require(enabled.enabled(), "the dummy KQvK fixture must enable probing");
    require(TB_LARGEST > 0, "Fathom must register the dummy KQvK table");
    require(enabled.large_table_limit() == static_cast<int>(TB_LARGEST),
            "the reported large-table limit must match Fathom's registered maximum");
}

void test_probe_eligibility_is_a_cheap_conservative_gate() {
    const koi::SyzygyTablebase disabled("definitely-missing-syzygy-path", 5, 1, true);
    require(!disabled.probe_eligible(state_from_fen("4k3/8/8/8/8/8/8/4K3 w - - 0 1")),
            "a disabled tablebase must never claim eligibility");

    const koi::test::TempDirectory root;
    write_dummy_kqvk(root.path());
    const koi::SyzygyTablebase tablebase(root.path(), 5, 1, true);
    require(tablebase.enabled(), "the dummy KQvK fixture must enable probing");

    require(tablebase.probe_eligible(state_from_fen("4k3/8/8/8/8/8/8/3QK3 w - - 0 1")),
            "a three-piece position inside the registered limit must be eligible");
    require(!tablebase.probe_eligible(state_from_fen("4k3/8/8/8/8/8/8/R2QK3 w - - 0 1")),
            "positions above the registered large-table limit must not be eligible");
    require(!tablebase.probe_eligible(state_from_fen("4k3/8/8/8/8/8/8/R3K3 w Q - 0 1")),
            "castling rights must gate eligibility even within the piece limit");
    require(tablebase.probe_eligible(state_from_fen("4k3/8/8/8/8/8/8/R3K3 w - - 0 1")),
            "the same three-piece position without castling rights must be eligible");
    require(!tablebase.probe_eligible(koi::GameState::startpos()),
            "the start position must never be eligible for a reduced-piece install");
}

void test_concurrent_enabled_wdl_probes_agree() {
    const koi::test::TempDirectory root;
    write_dummy_kqvk(root.path());
    const koi::SyzygyTablebase tablebase(root.path(), 5, 1, true);
    require(tablebase.enabled(), "the dummy KQvK fixture must enable probing");

    const koi::TablebaseSnapshot snapshot = state_from_fen(
        "4k3/8/8/8/8/8/8/3QK3 w - - 0 1").tablebase_snapshot();
    std::vector<std::optional<koi::SyzygyWdl>> results(8);
    std::vector<std::thread> workers;
    for (int index = 0; index < 8; ++index) {
        workers.emplace_back([&tablebase, &snapshot, &results, index] {
            results[static_cast<std::size_t>(index)] = tablebase.probe_wdl(snapshot);
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }
    for (const std::optional<koi::SyzygyWdl>& result : results) {
        require(result == results.front(),
                "concurrent WDL probes through one enabled tablebase must agree");
    }
}

} // namespace

int main(int argc, char** argv) {
    const std::vector<koi::test::TestCase> tests{
        {"snapshot rule metadata and piece bitboards",
         test_snapshot_converts_rule_metadata_and_piece_bitboards},
        {"snapshot native rule state after pinned double push",
         test_snapshot_uses_native_rule_state_after_a_pinned_double_push},
        {"absent and malformed paths disable probing safely",
         test_absent_and_malformed_paths_disable_probing_safely},
        {"empty and valid paths preserve optional fallback",
         test_empty_and_valid_paths_preserve_optional_fallback},
        {"wrong-sized fixture disables probing safely",
         test_wrong_sized_fixture_disables_probing_safely},
        {"tablebase instances share Fathom lifetime in both clear orders",
         test_tablebase_instances_share_fathom_lifetime_in_both_clear_orders},
        {"piece count and castling gate probing", test_piece_count_and_castling_gate_probing},
        {"malformed snapshots are rejected before probing",
         test_malformed_snapshots_are_rejected_before_probing},
        {"WDL conversion and concurrent disabled probes",
         test_wdl_conversion_and_concurrent_disabled_probes},
        {"50-move rule selects clock-aware root probe",
         test_50_move_rule_selects_clock_aware_root_probe},
        {"seven-piece probe limit is preserved", test_seven_piece_probe_limit_is_preserved},
        {"large table limit reflects the loaded directory",
         test_large_table_limit_reflects_the_loaded_directory},
        {"probe eligibility is a cheap conservative gate",
         test_probe_eligibility_is_a_cheap_conservative_gate},
        {"concurrent enabled WDL probes agree", test_concurrent_enabled_wdl_probes_agree},
    };
    return koi::test::run_tests(tests, argc, argv);
}
