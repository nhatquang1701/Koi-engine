#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "koi/classical_evaluator.hpp"
#include "koi/search_service.hpp"
#include "koi/time_manager.hpp"
#include "koi/transposition_table.hpp"

namespace {

using namespace std::chrono_literals;

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

koi::GameState require_state(std::string_view fen) {
    const auto state = koi::GameState::from_fen(fen);
    require(state.has_value(), "test FEN must construct a game state");
    return *state;
}

koi::GameState require_evaluation_fixture(std::string_view name) {
    const std::filesystem::path path = std::filesystem::path(__FILE__).parent_path() /
        "data" / "evaluation-positions.txt";
    std::ifstream input(path);
    require(input.good(), "evaluation fixture data must be readable");

    std::string line;
    while (std::getline(input, line)) {
        if (line.empty() || line.front() == '#') {
            continue;
        }
        const std::size_t separator = line.find('|');
        require(separator != std::string::npos, "evaluation fixture must contain a name and FEN");
        if (std::string_view(line).substr(0, separator) == name) {
            return require_state(std::string_view(line).substr(separator + 1));
        }
    }
    throw std::runtime_error("missing evaluation fixture: " + std::string(name));
}

struct CompletedSearch {
    mutable std::mutex mutex;
    std::optional<koi::SearchResult> result;
    std::uint32_t completions = 0;

    koi::SearchEventSink sink() {
        return {.on_complete = [this](const koi::SearchResult& completed) {
                    std::lock_guard lock(mutex);
                    result = completed;
                    ++completions;
                }};
    }

    koi::SearchResult take_result() {
        std::lock_guard lock(mutex);
        require(result.has_value(), "search must report a result");
        require(completions == 1, "search must report exactly one final result");
        return *result;
    }

    std::uint32_t completion_count() const {
        std::lock_guard lock(mutex);
        return completions;
    }
};

class ConcurrencyEvaluator final : public koi::Evaluator {
public:
    explicit ConcurrencyEvaluator(bool supports_concurrency = true)
        : supports_concurrency_(supports_concurrency) {}

    [[nodiscard]] bool supports_concurrent_evaluation() const noexcept override {
        return supports_concurrency_;
    }

    [[nodiscard]] int evaluate(const koi::GameState&, koi::Color) const override {
        const int current = active_.fetch_add(1, std::memory_order_relaxed) + 1;
        int observed = maximum_active_.load(std::memory_order_relaxed);
        while (current > observed &&
               !maximum_active_.compare_exchange_weak(observed, current, std::memory_order_relaxed)) {
        }
        std::this_thread::sleep_for(2ms);
        active_.fetch_sub(1, std::memory_order_relaxed);
        return 0;
    }

    [[nodiscard]] int maximum_active() const noexcept {
        return maximum_active_.load(std::memory_order_relaxed);
    }

private:
    bool supports_concurrency_;
    mutable std::atomic_int active_ = 0;
    mutable std::atomic_int maximum_active_ = 0;
};

class CheckedPositionEvaluator final : public koi::Evaluator {
public:
    [[nodiscard]] int evaluate(const koi::GameState& state, koi::Color) const override {
        if (state.in_check()) {
            checked_evaluations_.fetch_add(1, std::memory_order_relaxed);
        }
        return 0;
    }

    [[nodiscard]] bool supports_concurrent_evaluation() const noexcept override { return true; }

    [[nodiscard]] int checked_evaluations() const noexcept {
        return checked_evaluations_.load(std::memory_order_relaxed);
    }

private:
    mutable std::atomic_int checked_evaluations_ = 0;
};

koi::SearchResult search(koi::SearchService& service, koi::GameState root, koi::SearchLimits limits,
                         koi::SearchOptions options = {}) {
    CompletedSearch completed;
    koi::SearchHandle handle = service.start(std::move(root), limits, completed.sink(), options);
    handle.wait();
    return completed.take_result();
}

void test_evaluator_returns_material_and_pst_from_requested_perspective() {
    koi::ClassicalEvaluator evaluator;
    const koi::GameState queen_up = require_state("4k3/8/8/8/8/8/8/3QK3 w - - 0 1");
    const koi::GameState pawn_home = require_state("4k3/8/8/8/8/8/P7/4K3 w - - 0 1");
    const koi::GameState pawn_advanced = require_state("4k3/8/8/8/4P3/8/8/4K3 w - - 0 1");

    const int queen_score = evaluator.evaluate(queen_up, koi::Color::white);
    require(queen_score >= 850, "a white queen advantage must evaluate as strongly positive");
    require(evaluator.evaluate(queen_up, koi::Color::black) == -queen_score,
            "black perspective must negate the white perspective score");
    require(evaluator.evaluate(pawn_advanced, koi::Color::white) > evaluator.evaluate(pawn_home, koi::Color::white),
            "a centrally advanced white pawn must receive a positive PST improvement");
}

void test_evaluator_breakdown_scores_structure_activity_and_king_safety() {
    koi::ClassicalEvaluator evaluator;
    const koi::GameState mobile_knight = require_state("4k3/8/8/3N4/8/8/8/4K3 w - - 0 1");
    const koi::GameState corner_knight = require_state("4k3/8/8/8/8/8/8/N3K3 w - - 0 1");
    const koi::GameState passed_pawns = require_state("4k3/8/8/3P4/4P3/8/8/4K3 w - - 0 1");
    const koi::GameState doubled_pawns = require_state("4k3/8/8/3P4/3P4/8/8/4K3 w - - 0 1");
    const koi::GameState shielded_king = require_state("4k3/8/8/8/8/8/5PPP/4K3 w - - 0 1");
    const koi::GameState exposed_king = require_state("4k3/8/8/8/8/8/8/4K3 w - - 0 1");

    const auto mobile = evaluator.breakdown(mobile_knight, koi::Color::white);
    const auto corner = evaluator.breakdown(corner_knight, koi::Color::white);
    const auto passed = evaluator.breakdown(passed_pawns, koi::Color::white);
    const auto doubled = evaluator.breakdown(doubled_pawns, koi::Color::white);
    const auto shielded = evaluator.breakdown(shielded_king, koi::Color::white);
    const auto exposed = evaluator.breakdown(exposed_king, koi::Color::white);

    require(mobile.mobility > corner.mobility, "centralized pieces must receive a mobility bonus");
    require(passed.pawn_structure > doubled.pawn_structure,
            "passed and connected pawns must score better than doubled pawns");
    require(shielded.king_safety > exposed.king_safety,
            "a king pawn shield must improve king-safety scoring");
    require(mobile.total == evaluator.evaluate(mobile_knight, koi::Color::white),
            "breakdown total must match the evaluator result");
}

void test_evaluator_breakdown_is_perspective_symmetric() {
    koi::ClassicalEvaluator evaluator;
    const koi::GameState state = require_state("r3k2r/pp2bppp/2p1pn2/8/2B5/2N1PN2/PPPQ1PPP/R3K2R w KQkq - 0 1");
    const auto white = evaluator.breakdown(state, koi::Color::white);
    const auto black = evaluator.breakdown(state, koi::Color::black);

    require(white.material == -black.material && white.piece_square == -black.piece_square &&
                white.mobility == -black.mobility && white.pawn_structure == -black.pawn_structure &&
                white.activity == -black.activity && white.king_safety == -black.king_safety &&
                white.total == -black.total,
            "every evaluation component must negate under a perspective flip");
}

void test_evaluator_scores_backward_pawns_piece_mobility_and_dead_material() {
    koi::ClassicalEvaluator evaluator;
    const koi::GameState backward = require_state("k7/1b6/8/8/3P4/4P3/8/K7 w - - 0 1");
    const koi::GameState healthy = require_state("k7/2b5/8/8/3P4/4P3/8/K7 w - - 0 1");
    const koi::GameState central_queen = require_state("4k3/8/8/8/3Q4/8/8/4K3 w - - 0 1");
    const koi::GameState corner_queen = require_state("4k3/8/8/8/8/8/8/4K2Q w - - 0 1");
    const koi::GameState central_bishop = require_state("4k2r/8/8/8/2B5/8/8/4K3 w - - 0 1");
    const koi::GameState corner_bishop = require_state("4k2r/8/8/8/8/8/8/B3K3 w - - 0 1");
    const koi::GameState lone_bishop = require_state("4k3/8/8/8/8/8/2B5/4K3 w - - 0 1");

    const auto backward_score = evaluator.breakdown(backward, koi::Color::white);
    const auto healthy_score = evaluator.breakdown(healthy, koi::Color::white);
    const auto central_score = evaluator.breakdown(central_queen, koi::Color::white);
    const auto corner_score = evaluator.breakdown(corner_queen, koi::Color::white);
    const auto central_bishop_score = evaluator.breakdown(central_bishop, koi::Color::white);
    const auto corner_bishop_score = evaluator.breakdown(corner_bishop, koi::Color::white);

    require(backward_score.pawn_structure < healthy_score.pawn_structure,
            "a backward pawn must score below an otherwise healthy pawn structure");
    require(central_score.activity > corner_score.activity,
            "central bishops and queens must receive piece-specific mobility credit");
    require(central_bishop_score.activity > corner_bishop_score.activity,
            "bishop mobility must contribute to activity scoring");
    require(evaluator.evaluate(lone_bishop, koi::Color::white) == 0,
            "insufficient material must evaluate as a forced draw");
}

void test_evaluator_increases_advanced_passed_pawn_value_in_the_endgame() {
    koi::ClassicalEvaluator evaluator;
    const koi::GameState endgame = require_state("4k3/8/8/3P4/8/8/8/4K3 w - - 0 1");
    const koi::GameState middlegame = require_state("4q2k/8/8/3P4/8/8/8/4Q2K w - - 0 1");

    const auto endgame_score = evaluator.breakdown(endgame, koi::Color::white);
    const auto middlegame_score = evaluator.breakdown(middlegame, koi::Color::white);
    require(endgame_score.pawn_structure > middlegame_score.pawn_structure,
            "an advanced passed pawn must receive additional weight as material leaves the board");
}

void test_evaluator_penalizes_a_directly_blockaded_passed_pawn() {
    koi::ClassicalEvaluator evaluator;
    const auto advanced = evaluator.breakdown(require_evaluation_fixture("advanced_passed"), koi::Color::white);
    const auto blockaded = evaluator.breakdown(require_evaluation_fixture("blockaded_passed"), koi::Color::white);

    require(advanced.pawn_structure >= blockaded.pawn_structure + 10,
            "a directly blockaded passed pawn must lose meaningful structure credit against an unobstructed passer");
}

void test_evaluator_mirrors_terms_and_black_passed_pawn_blockades() {
    koi::ClassicalEvaluator evaluator;
    const auto white_terms = evaluator.breakdown(require_evaluation_fixture("term_mirror_white"), koi::Color::white);
    const auto black_terms = evaluator.breakdown(require_evaluation_fixture("term_mirror_black"), koi::Color::black);

    require(white_terms.material == black_terms.material &&
                white_terms.piece_square == black_terms.piece_square &&
                white_terms.mobility == black_terms.mobility &&
                white_terms.pawn_structure == black_terms.pawn_structure &&
                white_terms.activity == black_terms.activity &&
                white_terms.king_safety == black_terms.king_safety &&
                white_terms.total == black_terms.total,
            "a color-and-rank mirrored position must preserve every evaluator term");
    require(white_terms.mobility != 0 && white_terms.pawn_structure != 0 &&
                white_terms.activity != 0 && white_terms.king_safety != 0,
            "the mirrored evaluator fixture must exercise mobility, pawn, activity, and king-safety terms");

    const auto black_advanced =
        evaluator.breakdown(require_evaluation_fixture("black_advanced_passed"), koi::Color::black);
    const auto black_blockaded =
        evaluator.breakdown(require_evaluation_fixture("black_blockaded_passed"), koi::Color::black);
    require(black_advanced.pawn_structure >= black_blockaded.pawn_structure + 10,
            "a directly blockaded black passed pawn must lose meaningful structure credit");
}

void test_evaluator_endgame_passer_scaling_is_color_symmetric() {
    koi::ClassicalEvaluator evaluator;
    const auto white_endgame =
        evaluator.breakdown(require_evaluation_fixture("endgame_passer_white"), koi::Color::white);
    const auto white_middlegame =
        evaluator.breakdown(require_evaluation_fixture("middlegame_passer_white"), koi::Color::white);
    const auto black_endgame =
        evaluator.breakdown(require_evaluation_fixture("endgame_passer_black"), koi::Color::black);
    const auto black_middlegame =
        evaluator.breakdown(require_evaluation_fixture("middlegame_passer_black"), koi::Color::black);

    require(white_endgame.pawn_structure > white_middlegame.pawn_structure &&
                black_endgame.pawn_structure > black_middlegame.pawn_structure,
            "advanced passed pawns must gain endgame structure weight for both colors");
    require(white_endgame.pawn_structure == black_endgame.pawn_structure &&
                white_middlegame.pawn_structure == black_middlegame.pawn_structure,
            "endgame passed-pawn scaling must remain color symmetric");
}

void test_time_manager_applies_move_time_and_clock_limits() {
    koi::SearchLimits move_time;
    move_time.movetime = 100ms;
    move_time.white_clock = koi::ClockLimit{10s, 1s};
    koi::TimeManager fixed(move_time, koi::Color::white);
    require(fixed.time_budget().has_value(), "movetime must create a time budget");
    require(*fixed.time_budget() <= 100ms, "movetime must take precedence over clock allocation");

    koi::SearchLimits clock;
    clock.white_clock = koi::ClockLimit{10s, 1s};
    clock.moves_to_go = 20;
    koi::TimeManager allocated(clock, koi::Color::white);
    require(allocated.time_budget().has_value(), "side-to-move clock must create a time budget");
    require(*allocated.time_budget() > 0ms && *allocated.time_budget() < 10s,
            "clock allocation must retain a safety margin");

    koi::SearchLimits nodes;
    nodes.nodes = 5;
    koi::TimeManager node_limited(nodes, koi::Color::white);
    require(!node_limited.should_stop(4), "node limit must allow work below its boundary");
    require(node_limited.should_stop(5), "node limit must stop at its boundary");

    koi::SearchLimits infinite;
    infinite.infinite = true;
    infinite.movetime = 1ms;
    koi::TimeManager unbounded(infinite, koi::Color::white);
    require(!unbounded.time_budget().has_value(), "infinite search must ignore a time budget");

    koi::SearchLimits infinite_nodes;
    infinite_nodes.infinite = true;
    infinite_nodes.nodes = 1;
    koi::TimeManager infinite_node_limited(infinite_nodes, koi::Color::white);
    require(infinite_node_limited.node_limit() == 1,
            "infinite search must retain an explicit node limit");
    require(infinite_node_limited.should_stop(1),
            "infinite search must stop when its explicit node limit is reached");
}

void test_speed_scales_only_time_based_search_budgets() {
    koi::SearchLimits move_time;
    move_time.movetime = 1s;
    const koi::TimeManager normal(move_time, koi::Color::white, 100);
    const koi::TimeManager faster(move_time, koi::Color::white, 50);
    require(normal.time_budget().has_value() && faster.time_budget().has_value(),
            "speed-controlled movetime searches must have budgets");
    require(*faster.time_budget() < *normal.time_budget(),
            "Speed below 100 must reduce a movetime budget");

    koi::SearchLimits clock;
    clock.white_clock = koi::ClockLimit{10s, 1s};
    clock.moves_to_go = 20;
    const koi::TimeManager normal_clock(clock, koi::Color::white, 100);
    const koi::TimeManager faster_clock(clock, koi::Color::white, 50);
    require(*faster_clock.time_budget() < *normal_clock.time_budget(),
            "Speed below 100 must reduce a clock allocation");

    koi::SearchLimits very_large;
    very_large.movetime = std::chrono::milliseconds::max();
    const koi::TimeManager scaled_large(very_large, koi::Color::white, 50);
    require(scaled_large.time_budget().has_value() &&
                *scaled_large.time_budget() < std::chrono::milliseconds::max() / 2,
            "Speed must scale very large time values without overflow");

    koi::SearchLimits very_large_clock;
    very_large_clock.white_clock = koi::ClockLimit{std::chrono::milliseconds::max(),
                                                    std::chrono::milliseconds::max()};
    very_large_clock.moves_to_go = 2;
    const koi::TimeManager scaled_large_clock(very_large_clock, koi::Color::white, 50);
    require(scaled_large_clock.time_budget().has_value() &&
                *scaled_large_clock.time_budget() > std::chrono::milliseconds::max() / 2 - 100ms,
            "Clock allocation must saturate before scaling very large increments");

    koi::SearchLimits depth;
    depth.depth = 4;
    koi::SearchLimits nodes;
    nodes.nodes = 100;
    koi::SearchLimits infinite;
    infinite.infinite = true;
    require(!koi::TimeManager(depth, koi::Color::white, 1).time_budget().has_value(),
            "Speed must not add a time budget to depth searches");
    require(!koi::TimeManager(nodes, koi::Color::white, 1).time_budget().has_value(),
            "Speed must not add a time budget to node searches");
    require(!koi::TimeManager(infinite, koi::Color::white, 1).time_budget().has_value(),
            "Speed must not add a time budget to infinite searches");
}

void test_search_options_include_thread_and_speed_controls() {
    const koi::SearchOptions defaults;
    require(defaults.threads == 1, "SearchOptions must default to one search thread");
    require(defaults.speed_percent == 100, "SearchOptions must default to Speed 100");

    koi::SearchOptions configured;
    configured.threads = 2;
    configured.speed_percent = 50;
    require(configured.threads == 2 && configured.speed_percent == 50,
            "SearchOptions must retain explicit thread and speed values");
}

void test_root_filtering_keeps_only_requested_legal_move() {
    const auto expected = koi::Move::parse_uci("e2e4");
    require(expected.has_value(), "the requested root move must parse");

    koi::SearchLimits limits;
    limits.depth = 2;
    limits.search_moves_specified = true;
    limits.search_moves = {*expected};

    koi::SearchService service(std::make_shared<koi::ClassicalEvaluator>());
    const koi::SearchResult result = search(service, koi::GameState::startpos(), limits);

    require(result.completed_depth == 2, "a filtered root search must complete the requested depth");
    require(result.best_move == expected, "root filtering must retain the requested legal move only");
}

void test_root_filtering_ignores_syntactically_valid_illegal_move() {
    const auto illegal = koi::Move::parse_uci("a1a1");
    require(illegal.has_value(), "the syntactically valid illegal root move must parse");

    koi::SearchLimits limits;
    limits.depth = 2;
    limits.search_moves_specified = true;
    limits.search_moves = {*illegal};

    koi::SearchService service(std::make_shared<koi::ClassicalEvaluator>());
    const koi::SearchResult result = search(service, koi::GameState::startpos(), limits);

    require(!result.best_move.has_value(), "an empty filtered root must not return a best move");
}

void test_deterministic_multipv_reports_sorted_distinct_legal_lines() {
    const auto run = [] {
        std::vector<koi::SearchInfo> infos;
        std::optional<koi::SearchResult> result;
        koi::SearchLimits limits;
        limits.depth = 2;
        const koi::SearchOptions options{.multi_pv = 3};
        koi::SearchService service(std::make_shared<koi::ClassicalEvaluator>());
        koi::SearchHandle handle = service.start(
            koi::GameState::startpos(), limits,
            {.on_info = [&infos](const koi::SearchInfo& info) { infos.push_back(info); },
             .on_complete = [&result](const koi::SearchResult& completed) { result = completed; }},
            options);
        handle.wait();

        require(result.has_value(), "MultiPV search must report a completion result");
        std::vector<koi::SearchInfo> final_depth;
        for (const koi::SearchInfo& info : infos) {
            if (info.depth == 2) {
                final_depth.push_back(info);
            }
        }
        require(final_depth.size() == 3,
                "MultiPV must report three final-depth lines when three legal roots exist");
        const koi::GameState root = koi::GameState::startpos();
        std::vector<koi::Move> first_moves;
        for (std::size_t index = 0; index < final_depth.size(); ++index) {
            const koi::SearchInfo& info = final_depth[index];
            require(info.multipv == static_cast<int>(index + 1),
                    "MultiPV ranks must be consecutive starting at one");
            if (index > 0) {
                require(final_depth[index - 1].score_cp >= info.score_cp,
                        "MultiPV lines must be sorted by descending score");
            }
            require(!info.pv.empty(), "each MultiPV line must contain a principal variation");
            koi::GameState line_state = root;
            for (const koi::Move& move : info.pv) {
                require(line_state.is_legal(move), "MultiPV PV moves must remain legal");
                require(line_state.make_move(move), "MultiPV PV moves must be applicable in sequence");
            }
            require(std::find(first_moves.begin(), first_moves.end(), info.pv.front()) == first_moves.end(),
                    "MultiPV root moves must be distinct");
            first_moves.push_back(info.pv.front());
        }
        require(result->best_move == final_depth.front().pv.front() &&
                    result->score_cp == final_depth.front().score_cp &&
                    result->mate == final_depth.front().mate,
                "completion result must match the rank-one MultiPV line");
        return std::pair{std::move(final_depth), *result};
    };

    const auto first = run();
    const auto second = run();
    require(first.first.size() == second.first.size(),
            "repeated MultiPV searches must report the same number of lines");
    for (std::size_t index = 0; index < first.first.size(); ++index) {
        require(first.first[index].multipv == second.first[index].multipv &&
                    first.first[index].score_cp == second.first[index].score_cp &&
                    first.first[index].pv == second.first[index].pv,
                "repeated MultiPV searches must report identical rank, score, and PV data");
    }
}

void test_threaded_search_uses_multiple_root_workers_and_matches_reference_result() {
    auto threaded_evaluator = std::make_shared<ConcurrencyEvaluator>();
    koi::SearchService threaded_service(threaded_evaluator);
    koi::SearchOptions threaded_options;
    threaded_options.threads = 2;
    koi::SearchLimits limits;
    limits.depth = 2;
    const koi::SearchResult threaded =
        search(threaded_service, koi::GameState::startpos(), limits, threaded_options);

    auto reference_evaluator = std::make_shared<ConcurrencyEvaluator>();
    koi::SearchService reference_service(reference_evaluator);
    const koi::SearchResult reference = search(reference_service, koi::GameState::startpos(), limits);

    if (koi::maximum_search_threads() > 1) {
        require(threaded_evaluator->maximum_active() >= 2,
                "Threads greater than one must evaluate independent root jobs concurrently");
    } else {
        require(threaded_evaluator->maximum_active() >= 1,
                "a single-thread host must still evaluate the root search");
    }
    require(threaded.best_move.has_value() && reference.best_move.has_value(),
            "threaded and reference searches must both return a root move");
    require(*threaded.best_move == *reference.best_move && threaded.score_cp == reference.score_cp,
            "threaded fixed-depth search must match the single-thread reference result");
}

void test_classical_threaded_search_matches_reference_result() {
    const koi::GameState root = require_state(
        "r1bqk2r/pppp1ppp/2n2n2/8/2B5/2N5/PPPP1PPP/R1BQK2R w KQkq - 0 1");
    koi::SearchLimits limits;
    limits.depth = 3;

    koi::SearchService reference_service(std::make_shared<koi::ClassicalEvaluator>());
    const koi::SearchResult reference = search(reference_service, root, limits);

    koi::SearchOptions options;
    options.threads = std::min<std::size_t>(2, koi::maximum_search_threads());
    koi::SearchService threaded_service(std::make_shared<koi::ClassicalEvaluator>());
    const koi::SearchResult threaded = search(threaded_service, root, limits, options);

    require(reference.best_move.has_value() && threaded.best_move.has_value() &&
                root.is_legal(*reference.best_move) && root.is_legal(*threaded.best_move),
            "classical reference and threaded searches must return legal moves");
    require(*reference.best_move == *threaded.best_move && reference.score_cp == threaded.score_cp,
            "classical threaded search must match the single-thread reference result");
}

void test_equal_root_scores_keep_the_earliest_ordered_move() {
    const koi::GameState root = require_state("4k3/8/8/8/8/8/P6r/4K2R w - - 0 1");
    const auto expected = koi::Move::parse_uci("h1h2");
    require(expected.has_value(), "the ordered capture fixture must parse");

    koi::SearchLimits limits;
    limits.depth = 1;

    auto reference_evaluator = std::make_shared<ConcurrencyEvaluator>();
    koi::SearchService reference_service(reference_evaluator);
    const koi::SearchResult reference = search(reference_service, root, limits);
    require(reference.best_move == expected,
            "single-thread equal scores must retain the earliest move from root ordering");

    auto threaded_evaluator = std::make_shared<ConcurrencyEvaluator>();
    koi::SearchService threaded_service(threaded_evaluator);
    koi::SearchOptions options;
    options.threads = 2;
    const koi::SearchResult threaded = search(threaded_service, root, limits, options);
    require(threaded.best_move == expected,
            "threaded equal scores must retain the earliest move from root ordering");
}

void test_threaded_multipv_equal_scores_use_stable_ordered_root_tie_breaking() {
    const koi::GameState root = require_state("4k3/8/8/8/8/8/P6r/4K2R w - - 0 1");
    const std::vector<koi::Move> generated_moves = root.legal_moves();
    require(!generated_moves.empty(), "the equal-score fixture must have legal root moves");
    const auto ordered_capture = koi::Move::parse_uci("h1h2");
    require(ordered_capture.has_value() && generated_moves.front() != *ordered_capture,
            "the fixture must distinguish generated legal order from root move ordering");

    koi::SearchLimits limits;
    limits.depth = 2;
    koi::SearchOptions options;
    options.threads = 2;
    options.multi_pv = 3;

    std::vector<koi::SearchInfo> infos;
    std::optional<koi::SearchResult> result;
    koi::SearchService service(std::make_shared<ConcurrencyEvaluator>());
    koi::SearchHandle handle = service.start(
        root, limits,
        {.on_info = [&infos](const koi::SearchInfo& info) { infos.push_back(info); },
         .on_complete = [&result](const koi::SearchResult& completed) { result = completed; }},
        options);
    handle.wait();

    std::vector<koi::SearchInfo> final_depth;
    for (const koi::SearchInfo& info : infos) {
        if (info.depth == limits.depth) {
            final_depth.push_back(info);
        }
    }
    require(final_depth.size() == 3,
            "threaded MultiPV must report three final-depth lines for three legal roots");
    std::vector<koi::Move> first_moves;
    for (std::size_t index = 0; index < final_depth.size(); ++index) {
        const koi::SearchInfo& info = final_depth[index];
        require(info.multipv == static_cast<int>(index + 1),
                "threaded MultiPV ranks must be consecutive starting at one");
        require(!info.pv.empty() && root.is_legal(info.pv.front()),
                "threaded MultiPV must report legal root moves");
        require(std::find(first_moves.begin(), first_moves.end(), info.pv.front()) == first_moves.end(),
                "threaded MultiPV root moves must be distinct");
        first_moves.push_back(info.pv.front());
    }
    require(final_depth.front().pv.front() == *ordered_capture,
            "equal-score threaded MultiPV must retain the earliest stable ordered root move");
    require(result.has_value() && result->best_move == final_depth.front().pv.front() &&
                result->score_cp == final_depth.front().score_cp,
            "threaded MultiPV completion must match its rank-one line");
}

void test_threaded_multipv_matches_single_thread_at_final_depth() {
    const koi::GameState root = require_state("4k3/8/8/8/8/8/P6r/4K2R w - - 0 1");
    koi::SearchLimits limits;
    limits.depth = 2;

    const auto run = [&](std::size_t threads) {
        koi::SearchOptions options;
        options.threads = threads;
        options.multi_pv = 3;
        std::vector<koi::SearchInfo> final_depth;
        std::optional<koi::SearchResult> result;
        koi::SearchService service(std::make_shared<ConcurrencyEvaluator>());
        koi::SearchHandle handle = service.start(
            root, limits,
            {.on_info = [&final_depth, &limits](const koi::SearchInfo& info) {
                 if (info.depth == limits.depth) {
                     final_depth.push_back(info);
                 }
             },
             .on_complete = [&result](const koi::SearchResult& completed) { result = completed; }},
            options);
        handle.wait();
        require(result.has_value() && final_depth.size() == 3,
                "each final-depth MultiPV search must report three ranked lines");
        return std::pair{std::move(final_depth), *result};
    };

    const auto single_thread = run(1);
    const auto threaded = run(2);
    for (std::size_t index = 0; index < single_thread.first.size(); ++index) {
        const koi::SearchInfo& reference = single_thread.first[index];
        const koi::SearchInfo& candidate = threaded.first[index];
        require(reference.multipv == candidate.multipv && reference.score_cp == candidate.score_cp &&
                    reference.mate == candidate.mate && !reference.pv.empty() && !candidate.pv.empty() &&
                    reference.pv.front() == candidate.pv.front() && reference.pv == candidate.pv,
                "Threads=1 and Threads=2 must retain rank, score, root move, and PV at final depth");
    }
    require(single_thread.second.best_move == single_thread.first.front().pv.front() &&
                threaded.second.best_move == threaded.first.front().pv.front(),
            "MultiPV completion must retain the final-depth rank-one root move for both thread counts");
}

void test_threaded_multipv_is_stable_after_warming_the_shared_hash() {
    const koi::GameState root = require_state("4k3/8/8/8/8/8/P6r/4K2R w - - 0 1");
    const auto ordered_capture = koi::Move::parse_uci("h1h2");
    require(ordered_capture.has_value(), "the stable-order fixture must parse");

    koi::SearchLimits limits;
    limits.depth = 2;
    koi::SearchOptions options;
    options.threads = 2;
    options.multi_pv = 3;
    koi::SearchService service(std::make_shared<ConcurrencyEvaluator>());

    const auto run = [&] {
        std::vector<koi::SearchInfo> final_depth;
        std::optional<koi::SearchResult> result;
        koi::SearchHandle handle = service.start(
            root, limits,
            {.on_info = [&final_depth](const koi::SearchInfo& info) {
                 if (info.depth == 2) {
                     final_depth.push_back(info);
                 }
             },
             .on_complete = [&result](const koi::SearchResult& completed) { result = completed; }},
            options);
        handle.wait();
        require(result.has_value() && final_depth.size() == 3,
                "repeated threaded MultiPV searches must finish three final-depth lines");
        require(final_depth.front().pv.front() == *ordered_capture,
                "warmed threaded MultiPV must retain the stable ordered root prefix");
        return std::pair{std::move(final_depth), *result};
    };

    (void)run();
    const auto first = run();
    const auto second = run();
    require(first.second.stats.tt_hits > 0 && second.second.stats.tt_hits > 0,
            "repeated threaded MultiPV searches must probe entries warmed through the shared hash");
    for (std::size_t index = 0; index < first.first.size(); ++index) {
        require(first.first[index].score_cp == second.first[index].score_cp &&
                    first.first[index].pv == second.first[index].pv &&
                    first.first[index].multipv == second.first[index].multipv,
                "warmed threaded MultiPV must remain deterministic across repeated searches");
    }
}

void test_threaded_node_limit_is_global_and_never_exceeded() {
    auto evaluator = std::make_shared<koi::ClassicalEvaluator>();
    koi::SearchService service(evaluator);
    koi::SearchLimits limits;
    limits.nodes = 37;
    koi::SearchOptions options;
    options.threads = 2;

    const koi::SearchResult result = search(service, koi::GameState::startpos(), limits, options);
    require(result.stats.nodes + result.stats.qnodes <= 37,
            "a threaded search must enforce one global node limit across all workers");
    require(result.best_move.has_value() && koi::GameState::startpos().is_legal(*result.best_move),
            "a globally node-limited search must retain a legal fallback move");
}

void test_threaded_and_reference_node_limits_have_matching_accounting() {
    auto evaluator = std::make_shared<koi::ClassicalEvaluator>();
    koi::SearchLimits limits;
    limits.depth = 1;
    limits.nodes = 3;

    koi::SearchService reference_service(evaluator);
    const koi::SearchResult reference = search(reference_service, koi::GameState::startpos(), limits);

    koi::SearchService threaded_service(std::make_shared<koi::ClassicalEvaluator>());
    koi::SearchOptions options;
    options.threads = 2;
    const koi::SearchResult threaded = search(threaded_service, koi::GameState::startpos(), limits, options);

    require(reference.stats.nodes + reference.stats.qnodes == 3 &&
                threaded.stats.nodes + threaded.stats.qnodes == 3,
            "node-limited searches must account for the root and stop at the exact global boundary");
    require(reference.completed_depth == threaded.completed_depth &&
                reference.best_move == threaded.best_move && reference.score_cp == threaded.score_cp,
            "threaded and reference searches must have matching partial-search results");
}

void test_search_info_nodes_reports_all_visited_nodes() {
    auto evaluator = std::make_shared<koi::ClassicalEvaluator>();
    koi::SearchService service(evaluator);
    koi::SearchLimits limits;
    limits.depth = 2;
    std::optional<koi::SearchInfo> last_info;
    std::optional<koi::SearchResult> result;
    koi::SearchHandle handle = service.start(koi::GameState::startpos(), limits,
        {.on_info = [&last_info](const koi::SearchInfo& info) { last_info = info; },
         .on_complete = [&result](const koi::SearchResult& completed) { result = completed; }});
    handle.wait();

    require(last_info.has_value() && result.has_value(), "a completed search must report info and a result");
    require(last_info->nodes == result->stats.nodes + result->stats.qnodes,
            "search info node count must include all work used for nps");
}

void test_threaded_infinite_search_cancels_and_completes_once() {
    auto evaluator = std::make_shared<ConcurrencyEvaluator>();
    koi::SearchService service(evaluator);
    koi::SearchLimits limits;
    limits.infinite = true;
    limits.depth = 1;
    koi::SearchOptions options;
    options.threads = 2;
    CompletedSearch completed;

    koi::SearchHandle handle = service.start(koi::GameState::startpos(), limits, completed.sink(), options);
    std::this_thread::sleep_for(20ms);
    const auto stop_started = std::chrono::steady_clock::now();
    handle.stop();
    handle.wait();
    const auto stop_elapsed = std::chrono::steady_clock::now() - stop_started;

    require(!handle.running(), "a stopped threaded search must join all internal workers");
    require(stop_elapsed < 2s, "threaded cancellation must return promptly");
    const koi::SearchResult result = completed.take_result();
    require(result.best_move.has_value() && koi::GameState::startpos().is_legal(*result.best_move),
            "a cancelled threaded search must return a legal best move");
}

void test_non_concurrent_evaluators_are_serialized_across_simultaneous_handles() {
    auto evaluator = std::make_shared<ConcurrencyEvaluator>(false);
    koi::SearchService service(evaluator);
    koi::SearchLimits limits;
    limits.depth = 1;
    koi::SearchOptions options;
    options.threads = 2;

    koi::SearchHandle first = service.start(koi::GameState::startpos(), limits, {}, options);
    koi::SearchHandle second = service.start(koi::GameState::startpos(), limits, {}, options);
    first.wait();
    second.wait();

    require(evaluator->maximum_active() == 1,
            "a non-concurrent evaluator must be serialized across handles sharing one service");
}

void test_terminal_roots_return_mate_or_stalemate_scores() {
    auto evaluator = std::make_shared<koi::ClassicalEvaluator>();
    koi::SearchService service(evaluator);
    koi::SearchLimits limits;
    limits.depth = 3;

    const koi::SearchResult mate = search(
        service, require_state("7k/6Q1/5K2/8/8/8/8/8 b - - 0 1"), limits);
    require(!mate.best_move.has_value(), "a checkmated root must not return a move");
    require(mate.score_cp < -90000, "a checkmated side must receive a terminal losing score");
    require(mate.mate.has_value() && *mate.mate <= 0, "a checkmated side must report mate");

    const koi::SearchResult stalemate = search(
        service, require_state("7k/5Q2/6K1/8/8/8/8/8 b - - 0 1"), limits);
    require(!stalemate.best_move.has_value(), "a stalemated root must not return a move");
    require(stalemate.score_cp == 0 && !stalemate.mate.has_value(),
            "a stalemate must receive a draw score without mate");
}

void test_fixed_depth_search_is_deterministic_and_legal() {
    auto evaluator = std::make_shared<koi::ClassicalEvaluator>();
    koi::SearchService service(evaluator);
    koi::SearchLimits limits;
    limits.depth = 3;
    const koi::GameState root = koi::GameState::startpos();

    const koi::SearchResult first = search(service, root, limits);
    const koi::SearchResult second = search(service, root, limits);

    require(first.completed_depth == 3 && second.completed_depth == 3,
            "fixed-depth searches must complete the requested depth");
    require(first.best_move.has_value() && second.best_move.has_value(),
            "non-terminal roots must return a best move");
    require(*first.best_move == *second.best_move && first.score_cp == second.score_cp,
            "identical roots and limits must produce identical results");
    require(root.is_legal(*first.best_move), "search must only return legal root moves");
}

void test_fixed_depth_tactical_reference_output_is_preserved() {
    koi::SearchService service(std::make_shared<koi::ClassicalEvaluator>());
    koi::SearchLimits limits;
    limits.depth = 2;
    const koi::GameState root = require_state("4k3/8/8/3q4/4Q3/8/8/4K3 w - - 0 1");

    const koi::SearchResult result = search(service, root, limits);
    const auto expected = koi::Move::parse_uci("e4d5");
    require(expected.has_value(), "fixed-depth tactical reference move must parse");
    require(result.completed_depth == 2 && result.best_move == expected && result.score_cp == 1026,
            "single-thread fixed-depth tactical output must retain its reviewed move and score");
}

void test_search_reports_tactical_search_statistics() {
    auto evaluator = std::make_shared<koi::ClassicalEvaluator>();
    koi::SearchService service(evaluator);
    koi::SearchLimits limits;
    limits.depth = 3;

    const koi::SearchResult result = search(service, koi::GameState::startpos(), limits);
    require(result.stats.seldepth >= 3, "search must report the deepest visited ply");
    require(result.stats.pvs_searches > 0, "search must use principal-variation search after the first move");
}

void test_search_extends_checked_positions() {
    auto evaluator = std::make_shared<koi::ClassicalEvaluator>();
    koi::SearchService service(evaluator);
    koi::SearchLimits limits;
    limits.depth = 2;

    const koi::SearchResult result = search(
        service, require_state("7k/5Q2/6K1/8/8/8/8/8 w - - 0 1"), limits);
    require(result.stats.check_extensions > 0,
            "a checking tactical line must use at least one check extension");
}

void test_search_keeps_a_forced_quiet_evasion() {
    koi::SearchService service(std::make_shared<koi::ClassicalEvaluator>());
    koi::SearchLimits limits;
    limits.depth = 3;
    const koi::GameState root = require_state("k3r3/8/8/8/8/8/3N1N2/2NNKN2 w - - 0 1");
    const auto quiet_block = koi::Move::parse_uci("c1e2");
    require(quiet_block.has_value() && root.in_check(),
            "the quiet-defense fixture must begin with a checked king and a parsable interposition");
    const std::vector<koi::Move> evasions = root.legal_moves();
    require(std::find(evasions.begin(), evasions.end(), *quiet_block) != evasions.end() &&
                std::all_of(evasions.begin(), evasions.end(), [&root](const koi::Move& move) {
                    return !root.is_capture(move) && move.promotion() == koi::Promotion::none;
                }),
            "the quiet-defense fixture must allow only non-capturing interpositions");

    const koi::SearchResult result = search(service, root, limits);
    require(result.completed_depth == 3 && result.best_move.has_value() &&
                std::find(evasions.begin(), evasions.end(), *result.best_move) != evasions.end(),
            "search must retain a quiet interposition while resolving check");
}

void test_search_reports_mate_in_one_distance() {
    koi::SearchService service(std::make_shared<koi::ClassicalEvaluator>());
    koi::SearchLimits limits;
    limits.depth = 3;
    const koi::SearchResult result = search(
        service, require_state("7k/5Q2/6K1/8/8/8/8/8 w - - 0 1"), limits);

    require(result.best_move.has_value() && result.mate == std::optional<int>{1},
            "a forced mate in one must report a one-move mate distance");
}

void test_pawn_only_zugzwang_search_skips_null_pruning() {
    koi::SearchService service(std::make_shared<koi::ClassicalEvaluator>());
    koi::SearchLimits limits;
    limits.depth = 5;
    const koi::GameState root = require_state("8/8/8/3k4/3P4/3K4/8/8 w - - 0 1");
    const koi::SearchResult result = search(service, root, limits);

    require(result.best_move.has_value() && root.is_legal(*result.best_move),
            "a pawn-only zugzwang search must return a legal move");
    require(result.stats.null_cutoffs == 0,
            "null-move pruning must stay disabled in the pawn-only zugzwang endgame");
}

void test_search_reduces_late_quiet_moves_without_losing_root_legality() {
    koi::SearchService service(std::make_shared<koi::ClassicalEvaluator>());
    koi::SearchLimits limits;
    limits.depth = 4;
    const koi::GameState root = koi::GameState::startpos();
    const koi::SearchResult result = search(service, root, limits);

    require(result.best_move.has_value() && root.is_legal(*result.best_move),
            "late-move reduction search must preserve a legal root move");
    require(result.stats.lmr_reductions > 0,
            "a multi-move quiet root must exercise late-move reductions");
}

void test_quiescence_keeps_searching_checked_evasions_past_normal_cap() {
    auto evaluator = std::make_shared<CheckedPositionEvaluator>();
    koi::SearchService service(evaluator);
    koi::SearchLimits limits;
    limits.depth = 4;

    const koi::GameState root = require_state(
        "7k/2q2q2/1r1r1r2/8/8/1R1R1R2/2Q2Q2/K7 w - - 0 1");
    const koi::SearchResult result = search(service, root, limits);
    require(result.best_move.has_value() && root.is_legal(*result.best_move),
            "the long checking line must still produce a legal root move");
    require(evaluator->checked_evaluations() == 0,
            "quiescence must search legal evasions instead of statically evaluating checked nodes");
}

void test_quiescence_keeps_bounded_checking_continuations() {
    koi::SearchService service(std::make_shared<koi::ClassicalEvaluator>());
    koi::SearchLimits limits;
    limits.depth = 2;
    const koi::SearchResult result = search(
        service, require_state("4k3/8/8/3p4/4Q3/8/8/4K3 w - - 0 1"), limits);
    require(result.stats.qchecks > 0,
            "quiescence must retain checking continuations before its checking horizon");
}

void test_iterative_deepening_uses_aspiration_windows() {
    auto evaluator = std::make_shared<koi::ClassicalEvaluator>();
    koi::SearchService service(evaluator);
    koi::SearchLimits limits;
    limits.depth = 3;

    const koi::SearchResult result = search(service, koi::GameState::startpos(), limits);
    require(result.stats.aspiration_researches > 0,
            "a changed iterative score must trigger an aspiration-window re-search");
}

void test_infinite_search_runs_until_stopped_and_completes_once() {
    auto evaluator = std::make_shared<koi::ClassicalEvaluator>();
    koi::SearchService service(evaluator);
    koi::SearchLimits limits;
    limits.infinite = true;
    limits.depth = 1;
    CompletedSearch completed;

    koi::SearchHandle handle = service.start(koi::GameState::startpos(), limits, completed.sink());
    std::this_thread::sleep_for(50ms);
    require(handle.running(), "infinite search must remain running instead of completing its depth limit");
    require(completed.completion_count() == 0, "infinite search must not complete before cancellation");

    handle.stop();
    handle.wait();
    require(!handle.running(), "stopped infinite search must join its worker");
    const koi::SearchResult result = completed.take_result();
    require(result.best_move.has_value(), "stopped non-terminal infinite search must retain a legal fallback move");
}

void test_ponder_search_runs_until_stopped_and_completes_once() {
    auto evaluator = std::make_shared<koi::ClassicalEvaluator>();
    koi::SearchService service(evaluator);
    koi::SearchLimits limits;
    limits.depth = 1;
    limits.ponder = true;
    limits.movetime = 1ms;
    limits.white_clock = koi::ClockLimit{1ms, 0ms};
    CompletedSearch completed;

    koi::SearchHandle handle = service.start(koi::GameState::startpos(), limits, completed.sink());
    std::this_thread::sleep_for(50ms);
    require(handle.running(), "ponder search must remain running instead of completing its depth limit");
    require(completed.completion_count() == 0, "ponder search must not complete before cancellation");

    handle.stop();
    handle.wait();
    require(!handle.running(), "stopped ponder search must join its worker");
    const koi::SearchResult result = completed.take_result();
    require(result.best_move.has_value(), "stopped ponder search must retain a legal fallback move");
}

void test_ponder_terminal_and_empty_roots_wait_for_stop() {
    auto evaluator = std::make_shared<koi::ClassicalEvaluator>();
    koi::SearchService service(evaluator);

    const auto require_parked = [&service](koi::GameState root, koi::SearchLimits limits,
                                           std::string_view description) {
        CompletedSearch completed;
        koi::SearchHandle handle = service.start(std::move(root), std::move(limits), completed.sink());
        std::this_thread::sleep_for(20ms);
        require(handle.running(), std::string("ponder ") + std::string(description) +
                                      " must remain running until stop");
        require(completed.completion_count() == 0, std::string("ponder ") + std::string(description) +
                                                   " must not complete before stop");

        handle.stop();
        handle.wait();
        const koi::SearchResult result = completed.take_result();
        require(!result.best_move.has_value(), std::string("stopped ponder ") + std::string(description) +
                                                 " must report no legal best move");
    };

    koi::SearchLimits terminal_limits;
    terminal_limits.ponder = true;
    require_parked(require_state("7k/6Q1/5K2/8/8/8/8/8 b - - 0 1"), terminal_limits, "checkmate root");

    koi::SearchLimits draw_limits;
    draw_limits.ponder = true;
    require_parked(require_state("4k3/8/8/8/8/8/8/R3K3 w - - 100 1"), draw_limits, "rule-draw root");

    koi::SearchLimits empty_filter_limits;
    empty_filter_limits.ponder = true;
    empty_filter_limits.search_moves_specified = true;
    require_parked(koi::GameState::startpos(), empty_filter_limits, "empty searchmoves root");
}

void test_ponder_ignores_time_but_honors_node_limits() {
    koi::SearchLimits limits;
    limits.ponder = true;
    limits.movetime = 1ms;
    limits.nodes = 1;
    limits.white_clock = koi::ClockLimit{1ms, 0ms};

    const koi::TimeManager manager(limits, koi::Color::white);
    require(!manager.time_budget().has_value(), "ponder must ignore movetime and clock budgets");
    require(manager.node_limit() == 1, "ponder must retain an explicit node limit");
    require(manager.should_stop(1), "ponder must stop at its explicit node limit");
}

void test_service_hash_configuration_survives_default_start_and_non_default_override() {
    auto evaluator = std::make_shared<koi::ClassicalEvaluator>();
    koi::SearchService service(evaluator);
    koi::SearchLimits limits;
    limits.depth = 1;

    service.set_hash_size_mb(1);
    require(service.hash_size_mb() == 1, "service hash configuration must accept 1 MB");
    search(service, koi::GameState::startpos(), limits);
    require(service.hash_size_mb() == 1, "default SearchOptions must preserve the configured service hash size");

    CompletedSearch completed;
    koi::SearchOptions override;
    override.hash_mb = 2;
    koi::SearchHandle handle = service.start(koi::GameState::startpos(), limits, completed.sink(), override);
    handle.wait();
    completed.take_result();
    require(service.hash_size_mb() == 2, "a non-default per-start hash option must explicitly reconfigure the service hash");
}

void test_hash_configuration_clamps_to_uci_bounds_and_clear_discards_warmed_entries() {
    auto evaluator = std::make_shared<koi::ClassicalEvaluator>();
    koi::SearchService service(evaluator);
    koi::SearchLimits limits;
    limits.depth = 3;

    require(service.hash_size_mb() == 16, "SearchService must retain the 16 MB default hash size");
    const koi::SearchResult warmed = search(service, koi::GameState::startpos(), limits);
    require(warmed.stats.tt_hits > 0, "a completed iterative search must warm the transposition table");

    service.clear_hash();
    koi::SearchLimits one_ply;
    one_ply.depth = 1;
    const koi::SearchResult cleared = search(service, koi::GameState::startpos(), one_ply);
    require(cleared.stats.tt_hits == 0, "Clear Hash must remove entries used by a subsequent root search");

    service.set_hash_size_mb(0);
    require(service.hash_size_mb() == 1, "hash size must clamp to the UCI lower bound of 1 MB");
}

void test_transposition_table_stores_probes_and_clears_entries() {
    koi::TranspositionTable table;
    require(table.size_mb() == 16, "transposition table default must be 16 MB");
    const auto move = koi::Move::parse_uci("e2e4");
    require(move.has_value(), "test move must parse");

    require(!table.probe(0).has_value(), "a fresh table must not report a zero-key entry");
    table.store(0, 6, 17, koi::TranspositionBound::exact, *move);
    const auto zero_key_entry = table.probe(0);
    require(zero_key_entry.has_value() && zero_key_entry->score == 17,
            "a stored zero key must probe successfully");
    table.clear();
    require(!table.probe(0).has_value(), "a cleared table must not report a zero-key entry");

    table.new_generation();
    table.store(0x0123456789abcdefULL, 7, 42, koi::TranspositionBound::exact, *move);
    const auto entry = table.probe(0x0123456789abcdefULL);
    require(entry.has_value(), "stored key must probe successfully");
    require(entry->depth == 7 && entry->score == 42 && entry->bound == koi::TranspositionBound::exact,
            "probe must retain depth, score, and bound");
    require(entry->best_move == *move, "probe must retain the best move");

    table.clear();
    require(!table.probe(0x0123456789abcdefULL).has_value(), "clear must remove a stored entry");
    table.set_size_mb(1);
    require(table.size_mb() == 1, "hash configuration must accept the lower 1 MB bound");
}

void test_transposition_table_preserves_mate_distance_across_plies() {
    koi::TranspositionTable table;
    const auto move = koi::Move::parse_uci("e2e4");
    require(move.has_value(), "test move must parse");

    table.store(0x1111ULL, 6, 99'993, koi::TranspositionBound::exact, *move, 7);
    const auto winning_mate = table.probe(0x1111ULL, 3);
    require(winning_mate.has_value() && winning_mate->score == 99'997,
            "a winning mate score must be adjusted to the probing ply");

    table.store(0x2222ULL, 6, -99'993, koi::TranspositionBound::exact, *move, 7);
    const auto losing_mate = table.probe(0x2222ULL, 3);
    require(losing_mate.has_value() && losing_mate->score == -99'997,
            "a losing mate score must be adjusted to the probing ply");
}

void test_transposition_table_survives_concurrent_probe_store_and_maintenance() {
    koi::TranspositionTable table(1);
    const auto move = koi::Move::parse_uci("e2e4");
    require(move.has_value(), "test move must parse");
    std::atomic_bool invalid_entry = false;
    std::vector<std::thread> workers;
    for (std::uint64_t worker = 0; worker < 6; ++worker) {
        workers.emplace_back([&table, &invalid_entry, move, worker] {
            for (std::uint64_t iteration = 0; iteration < 2'000; ++iteration) {
                const std::uint64_t key = (worker + 1) * 0x100000001b3ULL + iteration;
                table.store(key, static_cast<int>(iteration % 8), static_cast<int>(worker),
                            koi::TranspositionBound::exact, *move);
                if (const auto entry = table.probe(key); entry.has_value() && entry->key != key) {
                    invalid_entry.store(true, std::memory_order_relaxed);
                }
            }
        });
    }
    std::thread maintenance([&table] {
        for (int iteration = 0; iteration < 100; ++iteration) {
            table.new_generation();
            table.clear();
        }
    });

    for (std::thread& worker : workers) {
        worker.join();
    }
    maintenance.join();
    require(!invalid_entry.load(std::memory_order_relaxed),
            "concurrent TT access must never return an entry for a different key");
}

void test_transposition_table_survives_concurrent_probe_store_and_resize() {
    koi::TranspositionTable table(1);
    const auto move = koi::Move::parse_uci("e2e4");
    require(move.has_value(), "test move must parse");
    std::atomic_bool invalid_entry = false;
    std::vector<std::thread> workers;
    for (std::uint64_t worker = 0; worker < 4; ++worker) {
        workers.emplace_back([&table, &invalid_entry, move, worker] {
            for (std::uint64_t iteration = 0; iteration < 2'000; ++iteration) {
                const std::uint64_t key = (worker + 7) * 0x100000001b3ULL + iteration;
                table.store(key, static_cast<int>(iteration % 8), static_cast<int>(worker),
                            koi::TranspositionBound::lower, *move);
                if (const auto entry = table.probe(key); entry.has_value() && entry->key != key) {
                    invalid_entry.store(true, std::memory_order_relaxed);
                }
            }
        });
    }
    for (int iteration = 0; iteration < 40; ++iteration) {
        const std::size_t megabytes = iteration % 2 == 0 ? 1 : 2;
        table.set_size_mb(megabytes);
        require(table.size_mb() == megabytes, "serialized resize must publish the requested table size");
    }
    for (std::thread& worker : workers) {
        worker.join();
    }
    require(!invalid_entry.load(std::memory_order_relaxed),
            "concurrent resize must not expose a transposition entry for another key");
}

void test_transposition_table_survives_mixed_concurrent_maintenance() {
    koi::TranspositionTable table(1);
    const auto move = koi::Move::parse_uci("e2e4");
    require(move.has_value(), "test move must parse");
    std::atomic_bool invalid_entry = false;
    std::atomic_bool invalid_size = false;
    std::vector<std::thread> workers;
    for (std::uint64_t worker = 0; worker < 6; ++worker) {
        workers.emplace_back([&table, &invalid_entry, move, worker] {
            for (std::uint64_t iteration = 0; iteration < 2'000; ++iteration) {
                const std::uint64_t key = (worker + 13) * 0x100000001b3ULL + iteration;
                table.store(key, static_cast<int>(iteration % 8), static_cast<int>(worker),
                            koi::TranspositionBound::exact, *move);
                if (const auto entry = table.probe(key); entry.has_value() && entry->key != key) {
                    invalid_entry.store(true, std::memory_order_relaxed);
                }
            }
        });
    }
    std::thread maintenance([&] {
        for (int iteration = 0; iteration < 80; ++iteration) {
            table.new_generation();
            table.clear();
            const std::size_t megabytes = iteration % 2 == 0 ? 1 : 2;
            table.set_size_mb(megabytes);
            if (table.size_mb() != megabytes) {
                invalid_size.store(true, std::memory_order_relaxed);
            }
        }
    });
    for (std::thread& worker : workers) {
        worker.join();
    }
    maintenance.join();
    require(!invalid_entry.load(std::memory_order_relaxed),
            "mixed TT maintenance must never expose an entry for another key");
    require(!invalid_size.load(std::memory_order_relaxed),
            "mixed TT maintenance must publish every requested resize");
}

struct TestCase {
    std::string_view name;
    void (*run)();
};

} // namespace

int main() {
    const std::vector<TestCase> tests{
        {"classical evaluator", test_evaluator_returns_material_and_pst_from_requested_perspective},
        {"classical evaluator breakdown", test_evaluator_breakdown_scores_structure_activity_and_king_safety},
        {"evaluator perspective symmetry", test_evaluator_breakdown_is_perspective_symmetric},
        {"evaluator endgame and mobility", test_evaluator_scores_backward_pawns_piece_mobility_and_dead_material},
        {"evaluator passed pawn endgame scaling", test_evaluator_increases_advanced_passed_pawn_value_in_the_endgame},
        {"evaluator passed pawn blockade", test_evaluator_penalizes_a_directly_blockaded_passed_pawn},
        {"evaluator mirrored terms and black blockade", test_evaluator_mirrors_terms_and_black_passed_pawn_blockades},
        {"evaluator color symmetric endgame passer", test_evaluator_endgame_passer_scaling_is_color_symmetric},
        {"time manager", test_time_manager_applies_move_time_and_clock_limits},
        {"speed budgets", test_speed_scales_only_time_based_search_budgets},
        {"search options", test_search_options_include_thread_and_speed_controls},
        {"root filtering legal move", test_root_filtering_keeps_only_requested_legal_move},
        {"root filtering illegal move", test_root_filtering_ignores_syntactically_valid_illegal_move},
        {"deterministic multipv", test_deterministic_multipv_reports_sorted_distinct_legal_lines},
        {"threaded root search", test_threaded_search_uses_multiple_root_workers_and_matches_reference_result},
        {"classical threaded parity", test_classical_threaded_search_matches_reference_result},
        {"stable root ties", test_equal_root_scores_keep_the_earliest_ordered_move},
        {"threaded multipv ordered root ties", test_threaded_multipv_equal_scores_use_stable_ordered_root_tie_breaking},
        {"threaded multipv final-depth parity", test_threaded_multipv_matches_single_thread_at_final_depth},
        {"threaded multipv warmed hash", test_threaded_multipv_is_stable_after_warming_the_shared_hash},
        {"threaded global nodes", test_threaded_node_limit_is_global_and_never_exceeded},
        {"threaded node parity", test_threaded_and_reference_node_limits_have_matching_accounting},
        {"search info accounting", test_search_info_nodes_reports_all_visited_nodes},
        {"threaded cancellation", test_threaded_infinite_search_cancels_and_completes_once},
        {"evaluator cross-handle safety", test_non_concurrent_evaluators_are_serialized_across_simultaneous_handles},
        {"terminal search", test_terminal_roots_return_mate_or_stalemate_scores},
        {"deterministic legal search", test_fixed_depth_search_is_deterministic_and_legal},
        {"fixed-depth tactical reference", test_fixed_depth_tactical_reference_output_is_preserved},
        {"tactical search statistics", test_search_reports_tactical_search_statistics},
        {"check extensions", test_search_extends_checked_positions},
        {"forced quiet evasion", test_search_keeps_a_forced_quiet_evasion},
        {"mate in one distance", test_search_reports_mate_in_one_distance},
        {"pawn-only zugzwang null safety", test_pawn_only_zugzwang_search_skips_null_pruning},
        {"late quiet move reductions", test_search_reduces_late_quiet_moves_without_losing_root_legality},
        {"checked quiescence cap", test_quiescence_keeps_searching_checked_evasions_past_normal_cap},
        {"bounded quiescence checks", test_quiescence_keeps_bounded_checking_continuations},
        {"aspiration windows", test_iterative_deepening_uses_aspiration_windows},
        {"infinite search lifecycle", test_infinite_search_runs_until_stopped_and_completes_once},
        {"ponder search lifecycle", test_ponder_search_runs_until_stopped_and_completes_once},
        {"ponder terminal lifecycle", test_ponder_terminal_and_empty_roots_wait_for_stop},
        {"ponder time and node limits", test_ponder_ignores_time_but_honors_node_limits},
        {"service hash persistence", test_service_hash_configuration_survives_default_start_and_non_default_override},
        {"hash bounds and clear", test_hash_configuration_clamps_to_uci_bounds_and_clear_discards_warmed_entries},
        {"transposition table", test_transposition_table_stores_probes_and_clears_entries},
        {"transposition table mate normalization", test_transposition_table_preserves_mate_distance_across_plies},
        {"transposition table concurrency", test_transposition_table_survives_concurrent_probe_store_and_maintenance},
        {"transposition table resize concurrency", test_transposition_table_survives_concurrent_probe_store_and_resize},
        {"transposition table mixed maintenance", test_transposition_table_survives_mixed_concurrent_maintenance},
    };

    const char* filter = std::getenv("KOI_TEST_FILTER");
    for (const TestCase& test : tests) {
        if (filter != nullptr && std::string_view(test.name).find(filter) == std::string_view::npos) {
            continue;
        }
        try {
            test.run();
            std::cout << "PASS " << test.name << '\n';
        } catch (const std::exception& error) {
            std::cerr << "FAIL " << test.name << ": " << error.what() << '\n';
            return 1;
        }
    }

    return 0;
}
