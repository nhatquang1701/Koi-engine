#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

#include "koi/game_state.hpp"
#include "koi/syzygy_tablebase.hpp"

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

void test_wdl_conversion_and_concurrent_disabled_probes() {
    require(koi::syzygy_score(koi::SyzygyWdl::win).score_cp > 0,
            "winning WDL must convert to a positive score");
    require(koi::syzygy_score(koi::SyzygyWdl::draw).score_cp == 0,
            "draw WDL must convert to a zero score");
    require(koi::syzygy_score(koi::SyzygyWdl::loss).score_cp < 0,
            "losing WDL must convert to a negative score");

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

} // namespace

int main() {
    try {
        test_snapshot_converts_rule_metadata_and_piece_bitboards();
        test_absent_and_malformed_paths_disable_probing_safely();
        test_piece_count_and_castling_gate_probing();
        test_wdl_conversion_and_concurrent_disabled_probes();
        std::cout << "PASS syzygy tablebase tests\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL syzygy tablebase tests: " << error.what() << '\n';
        return 1;
    }
}
