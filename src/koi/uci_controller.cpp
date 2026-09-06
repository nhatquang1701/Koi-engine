#include "koi/uci_controller.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
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
constexpr std::uint64_t kMinimumSlowMoverPercent = 10;
constexpr std::uint64_t kMaximumSlowMoverPercent = 1'000;
constexpr std::uint64_t kMaximumMoveOverheadMs = 5'000;
constexpr std::uint64_t kMinimumElo = 1'320;
constexpr std::uint64_t kMaximumElo = 3'190;
constexpr std::uint64_t kMinimumMultiPv = 1;
constexpr std::uint64_t kMaximumMultiPv = 16;
constexpr std::uint64_t kMaximumBookDepth = 40;
constexpr std::uint64_t kMaximumBookSafetyDepth = 3;
constexpr std::uint64_t kMinimumSyzygyProbeDepth = 1;
constexpr std::uint64_t kMaximumSyzygyProbeDepth = 100;
constexpr std::uint64_t kMaximumSyzygyProbeLimit = 5;
constexpr std::uintmax_t kDebugRotationBytes = 8U * 1024U * 1024U;
constexpr int kWdlScoreLimit = 1'000;

std::vector<std::string> remaining_tokens(std::istream& command) {
    std::vector<std::string> tokens;
    for (std::string token; command >> token;) {
        tokens.push_back(std::move(token));
    }
    return tokens;
}

std::string join_tokens(const std::vector<std::string>& tokens, std::size_t first,
                        std::size_t last) {
    std::string result;
    for (std::size_t index = first; index < last; ++index) {
        if (!result.empty()) {
            result += ' ';
        }
        result += tokens[index];
    }
    return result;
}

struct ParsedOption {
    std::string name;
    std::string value;
};

bool equals_ignore_case(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        const auto left_character = static_cast<unsigned char>(left[index]);
        const auto right_character = static_cast<unsigned char>(right[index]);
        if (std::tolower(left_character) != std::tolower(right_character)) {
            return false;
        }
    }
    return true;
}

std::optional<ParsedOption> parse_setoption(const std::vector<std::string>& tokens) {
    if (tokens.size() < 2 || tokens[0] != "name") {
        return std::nullopt;
    }
    const auto value = std::find(tokens.begin() + 1, tokens.end(), "value");
    const std::size_t value_index = static_cast<std::size_t>(value - tokens.begin());
    if (value == tokens.end() || value_index == 1) {
        return ParsedOption{join_tokens(tokens, 1, tokens.size()), {}};
    }
    return ParsedOption{join_tokens(tokens, 1, value_index),
                        join_tokens(tokens, value_index + 1, tokens.size())};
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

std::uint32_t root_ply(const GameState& state) {
    const std::uint32_t completed_fullmoves = static_cast<std::uint32_t>(state.fullmove_number() - 1);
    return completed_fullmoves * 2 + (state.side_to_move() == Color::black ? 1U : 0U);
}

struct Wdl {
    int win = 0;
    int draw = 0;
    int loss = 0;
};

Wdl score_to_wdl(const SearchInfo& info) noexcept {
    if (info.mate.has_value()) {
        return *info.mate > 0 ? Wdl{1'000, 0, 0} : Wdl{0, 0, 1'000};
    }
    const int score = std::clamp(info.score_cp, -kWdlScoreLimit, kWdlScoreLimit);
    const int draw = std::max(0, 300 - std::abs(score) / 4);
    const int decisive = 1'000 - draw;
    const int win = decisive * (score + kWdlScoreLimit) / (2 * kWdlScoreLimit);
    return Wdl{win, draw, decisive - win};
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
                             std::ostream& diagnostics, SearchService search_service,
                             std::filesystem::path executable_directory)
    : input_(input), output_(output), diagnostics_(diagnostics),
      executable_directory_(std::move(executable_directory)),
      opening_book_(executable_directory_),
      search_service_(std::move(search_service)) {}

UciController::~UciController() {
    stop_and_suppress_active_search();
}

int UciController::run() {
    for (std::string line; std::getline(input_, line);) {
        debug_event("command " + line);
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
    const std::optional<ParsedOption> parsed = parse_setoption(tokens);
    if (!parsed.has_value()) {
        return;
    }
    const std::string& name = parsed->name;
    const std::string& value = parsed->value;

    if (equals_ignore_case(name, "RandomSeed")) {
        std::uint32_t seed = 0;
        if (parse_random_seed(value, seed)) {
            stop_and_suppress_active_search();
            chooser_.set_seed(seed);
            random_seed_ = seed;
        }
        return;
    }

    if (equals_ignore_case(name, "Hash")) {
        std::uint64_t megabytes = 0;
        if (parse_uint64(value, megabytes) && megabytes >= kMinimumHashMegabytes &&
            megabytes <= kMaximumHashMegabytes) {
            stop_and_suppress_active_search();
            search_service_.set_hash_size_mb(static_cast<std::size_t>(megabytes));
        }
        return;
    }

    if (equals_ignore_case(name, "Threads")) {
        std::uint64_t threads = 0;
        if (parse_uint64(value, threads) && threads >= 1 && threads <= maximum_search_threads()) {
            stop_and_suppress_active_search();
            threads_ = static_cast<std::size_t>(threads);
        }
        return;
    }

    if (equals_ignore_case(name, "Speed")) {
        std::uint64_t speed = 0;
        if (parse_uint64(value, speed) && speed >= kMinimumSpeedPercent &&
            speed <= kMaximumSpeedPercent) {
            stop_and_suppress_active_search();
            speed_percent_ = static_cast<std::uint8_t>(speed);
        }
        return;
    }

    if (equals_ignore_case(name, "UCI_AnalyseMode")) {
        bool analyse_mode = false;
        if (parse_boolean(value, analyse_mode) && analyse_mode_ != analyse_mode) {
            stop_and_suppress_active_search();
            analyse_mode_ = analyse_mode;
        }
        return;
    }

    if (equals_ignore_case(name, "MultiPV")) {
        std::uint64_t multi_pv = 0;
        if (parse_uint64(value, multi_pv) && multi_pv >= kMinimumMultiPv &&
            multi_pv <= kMaximumMultiPv) {
            stop_and_suppress_active_search();
            multi_pv_ = static_cast<std::size_t>(multi_pv);
        }
        return;
    }

    if (equals_ignore_case(name, "Ponder")) {
        bool ponder_enabled = false;
        if (parse_boolean(value, ponder_enabled)) {
            stop_and_suppress_active_search();
            ponder_enabled_ = ponder_enabled;
        }
        return;
    }

    if (equals_ignore_case(name, "OwnBook")) {
        bool own_book = false;
        if (parse_boolean(value, own_book)) {
            stop_and_suppress_active_search();
            own_book_ = own_book;
        }
        return;
    }

    if (equals_ignore_case(name, "BookRandom")) {
        bool book_random = false;
        if (parse_boolean(value, book_random)) {
            stop_and_suppress_active_search();
            book_random_ = book_random;
        }
        return;
    }

    if (equals_ignore_case(name, "BookSafety")) {
        bool book_safety = false;
        if (parse_boolean(value, book_safety)) {
            stop_and_suppress_active_search();
            book_safety_ = book_safety;
        }
        return;
    }

    if (equals_ignore_case(name, "BookSafetyDepth")) {
        std::uint64_t book_safety_depth = 0;
        if (parse_uint64(value, book_safety_depth) && book_safety_depth <= kMaximumBookSafetyDepth) {
            stop_and_suppress_active_search();
            book_safety_depth_ = static_cast<std::uint8_t>(book_safety_depth);
        }
        return;
    }

    if (equals_ignore_case(name, "BookFile")) {
        if (!value.empty()) {
            stop_and_suppress_active_search();
            opening_book_.set_file(value);
        }
        return;
    }

    if (equals_ignore_case(name, "BookDepth")) {
        std::uint64_t book_depth = 0;
        if (parse_uint64(value, book_depth) && book_depth <= kMaximumBookDepth) {
            stop_and_suppress_active_search();
            book_depth_ = static_cast<std::uint8_t>(book_depth);
        }
        return;
    }

    if (equals_ignore_case(name, "Clear Hash")) {
        stop_and_suppress_active_search();
        search_service_.clear_hash();
    }

    if (equals_ignore_case(name, "UCI_ShowWDL")) {
        bool show_wdl = false;
        if (parse_boolean(value, show_wdl)) {
            stop_and_suppress_active_search();
            show_wdl_ = show_wdl;
        }
        return;
    }

    if (equals_ignore_case(name, "Move Overhead")) {
        std::uint64_t overhead = 0;
        if (parse_uint64(value, overhead) && overhead <= kMaximumMoveOverheadMs) {
            stop_and_suppress_active_search();
            move_overhead_ms_ = static_cast<std::uint32_t>(overhead);
        }
        return;
    }

    if (equals_ignore_case(name, "Slow Mover")) {
        std::uint64_t slow_mover = 0;
        if (parse_uint64(value, slow_mover) && slow_mover >= kMinimumSlowMoverPercent &&
            slow_mover <= kMaximumSlowMoverPercent) {
            stop_and_suppress_active_search();
            slow_mover_percent_ = static_cast<std::uint32_t>(slow_mover);
        }
        return;
    }

    if (equals_ignore_case(name, "UCI_LimitStrength")) {
        bool limit_strength = false;
        if (parse_boolean(value, limit_strength)) {
            stop_and_suppress_active_search();
            limit_strength_ = limit_strength;
        }
        return;
    }

    if (equals_ignore_case(name, "UCI_Elo")) {
        std::uint64_t elo = 0;
        if (parse_uint64(value, elo) && elo >= kMinimumElo && elo <= kMaximumElo) {
            stop_and_suppress_active_search();
            elo_ = static_cast<std::uint32_t>(elo);
        }
        return;
    }

    if (equals_ignore_case(name, "StrengthMode")) {
        bool strength_mode = false;
        if (parse_boolean(value, strength_mode) && strength_mode_ != strength_mode) {
            stop_and_suppress_active_search();
            strength_mode_ = strength_mode;
        }
        return;
    }

    if (equals_ignore_case(name, "SyzygyPath")) {
        stop_and_suppress_active_search();
        syzygy_path_ = value;
        rebuild_syzygy();
        return;
    }

    if (equals_ignore_case(name, "SyzygyProbeDepth")) {
        std::uint64_t depth = 0;
        if (parse_uint64(value, depth) && depth >= kMinimumSyzygyProbeDepth &&
            depth <= kMaximumSyzygyProbeDepth) {
            stop_and_suppress_active_search();
            syzygy_probe_depth_ = static_cast<std::uint8_t>(depth);
            rebuild_syzygy();
        }
        return;
    }

    if (equals_ignore_case(name, "SyzygyProbeLimit")) {
        std::uint64_t limit = 0;
        if (parse_uint64(value, limit) && limit <= kMaximumSyzygyProbeLimit) {
            stop_and_suppress_active_search();
            syzygy_probe_limit_ = static_cast<std::uint8_t>(limit);
            rebuild_syzygy();
        }
        return;
    }

    if (equals_ignore_case(name, "Syzygy50MoveRule")) {
        bool fifty_move_rule = false;
        if (parse_boolean(value, fifty_move_rule)) {
            stop_and_suppress_active_search();
            syzygy_50_move_rule_ = fifty_move_rule;
            rebuild_syzygy();
        }
        return;
    }

    if (equals_ignore_case(name, "Debug")) {
        bool debug = false;
        if (parse_boolean(value, debug)) {
            stop_and_suppress_active_search();
            debug_enabled_ = debug;
            configure_debug_file();
        }
        return;
    }

    if (equals_ignore_case(name, "DebugFile")) {
        stop_and_suppress_active_search();
        debug_file_path_ = value;
        if (debug_enabled_) {
            configure_debug_file();
        }
    }
}

void UciController::handle_go(std::istream& command) {
    std::string arguments;
    std::getline(command, arguments);
    SearchLimits limits = uci::parse_go_limits(arguments);

    const bool has_explicit_limit = limits.depth.has_value() || limits.nodes.has_value() ||
        limits.movetime.has_value() || limits.infinite || limits.ponder;
    const bool has_side_to_move_clock = position_.side_to_move() == Color::white ?
        limits.white_clock.has_value() : limits.black_clock.has_value();
    if (!has_explicit_limit && !has_side_to_move_clock) {
        // A malformed or asymmetric clock command must still complete. The
        // TimeManager intentionally leaves a clock for the other side unset;
        // the controller supplies the same bounded fallback as bare `go`.
        limits.movetime = std::chrono::milliseconds{250};
    }
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
    if (!root.has_value() || !limits.has_value()) {
        return;
    }
    if (expected_move.has_value() && root->is_legal(*expected_move)) {
        (void)root->make_move(*expected_move);
    }

    limits->ponder = false;
    const bool has_side_to_move_clock = root->side_to_move() == Color::white ?
        limits->white_clock.has_value() : limits->black_clock.has_value();
    const bool has_normal_limit = limits->depth.has_value() || limits->nodes.has_value() ||
        limits->movetime.has_value() || has_side_to_move_clock || limits->infinite;
    if (!has_normal_limit) {
        limits->movetime = std::chrono::milliseconds{250};
    }
    limits->search_moves_specified = false;
    limits->search_moves.clear();
    start_search(std::move(*root), std::move(*limits), true);
}

void UciController::start_search(GameState root, SearchLimits limits, bool skip_book) {
    const std::uint64_t generation = begin_generation();
    debug_event("search start generation " + std::to_string(generation));
    const bool book_eligible = !skip_book && !analyse_mode_ && multi_pv_ == 1 && !limits.infinite &&
        !limits.ponder && !limits.search_moves_specified;
    if (book_eligible) {
        const std::uint32_t ply = root_ply(root);
        const std::optional<BookChoice> choice =
            opening_book_.choose(root, ply, own_book_, book_depth_, random_seed_, book_random_,
                                 book_safety_, book_safety_depth_);
        if (choice.has_value()) {
            write_book_completion(generation, *choice, ply);
            return;
        }
    }
    const GameState search_root = root;
    const bool is_ponder_search = limits.ponder;
    const bool allow_ponder_move = !limits.search_moves_specified;

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
    sink.on_complete = [this, generation, allow_ponder_move](const SearchResult& result) {
        debug_event("search complete generation " + std::to_string(generation));
        SearchResult completed = result;
        {
            std::lock_guard lock(output_mutex_);
            if (generation == generation_ && allow_ponder_move &&
                result.best_move == principal_variation_best_move_) {
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
    options.show_wdl = show_wdl_;
    options.move_overhead_ms = move_overhead_ms_;
    options.slow_mover_percent = slow_mover_percent_;
    options.limit_strength = limit_strength_;
    options.elo = elo_;
    options.strength_mode = strength_mode_;
    options.syzygy = syzygy_;
    active_search_.emplace(search_service_.start(std::move(root), std::move(limits), std::move(sink), options));
}

void UciController::stop_active_search() {
    if (!active_search_.has_value()) {
        clear_ponder_state();
        return;
    }

    debug_event("search cancellation requested");
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
    debug_event("search cancellation requested with generation suppression");
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
               "option name Hash type spin default 512 min 1 max 4096\n"
               "option name Threads type spin default 1 min 1 max " << maximum_search_threads() << "\n"
               "option name Speed type spin default 100 min 1 max 100\n"
               "option name UCI_AnalyseMode type check default false\n"
               "option name MultiPV type spin default 1 min 1 max 16\n"
               "option name Ponder type check default false\n"
               "option name OwnBook type check default true\n"
               "option name BookFile type string default book.bin\n"
               "option name BookDepth type spin default 16 min 0 max 40\n"
               "option name BookRandom type check default false\n"
               "option name BookSafety type check default true\n"
               "option name BookSafetyDepth type spin default 2 min 0 max 3\n"
               "option name Clear Hash type button\n"
               "option name UCI_ShowWDL type check default false\n"
               "option name Move Overhead type spin default 10 min 0 max 5000\n"
               "option name Slow Mover type spin default 100 min 10 max 1000\n"
               "option name UCI_LimitStrength type check default false\n"
               "option name UCI_Elo type spin default 1320 min 1320 max 3190\n"
               "option name StrengthMode type check default false\n"
               "option name SyzygyPath type string default \n"
               "option name SyzygyProbeDepth type spin default 1 min 1 max 100\n"
               "option name SyzygyProbeLimit type spin default 5 min 0 max 5\n"
               "option name Syzygy50MoveRule type check default true\n"
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
    if (show_wdl_) {
        const Wdl wdl = score_to_wdl(info);
        output_ << " wdl " << wdl.win << ' ' << wdl.draw << ' ' << wdl.loss;
    }
    output_ << " nodes " << info.nodes << " nps " << info.nps
            << " time " << info.elapsed.count() << " pv";
    for (const Move& move : info.pv) {
        output_ << ' ' << move.uci();
    }
    if (info.tbhits != 0) {
        output_ << " tbhits " << info.tbhits;
    }
    output_ << '\n' << std::flush;
}

void UciController::rebuild_syzygy() {
    syzygy_.reset();
    syzygy_ = std::make_shared<SyzygyTablebase>(
        syzygy_path_, syzygy_probe_limit_, syzygy_probe_depth_, syzygy_50_move_rule_);
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

void UciController::write_book_completion(std::uint64_t generation, const BookChoice& choice,
                                           std::uint32_t ply) {
    std::lock_guard lock(output_mutex_);
    if (generation != generation_) {
        return;
    }

    output_ << "info string book move " << choice.move.uci() << " depth " << ply << '\n'
             << "bestmove " << choice.move.uci() << '\n' << std::flush;
}

std::filesystem::path UciController::debug_path() const {
    const std::filesystem::path base = executable_directory_.empty() ?
        std::filesystem::current_path() : executable_directory_;
    const std::filesystem::path requested = debug_file_path_.empty() ?
        std::filesystem::path{"koi-debug.log"} : debug_file_path_;
    return requested.is_absolute() ? requested : base / requested;
}

void UciController::rotate_debug_file_if_needed(std::size_t incoming_bytes) {
    const std::filesystem::path path = debug_path();
    std::error_code error;
    const std::uintmax_t current_size = std::filesystem::exists(path, error) && !error ?
        std::filesystem::file_size(path, error) : 0;
    if (error || current_size < kDebugRotationBytes &&
        incoming_bytes <= kDebugRotationBytes - current_size) {
        return;
    }

    if (debug_file_.is_open()) {
        debug_file_.close();
    }
    for (int backup = 2; backup >= 1; --backup) {
        const std::filesystem::path source = path.string() + "." + std::to_string(backup);
        const std::filesystem::path target = path.string() + "." + std::to_string(backup + 1);
        std::filesystem::remove(target, error);
        error.clear();
        if (std::filesystem::exists(source, error) && !error) {
            std::filesystem::rename(source, target, error);
            error.clear();
        }
    }
    std::filesystem::rename(path, path.string() + ".1", error);
}

void UciController::configure_debug_file() {
    std::lock_guard lock(debug_mutex_);
    if (debug_file_.is_open()) {
        debug_file_.close();
    }
    if (!debug_enabled_) {
        return;
    }

    const std::filesystem::path path = debug_path();
    std::error_code error;
    const bool oversized = std::filesystem::exists(path, error) && !error &&
        std::filesystem::file_size(path, error) >= kDebugRotationBytes;
    if (oversized && !error) {
        rotate_debug_file_if_needed(0);
    }
    debug_file_.open(path, std::ios::binary | std::ios::app);
}

void UciController::debug_event(std::string message) noexcept {
    try {
        std::lock_guard lock(debug_mutex_);
        if (!debug_enabled_) {
            return;
        }
        if (!debug_file_.is_open()) {
            const std::filesystem::path path = debug_path();
            debug_file_.open(path, std::ios::binary | std::ios::app);
        }
        if (!debug_file_.is_open()) {
            return;
        }
        message.push_back('\n');
        rotate_debug_file_if_needed(message.size());
        if (!debug_file_.is_open()) {
            debug_file_.open(debug_path(), std::ios::binary | std::ios::app);
        }
        if (!debug_file_.is_open()) {
            return;
        }
        debug_file_.write(message.data(), static_cast<std::streamsize>(message.size()));
        debug_file_.flush();
    } catch (...) {
        // Developer diagnostics are strictly best effort and never affect UCI.
    }
}

void UciController::write_position_error(const char* message) {
    {
        std::lock_guard lock(output_mutex_);
        output_ << "info string " << message << '\n' << std::flush;
    }
    diagnostics_ << "UCI position error: " << message << '\n';
}

} // namespace koi
