#pragma once

#include <iosfwd>

#include "koi/game_state.hpp"
#include "koi/move_chooser.hpp"

namespace koi {

class UciController {
public:
    UciController(std::istream& input, std::ostream& output, std::ostream& diagnostics);

    int run();

private:
    void handle_position(std::istream& command);
    void handle_setoption(std::istream& command);
    void write_bestmove();
    void write_position_error(const char* message);

    std::istream& input_;
    std::ostream& output_;
    std::ostream& diagnostics_;
    GameState position_;
    RandomMoveChooser chooser_{0};
};

} // namespace koi
