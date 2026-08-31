#include "koi/uci_controller.hpp"

#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "koi/classical_evaluator.hpp"

namespace koi {

namespace {

constexpr std::uint64_t kMaximumRandomSeed = 2'147'483'647;
constexpr std::uint64_t kMinimumHashMegabytes = 1;
constexpr std::uint64_t kMaximumHashMegabytes = 4'096;
constexpr std::uint64_t kMinimumSpeedPercent = 1;
constexpr std::uint64_t kMaximumSpeedPercent = 100;
constexpr std::uint64_t kMinimumMultiPv = 1;
constexpr std::uint64_t kMaximumMultiPv = 16;

std::vector<std::string> remaining_tokens(std::istream& command) {
    std::vector<std::string> tokens;
    for (std::string token; command >> token;) {
        tokens.push_back(std::move(token));
    }
    return tokens;
}

bool parse_uint64(const std::string& value, std::uint64_t& parsed) {
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    return error == std::errc{} && end == value.data() + value.size();
}

bool parse_boolean(const std::string& value, bool& parsed) {
    std::string normalized;
    normalized.reserve(value.size());
    for (const unsigned char character : value) {
        normalized.push_back(static_cast<char>(std::tolower(character)));
    }

    if (normalized == "true") {
        parsed = true;
        return true;
    }
    if (normalized == "false") {
        parsed = false;
        return true;
    }
    return false;
}

bool parse_random_seed(const std::string& value, std::uint32_t& seed) {
    std::uint64_t parsed = 0;
    if (!parse_uint64(value, parsed) || parsed > kMaximumRandomSeed) {
        return false;
    }
    seed = static_cast<std::uint32_t>(parsed);
    return true;
}

bool parse_milliseconds(const std::string& value, std::chrono::milliseconds& duration) {
    std::uint64_t parsed = 0;
    using Rep = std::chrono::milliseconds::rep;
    if (!parse_uint64(value, parsed) ||
        parsed > static_cast<std::uint64_t>(std::numeric_limits<Rep>::max())) {
        return false;
    }
    duration = std::chrono::milliseconds{static_cast<Rep>(parsed)};
    return true;
}

} // namespace

namespace uci {

SearchLimits parse_go_limits(std::string_view arguments) {
    std::istringstream command{std::string(arguments)};
    const std::vector<std::string> tokens = remaining_tokens(command);
    SearchLimits limits;
    std::optional<std::chrono::milliseconds> white_time;
    std::optional<std::chrono::milliseconds> black_time;
    std::optional<std::chrono::milliseconds> white_increment;
    std::optional<std::chrono::milliseconds> black_increment;

    for (std::size_t index = 0; index < tokens.size(); ++index) {
        const std::string& token = tokens[index];
        if (token == "infinite") {
            limits.infinite = true;
            continue;
        }
        if (token == "ponder") {
            limits.ponder = true;
            continue;
        }
        if (token == "searchmoves") {
            limits.search_moves_specified = true;
            ++index;
            while (index < tokens.size()) {
                const auto move = Move::parse_uci(tokens[index]);
                if (!move.has_value()) {
                    break;
                }
                limits.search_moves.push_back(*move);
                ++index;
            }
            if (index < tokens.size()) {
                --index;
            }
            continue;
        }
        if (index + 1 >= tokens.size()) {
            continue;
        }

        const std::string& value = tokens[index + 1];
        if (token == "depth") {
            std::uint64_t parsed = 0;
            if (parse_uint64(value, parsed) && parsed > 0 &&
                parsed <= static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
                limits.depth = static_cast<int>(parsed);
            }
            ++index;
        } else if (token == "nodes") {
            std::uint64_t parsed = 0;
            if (parse_uint64(value, parsed)) {
                limits.nodes = parsed;
            }
            ++index;
        } else if (token == "movetime") {
            std::chrono::milliseconds parsed;
            if (parse_milliseconds(value, parsed)) {
                limits.movetime = parsed;
            }
            ++index;
        } else if (token == "wtime") {
            std::chrono::milliseconds parsed;
            if (parse_milliseconds(value, parsed)) {
                white_time = parsed;
            }
            ++index;
        } else if (token == "btime") {
            std::chrono::milliseconds parsed;
            if (parse_milliseconds(value, parsed)) {
                black_time = parsed;
            }
            ++index;
        } else if (token == "winc") {
            std::chrono::milliseconds parsed;
            if (parse_milliseconds(value, parsed)) {
                white_increment = parsed;
            }
            ++index;
        } else if (token == "binc") {
            std::chrono::milliseconds parsed;
            if (parse_milliseconds(value, parsed)) {
                black_increment = parsed;
            }
            ++index;
        } else if (token == "movestogo") {
            std::uint64_t parsed = 0;
            if (parse_uint64(value, parsed) && parsed > 0 &&
                parsed <= static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max())) {
                limits.moves_to_go = static_cast<std::uint32_t>(parsed);
            }
            ++index;
        }
    }

    if (white_time.has_value()) {
        limits.white_clock = ClockLimit{white_time.value_or(std::chrono::milliseconds::zero()),
                                        white_increment.value_or(std::chrono::milliseconds::zero())};
    }
    if (black_time.has_value()) {
        limits.black_clock = ClockLimit{black_time.value_or(std::chrono::milliseconds::zero()),
                                        black_increment.value_or(std::chrono::milliseconds::zero())};
    }

    const bool has_usable_limit = limits.depth.has_value() || limits.nodes.has_value() ||
        limits.movetime.has_value() || limits.white_clock.has_value() ||
        limits.black_clock.has_value() || limits.infinite || limits.ponder;
    if (!has_usable_limit) {
        limits.movetime = std::chrono::milliseconds{250};
    }
    return limits;
}

} // namespace uci

UciController::UciController(std::istream& input, std::ostream& output, std::ostream& diagnostics)
    : UciController(input, output, diagnostics,
                    SearchService{std::make_shared<ClassicalEvaluator>()}) {}

UciController::UciController(std::istream& input, std::ostream& output,
                             std::ostream& diagnostics, SearchService search_service)
    : input_(input), output_(output), diagnostics_(diagnostics),
      search_service_(std::move(search_service)) {}

UciController::~UciController() {
    stop_and_suppress_active_search();
}

int UciController::run() {
    for (std::string line; std::getline(input_, line);) {
        std::istringstream command(line);
        std::string name;
        if (!(command >> name)) {
            continue;
        }

        if (name == "uci") {
            write_handshake();
        } else if (name == "isready") {
            write_readyok();
        } else if (name == "ucinewgame") {
            stop_and_suppress_active_search();
            position_ = GameState::startpos();
        } else if (name == "position") {
            handle_position(command);
        } else if (name == "setoption") {
            handle_setoption(command);
        } else if (name == "go") {
            handle_go(command);
        } else if (name == "ponderhit") {
            handle_ponderhit();
        } else if (name == "stop") {
            stop_active_search();
        } else if (name == "quit") {
            stop_and_suppress_active_search();
            return 0;
        }
    }

    stop_and_suppress_active_search();
    return 0;
}

void UciController::handle_position(std::istream& command) {
    const std::vector<std::string> tokens = remaining_tokens(command);
    if (tokens.empty()) {
        write_position_error("missing position");
        return;
    }

    GameState candidate = GameState::startpos();
    std::size_t next = 0;
    if (tokens[0] == "startpos") {
        next = 1;
    } else if (tokens[0] == "fen") {
        if (tokens.size() < 7) {
            write_position_error("invalid FEN");
            return;
        }

        std::string fen = tokens[1];
        for (std::size_t field = 2; field < 7; ++field) {
            fen += ' ';
            fen += tokens[field];
        }
        const auto parsed = GameState::from_fen(fen);
        if (!parsed) {
            write_position_error("invalid FEN");
            return;
        }
        candidate = *parsed;
        next = 7;
    } else {
        write_position_error("invalid position");
        return;
    }

    if (next < tokens.size()) {
        if (tokens[next] != "moves") {
            write_position_error("invalid position");
            return;
        }
        ++next;
        for (; next < tokens.size(); ++next) {
            const auto move = Move::parse_uci(tokens[next]);
            if (!move || !candidate.make_move(*move)) {
                write_position_error("invalid move");
                return;
            }
        }
    }

    stop_and_suppress_active_search();
    position_ = std::move(candidate);
}

void UciController::handle_setoption(std::istream& command) {
    const std::vector<std::string> tokens = remaining_tokens(command);
    if (tokens.size() == 4 && tokens[0] == "name" && tokens[1] == "RandomSeed" &&
        tokens[2] == "value") {
        std::uint32_t seed = 0;
        if (parse_random_seed(tokens[3], seed)) {
            stop_and_suppress_active_search();
            chooser_.set_seed(seed);
        }
        return;
    }

    if (tokens.size() == 4 && tokens[0] == "name" && tokens[1] == "Hash" &&
        tokens[2] == "value") {
        std::uint64_t megabytes = 0;
        if (parse_uint64(tokens[3], megabytes) && megabytes >= kMinimumHashMegabytes &&
            megabytes <= kMaximumHashMegabytes) {
            stop_and_suppress_active_search();
            search_service_.set_hash_size_mb(static_cast<std::size_t>(megabytes));
        }
        return;
    }

    if (tokens.size() == 4 && tokens[0] == "name" && tokens[1] == "Threads" &&
        tokens[2] == "value") {
        std::uint64_t threads = 0;
        if (parse_uint64(tokens[3], threads) && threads >= 1 && threads <= maximum_search_threads()) {
            stop_and_suppress_active_search();
            threads_ = static_cast<std::size_t>(threads);
        }
        return;
    }

    if (tokens.size() == 4 && tokens[0] == "name" && tokens[1] == "Speed" &&
        tokens[2] == "value") {
        std::uint64_t speed = 0;
        if (parse_uint64(tokens[3], speed) && speed >= kMinimumSpeedPercent &&
            speed <= kMaximumSpeedPercent) {
            stop_and_suppress_active_search();
            speed_percent_ = static_cast<std::uint8_t>(speed);
        }
        return;
    }

    if (tokens.size() == 4 && tokens[0] == "name" && tokens[1] == "UCI_AnalyseMode" &&
        tokens[2] == "value") {
        bool analyse_mode = false;
        if (parse_boolean(tokens[3], analyse_mode) && analyse_mode_ != analyse_mode) {
            stop_and_suppress_active_search();
            analyse_mode_ = analyse_mode;
        }
        return;
    }

    if (tokens.size() == 4 && tokens[0] == "name" && tokens[1] == "MultiPV" &&
        tokens[2] == "value") {
        std::uint64_t multi_pv = 0;
        if (parse_uint64(tokens[3], multi_pv) && multi_pv >= kMinimumMultiPv &&
            multi_pv <= kMaximumMultiPv) {
            stop_and_suppress_active_search();
            multi_pv_ = static_cast<std::size_t>(multi_pv);
        }
        return;
    }

    if (tokens.size() == 4 && tokens[0] == "name" && tokens[1] == "Ponder" &&
        tokens[2] == "value") {
        bool ponder_enabled = false;
        if (parse_boolean(tokens[3], ponder_enabled)) {
            stop_and_suppress_active_search();
            ponder_enabled_ = ponder_enabled;
        }
        return;
    }

    if (tokens.size() == 3 && tokens[0] == "name" && tokens[1] == "Clear" &&
        tokens[2] == "Hash") {
        stop_and_suppress_active_search();
        search_service_.clear_hash();
    }
}

void UciController::handle_go(std::istream& command) {
    std::string arguments;
    std::getline(command, arguments);
    SearchLimits limits = uci::parse_go_limits(arguments);
    stop_and_suppress_active_search();

    if (limits.ponder) {
        ponder_root_ = position_;
        ponder_limits_ = limits;
        active_ponder_ = true;
    }
    start_search(position_, std::move(limits));
}

void UciController::handle_ponderhit() {
    std::optional<GameState> root;
    std::optional<SearchLimits> limits;
    std::optional<Move> expected_move;
    {
        std::lock_guard lock(output_mutex_);
        if (!active_ponder_ || !ponder_root_.has_value() || !ponder_limits_.has_value()) {
            return;
        }
        root = ponder_root_;
        limits = ponder_limits_;
        expected_move = ponder_expected_move_;
    }

    stop_and_suppress_active_search();
    if (!root.has_value() || !limits.has_value() || !expected_move.has_value() ||
        !root->make_move(*expected_move)) {
        return;
    }

    limits->ponder = false;
    const bool has_normal_limit = limits->depth.has_value() || limits->nodes.has_value() ||
        limits->movetime.has_value() || limits->white_clock.has_value() ||
        limits->black_clock.has_value() || limits->infinite;
    if (!has_normal_limit) {
        limits->movetime = std::chrono::milliseconds{250};
    }
    limits->search_moves_specified = false;
    limits->search_moves.clear();
    start_search(std::move(*root), std::move(*limits));
}

void UciController::start_search(GameState root, SearchLimits limits) {
    const std::uint64_t generation = begin_generation();
    const GameState search_root = root;
    const bool is_ponder_search = limits.ponder;

    SearchEventSink sink;
    sink.on_info = [this, generation, search_root, is_ponder_search](const SearchInfo& info) {
        if (info.multipv == 1 && info.pv.size() >= 2) {
            GameState after_best = search_root;
            if (after_best.make_move(info.pv[0]) && after_best.is_legal(info.pv[1])) {
                std::lock_guard lock(output_mutex_);
                if (generation == generation_) {
                    principal_variation_best_move_ = info.pv[0];
                    principal_variation_ponder_move_ = info.pv[1];
                    if (is_ponder_search && !ponder_expected_move_.has_value()) {
                        ponder_root_ = std::move(after_best);
                        ponder_expected_move_ = info.pv[1];
                    }
                }
            }
        }
        write_search_info(generation, info);
    };
    sink.on_complete = [this, generation](const SearchResult& result) {
        SearchResult completed = result;
        {
            std::lock_guard lock(output_mutex_);
            if (generation == generation_ && result.best_move == principal_variation_best_move_) {
                completed.ponder_move = principal_variation_ponder_move_;
            }
        }
        write_search_completion(generation, completed);
    };

    SearchOptions options;
    options.threads = threads_;
    options.speed_percent = speed_percent_;
    options.multi_pv = multi_pv_;
    options.analyse_mode = analyse_mode_;
    active_search_.emplace(search_service_.start(std::move(root), std::move(limits), std::move(sink), options));
}

void UciController::stop_active_search() {
    if (!active_search_.has_value()) {
        clear_ponder_state();
        return;
    }

    active_search_->stop();
    active_search_->wait();
    active_search_.reset();
    clear_ponder_state();
}

void UciController::stop_and_suppress_active_search() {
    if (!active_search_.has_value()) {
        clear_ponder_state();
        return;
    }

    {
        std::lock_guard lock(output_mutex_);
        ++generation_;
    }
    active_search_->stop();
    active_search_->wait();
    active_search_.reset();
    clear_ponder_state();
}

void UciController::clear_ponder_state() {
    ponder_root_.reset();
    ponder_limits_.reset();
    ponder_expected_move_.reset();
    active_ponder_ = false;
}

std::uint64_t UciController::begin_generation() {
    std::lock_guard lock(output_mutex_);
    principal_variation_best_move_.reset();
    principal_variation_ponder_move_.reset();
    ponder_expected_move_.reset();
    return ++generation_;
}

void UciController::write_handshake() {
    std::lock_guard lock(output_mutex_);
    output_ << "id name Koi Engine\n"
               "id author Koi Engine contributors\n"
               "option name RandomSeed type spin default 0 min 0 max 2147483647\n"
               "option name Hash type spin default 16 min 1 max 4096\n"
               "option name Threads type spin default 1 min 1 max " << maximum_search_threads() << "\n"
               "option name Speed type spin default 100 min 1 max 100\n"
               "option name UCI_AnalyseMode type check default false\n"
               "option name MultiPV type spin default 1 min 1 max 16\n"
               "option name Ponder type check default false\n"
               "option name Clear Hash type button\n"
               "uciok\n"
            << std::flush;
}

void UciController::write_readyok() {
    std::lock_guard lock(output_mutex_);
    output_ << "readyok\n" << std::flush;
}

void UciController::write_search_info(std::uint64_t generation, const SearchInfo& info) {
    std::lock_guard lock(output_mutex_);
    if (generation != generation_) {
        return;
    }

    output_ << "info depth " << info.depth << " seldepth " << info.seldepth
            << " multipv " << info.multipv << " score ";
    if (info.mate.has_value()) {
        output_ << "mate " << *info.mate;
    } else {
        output_ << "cp " << info.score_cp;
    }
    output_ << " nodes " << info.nodes << " nps " << info.nps
            << " time " << info.elapsed.count() << " pv";
    for (const Move& move : info.pv) {
        output_ << ' ' << move.uci();
    }
    output_ << '\n' << std::flush;
}

void UciController::write_search_completion(std::uint64_t generation,
                                            const SearchResult& result) {
    std::lock_guard lock(output_mutex_);
    if (generation != generation_) {
        return;
    }

    output_ << "bestmove "
            << (result.best_move.has_value() ? result.best_move->uci() : "0000");
    if (ponder_enabled_ && result.best_move.has_value() && result.ponder_move.has_value()) {
        output_ << " ponder " << result.ponder_move->uci();
    }
    output_ << '\n' << std::flush;
}

void UciController::write_position_error(const char* message) {
    {
        std::lock_guard lock(output_mutex_);
        output_ << "info string " << message << '\n' << std::flush;
    }
    diagnostics_ << "UCI position error: " << message << '\n';
}

} // namespace koi
