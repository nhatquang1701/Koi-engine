#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iosfwd>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#include "koi/game_state.hpp"
#include "koi/completion_gate.hpp"
#include "koi/move_chooser.hpp"
#include "koi/opening_book.hpp"
#include "koi/search_service.hpp"
#include "koi/syzygy_tablebase.hpp"

namespace koi {

namespace uci {

// Parses the portion of a UCI "go" command following the command name.
[[nodiscard]] SearchLimits parse_go_limits(std::string_view arguments);

} // namespace uci

class UciController {
public:
    UciController(std::istream& input, std::ostream& output, std::ostream& diagnostics);
    UciController(std::istream& input, std::ostream& output, std::ostream& diagnostics,
                  SearchService search_service,
                  std::filesystem::path executable_directory = {});
    ~UciController();

    UciController(const UciController&) = delete;
    UciController& operator=(const UciController&) = delete;

    int run();

private:
    enum class ControllerState : std::uint8_t {
        Idle,
        Searching,
        Pondering,
        Stopping,
        ShuttingDown,
    };

    void handle_position(std::istream& command, std::string_view command_text);
    void handle_setoption(std::istream& command);
    void handle_go(std::istream& command);
    void handle_ponderhit();
    void start_search(GameState root, SearchLimits limits, bool skip_book = false);
    void stop_active_search();
    void stop_and_suppress_active_search();
    void clear_ponder_state();
    [[nodiscard]] std::uint64_t begin_generation();
    void write_handshake();
    void write_readyok();
    void write_search_info(std::uint64_t generation, const SearchInfo& info);
    void write_search_completion(std::uint64_t generation, const SearchResult& result);
    void write_book_completion(std::uint64_t generation, const GameState& root,
                               const SearchLimits& limits, const BookChoice& choice,
                               std::uint32_t root_ply,
                               const std::shared_ptr<CompletionOnce>& completion_once);
    void write_position_error(const char* message);
    void write_perft_results(int depth);
    void debug_event(std::string message) noexcept;
    void debug_json_event(std::string event, std::string fields) noexcept;
    void configure_debug_file();
    void rebuild_syzygy();
    [[nodiscard]] std::filesystem::path debug_path() const;
    void rotate_debug_file_if_needed(std::size_t incoming_bytes);

    std::istream& input_;
    std::ostream& output_;
    std::ostream& diagnostics_;
    std::mutex output_mutex_;
    std::mutex debug_mutex_;
    std::filesystem::path executable_directory_;
    std::filesystem::path debug_file_path_;
    std::ofstream debug_file_;
    GameState position_;
    RandomMoveChooser chooser_{0};
    OpeningBook opening_book_;
    SearchService search_service_;
    std::optional<SearchHandle> active_search_;
    std::optional<GameState> ponder_origin_;
    std::optional<GameState> ponder_root_;
    std::optional<SearchLimits> ponder_limits_;
    std::optional<Move> principal_variation_best_move_;
    std::optional<Move> principal_variation_ponder_move_;
    std::optional<Move> ponder_predicted_move_;
    std::optional<Move> ponder_expected_move_;
    bool active_ponder_ = false;
    std::size_t hash_mb_ = 512;
    std::size_t threads_ = 1;
    std::uint8_t speed_percent_ = 100;
    bool show_wdl_ = false;
    std::uint32_t move_overhead_ms_ = 30;
    std::uint32_t slow_mover_percent_ = 100;
    bool limit_strength_ = false;
    std::uint32_t elo_ = 1320;
    bool strength_mode_ = false;
    bool debug_enabled_ = false;
    bool analyse_mode_ = false;
    std::size_t multi_pv_ = 1;
    bool ponder_enabled_ = false;
    bool own_book_ = true;
    bool book_random_ = false;
    bool book_safety_ = true;
    std::uint8_t book_safety_depth_ = 2;
    std::uint8_t book_depth_ = 16;
    std::uint32_t random_seed_ = 0;
    std::filesystem::path syzygy_path_;
    std::filesystem::path eval_file_;
    std::uint8_t syzygy_probe_depth_ = 1;
    std::uint8_t syzygy_probe_limit_ = 7;
    std::uint8_t syzygy_interior_depth_ = 0;
    bool syzygy_50_move_rule_ = true;
    std::shared_ptr<const SyzygyTablebase> syzygy_ =
        std::make_shared<SyzygyTablebase>();
    CompletionGate completion_gate_;
    // Protocol staleness counter: begin_generation() bumps this for every new
    // search request and write_search_completion() drops replies whose
    // captured generation no longer matches.  This is not the transposition
    // table's replacement epoch (advanced by TranspositionTable::new_generation)
    // and not a SearchRequestIdentity; see search_types.hpp for the three
    // distinct "generation" concepts.
    std::uint64_t generation_ = 0;
    std::string last_command_;
    ControllerState state_ = ControllerState::Idle;
};

} // namespace koi
