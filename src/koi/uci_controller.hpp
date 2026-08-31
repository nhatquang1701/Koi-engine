#pragma once

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <mutex>
#include <optional>
#include <string_view>

#include "koi/game_state.hpp"
#include "koi/move_chooser.hpp"
#include "koi/search_service.hpp"

namespace koi {

namespace uci {

// Parses the portion of a UCI "go" command following the command name.
[[nodiscard]] SearchLimits parse_go_limits(std::string_view arguments);

} // namespace uci

class UciController {
public:
    UciController(std::istream& input, std::ostream& output, std::ostream& diagnostics);
    UciController(std::istream& input, std::ostream& output, std::ostream& diagnostics,
                  SearchService search_service);
    ~UciController();

    UciController(const UciController&) = delete;
    UciController& operator=(const UciController&) = delete;

    int run();

private:
    void handle_position(std::istream& command);
    void handle_setoption(std::istream& command);
    void handle_go(std::istream& command);
    void handle_ponderhit();
    void start_search(GameState root, SearchLimits limits);
    void stop_active_search();
    void stop_and_suppress_active_search();
    void clear_ponder_state();
    [[nodiscard]] std::uint64_t begin_generation();
    void write_handshake();
    void write_readyok();
    void write_search_info(std::uint64_t generation, const SearchInfo& info);
    void write_search_completion(std::uint64_t generation, const SearchResult& result);
    void write_position_error(const char* message);

    std::istream& input_;
    std::ostream& output_;
    std::ostream& diagnostics_;
    std::mutex output_mutex_;
    GameState position_;
    RandomMoveChooser chooser_{0};
    SearchService search_service_;
    std::optional<SearchHandle> active_search_;
    std::optional<GameState> ponder_root_;
    std::optional<SearchLimits> ponder_limits_;
    bool active_ponder_ = false;
    std::size_t threads_ = 1;
    std::uint8_t speed_percent_ = 100;
    bool analyse_mode_ = false;
    std::size_t multi_pv_ = 1;
    bool ponder_enabled_ = false;
    std::uint64_t generation_ = 0;
};

} // namespace koi
