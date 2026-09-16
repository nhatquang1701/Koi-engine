#include "koi/uci_controller.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "koi/classical_evaluator.hpp"
#include "koi/nnue.hpp"

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
constexpr std::uint64_t kMaximumSyzygyProbeLimit = 7;
constexpr std::uintmax_t kDebugRotationBytes = 8U * 1024U * 1024U;
constexpr int kWdlScoreLimit = 1'000;
// Bounded fallback for a bare `go` and for malformed or asymmetric clock
// commands. A UCI `go` always has to be answered, so a missing usable limit
// must never leave the engine searching indefinitely.
constexpr std::chrono::milliseconds kBareGoFallback{250};

const char* hash_status_name(HashResizeStatus status) noexcept {
    switch (status) {
    case HashResizeStatus::applied:
        return "applied";
    case HashResizeStatus::reduced:
        return "reduced";
    case HashResizeStatus::unchanged:
        return "unchanged";
    case HashResizeStatus::disabled:
        return "disabled";
    }
    return "unknown";
}

const char* hash_reason_name(HashResizeReason reason) noexcept {
    switch (reason) {
    case HashResizeReason::none:
        return "none";
    case HashResizeReason::request_clamped:
        return "request_clamped";
    case HashResizeReason::physical_memory_cap:
        return "physical_memory_cap";
    case HashResizeReason::commit_cap:
        return "commit_cap";
    case HashResizeReason::allocation_failed:
        return "allocation_failed";
    case HashResizeReason::startup_unavailable:
        return "startup_unavailable";
    }
    return "unknown";
}

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

enum class UciOptionKind {
    check,
    spin,
    string,
    button,
};

enum class UciOptionId {
    random_seed,
    hash,
    threads,
    speed,
    analyse_mode,
    multi_pv,
    ponder,
    own_book,
    book_file,
    book_depth,
    book_random,
    book_safety,
    book_safety_depth,
    clear_hash,
    show_wdl,
    move_overhead,
    slow_mover,
    limit_strength,
    elo,
    strength_mode,
    syzygy_path,
    syzygy_probe_depth,
    syzygy_probe_limit,
    syzygy_50_move_rule,
    eval_file,
    debug,
    debug_file,
};

struct UciOptionDescriptor {
    std::string_view name;
    UciOptionKind kind;
    UciOptionId id;
    std::string_view default_value;
    std::uint64_t minimum;
    std::uint64_t maximum;
    bool dynamic_maximum;
    bool advertised;
};

// Single source of truth for the UCI option surface. Both `write_handshake`
// and `handle_setoption` consume this table, so an advertised option can never
// drift out of sync with the option the controller actually applies. The order
// of the entries is the order advertised to a GUI and must stay stable.
constexpr std::array<UciOptionDescriptor, 27> kUciOptions{{
    {"RandomSeed", UciOptionKind::spin, UciOptionId::random_seed, "0", 0,
     kMaximumRandomSeed, false, true},
    {"Hash", UciOptionKind::spin, UciOptionId::hash, "512", kMinimumHashMegabytes,
     kMaximumHashMegabytes, false, true},
    {"Threads", UciOptionKind::spin, UciOptionId::threads, "1", 1, 0, true, true},
    {"Speed", UciOptionKind::spin, UciOptionId::speed, "100", kMinimumSpeedPercent,
     kMaximumSpeedPercent, false, true},
    {"UCI_AnalyseMode", UciOptionKind::check, UciOptionId::analyse_mode, "false", 0, 0, false,
     true},
    {"MultiPV", UciOptionKind::spin, UciOptionId::multi_pv, "1", kMinimumMultiPv,
     kMaximumMultiPv, false, true},
    {"Ponder", UciOptionKind::check, UciOptionId::ponder, "false", 0, 0, false, true},
    {"OwnBook", UciOptionKind::check, UciOptionId::own_book, "true", 0, 0, false, true},
    {"BookFile", UciOptionKind::string, UciOptionId::book_file, "book.bin", 0, 0, false, true},
    {"BookDepth", UciOptionKind::spin, UciOptionId::book_depth, "16", 0, kMaximumBookDepth,
     false, true},
    {"BookRandom", UciOptionKind::check, UciOptionId::book_random, "false", 0, 0, false, true},
    {"BookSafety", UciOptionKind::check, UciOptionId::book_safety, "true", 0, 0, false, true},
    {"BookSafetyDepth", UciOptionKind::spin, UciOptionId::book_safety_depth, "2", 0,
     kMaximumBookSafetyDepth, false, true},
    {"Clear Hash", UciOptionKind::button, UciOptionId::clear_hash, "", 0, 0, false, true},
    {"UCI_ShowWDL", UciOptionKind::check, UciOptionId::show_wdl, "false", 0, 0, false, true},
    {"Move Overhead", UciOptionKind::spin, UciOptionId::move_overhead, "30", 0,
     kMaximumMoveOverheadMs, false, true},
    {"Slow Mover", UciOptionKind::spin, UciOptionId::slow_mover, "100", kMinimumSlowMoverPercent,
     kMaximumSlowMoverPercent, false, true},
    {"UCI_LimitStrength", UciOptionKind::check, UciOptionId::limit_strength, "false", 0, 0, false,
     true},
    {"UCI_Elo", UciOptionKind::spin, UciOptionId::elo, "1320", kMinimumElo, kMaximumElo, false,
     true},
    {"StrengthMode", UciOptionKind::check, UciOptionId::strength_mode, "false", 0, 0, false, true},
    {"SyzygyPath", UciOptionKind::string, UciOptionId::syzygy_path, "", 0, 0, false, true},
    {"SyzygyProbeDepth", UciOptionKind::spin, UciOptionId::syzygy_probe_depth, "1",
     kMinimumSyzygyProbeDepth, kMaximumSyzygyProbeDepth, false, true},
    {"SyzygyProbeLimit", UciOptionKind::spin, UciOptionId::syzygy_probe_limit, "5", 0,
     kMaximumSyzygyProbeLimit, false, true},
    {"Syzygy50MoveRule", UciOptionKind::check, UciOptionId::syzygy_50_move_rule, "true", 0, 0,
     false, true},
    // Empty keeps whatever evaluator the engine booted with (the classical
    // evaluator unless a `koi.nnue` network was found beside the executable or
    // advertised with KOI_NNUE_PATH).  A non-empty path loads and activates a
    // Koi NNUE network, with the classical evaluator retained as a fallback.
    {"EvalFile", UciOptionKind::string, UciOptionId::eval_file, "", 0, 0, false, true},
    // Developer diagnostics stay settable but are intentionally not advertised.
    {"Debug", UciOptionKind::check, UciOptionId::debug, "false", 0, 0, false, false},
    {"DebugFile", UciOptionKind::string, UciOptionId::debug_file, "koi-debug.log", 0, 0, false,
     false},
}};

const UciOptionDescriptor* find_uci_option(std::string_view name) {
    for (const UciOptionDescriptor& option : kUciOptions) {
        if (equals_ignore_case(name, option.name)) {
            return &option;
        }
    }
    return nullptr;
}

std::uint64_t option_maximum(const UciOptionDescriptor& option) {
    return option.dynamic_maximum ? static_cast<std::uint64_t>(maximum_search_threads())
                                  : option.maximum;
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

std::string debug_quoted(std::string_view value) {
    std::string result;
    result.reserve(value.size() + 2);
    result.push_back('"');
    for (const char character : value) {
        switch (character) {
        case '\\':
        case '"':
            result.push_back('\\');
            result.push_back(character);
            break;
        case '\b':
            result += "\\b";
            break;
        case '\f':
            result += "\\f";
            break;
        case '\n':
            result += "\\n";
            break;
        case '\r':
            result += "\\r";
            break;
        case '\t':
            result += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(character) < 0x20U) {
                std::ostringstream escaped;
                escaped << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                        << static_cast<unsigned int>(static_cast<unsigned char>(character));
                result += escaped.str();
            } else {
                result.push_back(character);
            }
            break;
        }
    }
    result.push_back('"');
    return result;
}

std::string debug_color(Color color) {
    return color == Color::white ? "w" : "b";
}

std::string debug_square(Square square) {
    return square.index() < Square::kInvalid ? square.uci() : "-";
}

std::string debug_castling(std::uint8_t rights) {
    std::string result;
    if ((rights & kWhiteKingSideCastling) != 0) result += 'K';
    if ((rights & kWhiteQueenSideCastling) != 0) result += 'Q';
    if ((rights & kBlackKingSideCastling) != 0) result += 'k';
    if ((rights & kBlackQueenSideCastling) != 0) result += 'q';
    return result.empty() ? "-" : result;
}

std::string debug_hex(std::uint64_t value) {
    std::ostringstream stream;
    stream << std::hex << std::setw(16) << std::setfill('0') << value;
    return stream.str();
}

std::uint64_t legal_move_digest(const std::vector<Move>& legal_moves) {
    std::vector<std::string> coordinates;
    coordinates.reserve(legal_moves.size());
    for (const Move& move : legal_moves) {
        coordinates.push_back(move.uci());
    }
    std::sort(coordinates.begin(), coordinates.end());

    std::uint64_t digest = 1469598103934665603ULL;
    for (const std::string& coordinate : coordinates) {
        for (const unsigned char character : coordinate) {
            digest ^= character;
            digest *= 1099511628211ULL;
        }
        digest ^= static_cast<unsigned char>('\n');
        digest *= 1099511628211ULL;
    }
    return digest;
}

template <typename T>
std::string debug_optional(const std::optional<T>& value) {
    return value.has_value() ? std::to_string(*value) : "-";
}

std::string debug_limits(const SearchLimits& limits) {
    std::ostringstream stream;
    stream << "depth=" << debug_optional(limits.depth)
           << ",nodes=" << debug_optional(limits.nodes)
           << ",movetime_ms=" <<
        (limits.movetime.has_value() ? std::to_string(limits.movetime->count()) : "-")
           << ",wtime_ms=" <<
        (limits.white_clock.has_value() ? std::to_string(limits.white_clock->remaining.count()) : "-")
           << ",btime_ms=" <<
        (limits.black_clock.has_value() ? std::to_string(limits.black_clock->remaining.count()) : "-")
           << ",winc_ms=" <<
        (limits.white_clock.has_value() ? std::to_string(limits.white_clock->increment.count()) : "-")
           << ",binc_ms=" <<
        (limits.black_clock.has_value() ? std::to_string(limits.black_clock->increment.count()) : "-")
           << ",movestogo=" << debug_optional(limits.moves_to_go)
           << ",infinite=" << (limits.infinite ? "true" : "false")
           << ",ponder=" << (limits.ponder ? "true" : "false")
           << ",searchmoves=" << (limits.search_moves_specified ? "true" : "false");
    if (limits.search_moves_specified) {
        stream << ",searchmove_list=";
        for (std::size_t index = 0; index < limits.search_moves.size(); ++index) {
            if (index != 0) stream << ',';
            stream << limits.search_moves[index].uci();
        }
    }
    return stream.str();
}

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

std::vector<Move> legal_pv_prefix(const GameState& root, const std::vector<Move>& pv) {
    GameState position = root;
    std::vector<Move> prefix;
    prefix.reserve(pv.size());
    for (const Move& move : pv) {
        if (!position.is_legal(move) || !position.make_move(move)) {
            break;
        }
        prefix.push_back(move);
    }
    return prefix;
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
        limits.movetime = kBareGoFallback;
    }
    return limits;
}

} // namespace uci

UciController::UciController(std::istream& input, std::ostream& output, std::ostream& diagnostics)
    : UciController(input, output, diagnostics,
                    SearchService{std::make_shared<ClassicalEvaluator>(), {}, 1}) {}

UciController::UciController(std::istream& input, std::ostream& output,
                             std::ostream& diagnostics, SearchService search_service,
                             std::filesystem::path executable_directory)
    : input_(input), output_(output), diagnostics_(diagnostics),
      executable_directory_(std::move(executable_directory)),
      opening_book_(executable_directory_),
      search_service_(std::move(search_service)) {}

UciController::~UciController() {
    {
        std::lock_guard lock(output_mutex_);
        state_ = ControllerState::ShuttingDown;
    }
    stop_and_suppress_active_search();
}

int UciController::run() {
    for (std::string line; std::getline(input_, line);) {
        {
            std::lock_guard lock(output_mutex_);
            last_command_ = line;
        }
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
            search_service_.clear_hash();
            debug_json_event("ucinewgame", "\"hash_cleared\":true");
            position_ = GameState::startpos();
        } else if (name == "position") {
            handle_position(command, line);
        } else if (name == "setoption") {
            handle_setoption(command);
        } else if (name == "go") {
            handle_go(command);
        } else if (name == "ponderhit") {
            handle_ponderhit();
        } else if (name == "debug") {
            // Standard UCI `debug on|off` command. It aliases the unadvertised
            // Debug option so both spellings configure the same diagnostics.
            std::string debug_value;
            if ((command >> debug_value) &&
                (equals_ignore_case(debug_value, "on") ||
                 equals_ignore_case(debug_value, "off"))) {
                const bool debug = equals_ignore_case(debug_value, "on");
                if (debug_enabled_ != debug) {
                    stop_and_suppress_active_search();
                    debug_enabled_ = debug;
                    configure_debug_file();
                }
            }
        } else if (name == "stop") {
            stop_active_search();
        } else if (name == "quit") {
            {
                std::lock_guard lock(output_mutex_);
                state_ = ControllerState::ShuttingDown;
            }
            stop_and_suppress_active_search();
            return 0;
        }
    }

    stop_and_suppress_active_search();
    return 0;
}

void UciController::handle_position(std::istream& command, std::string_view command_text) {
    // Suppress the old generation before doing any parsing work. A worker may
    // finish while a replacement position is being validated; it must not be
    // allowed to emit a completion for the old root during that window.
    {
        std::lock_guard lock(output_mutex_);
        state_ = ControllerState::ShuttingDown;
    }
    stop_and_suppress_active_search();
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

    position_ = std::move(candidate);
    const std::vector<Move> legal = position_.legal_moves();
    debug_event("position accepted generation " + std::to_string(generation_) +
                " key " + std::to_string(position_.position_key()) +
                " fen " + position_.fen() +
                " legal_moves " + std::to_string(legal.size()));
    if (debug_enabled_) {
        const PositionConsistencySnapshot snapshot = position_.consistency_snapshot();
        debug_json_event(
            "position",
            "\"message\":" + debug_quoted(
                "position_record command=" + std::string(command_text) +
                " legal_digest=" + debug_hex(legal_move_digest(legal))) +
                ",\"command\":" + debug_quoted(command_text) +
                ",\"generation\":" + std::to_string(generation_) +
                ",\"root_fen\":" + debug_quoted(snapshot.native_fen) +
                ",\"position_key\":" + debug_quoted(debug_hex(snapshot.native_position_key)) +
                ",\"side_to_move\":" + debug_quoted(
                    snapshot.native_side_to_move == Color::white ? "white" : "black") +
                ",\"castling_rights\":" + debug_quoted(
                    debug_castling(snapshot.native_castling_rights)) +
                ",\"en_passant\":" + debug_quoted(debug_square(position_.en_passant_square())) +
                ",\"halfmove_clock\":" + std::to_string(snapshot.native_halfmove_clock) +
                ",\"fullmove_number\":" + std::to_string(snapshot.native_fullmove_number) +
                ",\"legal_move_count\":" + std::to_string(snapshot.native_legal_moves.size()) +
                ",\"native_legal_count\":" + std::to_string(snapshot.native_legal_moves.size()) +
                ",\"shadow_legal_count\":" + std::to_string(snapshot.shadow_legal_moves.size()) +
                ",\"legal_move_digest\":" + debug_quoted(debug_hex(legal_move_digest(legal))) +
                ",\"native_shadow_consistent\":" +
                    (snapshot.consistent() ? "true" : "false"));
    }
}

void UciController::handle_setoption(std::istream& command) {
    const std::vector<std::string> tokens = remaining_tokens(command);
    const std::optional<ParsedOption> parsed = parse_setoption(tokens);
    if (!parsed.has_value()) {
        return;
    }
    const std::string& name = parsed->name;
    const std::string& value = parsed->value;
    const UciOptionDescriptor* option = find_uci_option(name);
    if (option == nullptr) {
        return;
    }

    // Type-aware helpers keep the parse -> ignore-invalid -> stop/suppress ->
    // commit sequence identical for every option: an invalid value never stops
    // the active search and never changes engine state.
    const auto apply_boolean = [this, &value](const auto& commit) {
        bool parsed_boolean = false;
        if (parse_boolean(value, parsed_boolean)) {
            stop_and_suppress_active_search();
            commit(parsed_boolean);
        }
    };
    const auto apply_unsigned = [this, &value](const UciOptionDescriptor& descriptor,
                                               const auto& commit) {
        std::uint64_t parsed_value = 0;
        if (parse_uint64(value, parsed_value) && parsed_value >= descriptor.minimum &&
            parsed_value <= option_maximum(descriptor)) {
            stop_and_suppress_active_search();
            commit(parsed_value);
        }
    };

    switch (option->id) {
    case UciOptionId::random_seed: {
        std::uint32_t seed = 0;
        if (parse_random_seed(value, seed)) {
            stop_and_suppress_active_search();
            chooser_.set_seed(seed);
            random_seed_ = seed;
        }
        break;
    }
    case UciOptionId::hash: {
        std::uint64_t megabytes = 0;
        if (parse_uint64(value, megabytes) && megabytes >= option->minimum &&
            megabytes <= option->maximum) {
            stop_and_suppress_active_search();
            hash_mb_ = static_cast<std::size_t>(megabytes);
            try {
                const HashResizeResult result =
                    search_service_.set_hash_size_mb(static_cast<std::size_t>(megabytes));
                debug_json_event(
                    "hash_resize",
                    "\"requested_mb\":" + std::to_string(result.requested_mb) +
                        ",\"effective_mb\":" + std::to_string(result.effective_mb) +
                        ",\"allocated_bytes\":" + std::to_string(result.allocated_bytes) +
                        ",\"segment_count\":" + std::to_string(result.segment_count) +
                        ",\"total_physical_bytes\":" +
                            std::to_string(result.total_physical_bytes) +
                        ",\"available_physical_bytes\":" +
                            std::to_string(result.available_physical_bytes) +
                        ",\"available_commit_bytes\":" +
                            std::to_string(result.available_commit_bytes) +
                        ",\"status\":" + debug_quoted(hash_status_name(result.status)) +
                        ",\"reason\":" + debug_quoted(hash_reason_name(result.reason)));
                if (result.status == HashResizeStatus::reduced ||
                    result.status == HashResizeStatus::disabled ||
                    result.reason != HashResizeReason::none) {
                    std::lock_guard lock(output_mutex_);
                    output_ << "info string hash requested " << result.requested_mb
                            << " MB effective " << result.effective_mb << " MB status "
                            << hash_status_name(result.status) << " reason "
                            << hash_reason_name(result.reason) << '\n' << std::flush;
                }
            } catch (...) {
                debug_event("hash resize failed outside the guarded allocation boundary");
                std::lock_guard lock(output_mutex_);
                output_ << "info string hash resize failed; previous table preserved\n" << std::flush;
            }
        }
        break;
    }


    case UciOptionId::threads:
        apply_unsigned(*option, [this](std::uint64_t threads) {
            threads_ = static_cast<std::size_t>(threads);
        });
        break;

    case UciOptionId::speed:
        apply_unsigned(*option, [this](std::uint64_t speed) {
            speed_percent_ = static_cast<std::uint8_t>(speed);
        });
        break;

    case UciOptionId::analyse_mode: {
        bool analyse_mode = false;
        if (parse_boolean(value, analyse_mode) && analyse_mode_ != analyse_mode) {
            stop_and_suppress_active_search();
            analyse_mode_ = analyse_mode;
        }
        break;
    }

    case UciOptionId::multi_pv:
        apply_unsigned(*option, [this](std::uint64_t multi_pv) {
            multi_pv_ = static_cast<std::size_t>(multi_pv);
        });
        break;

    case UciOptionId::ponder:
        apply_boolean([this](bool ponder_enabled) { ponder_enabled_ = ponder_enabled; });
        break;

    case UciOptionId::own_book:
        apply_boolean([this](bool own_book) { own_book_ = own_book; });
        break;

    case UciOptionId::book_random:
        apply_boolean([this](bool book_random) { book_random_ = book_random; });
        break;

    case UciOptionId::book_safety:
        apply_boolean([this](bool book_safety) { book_safety_ = book_safety; });
        break;

    case UciOptionId::book_safety_depth:
        apply_unsigned(*option, [this](std::uint64_t book_safety_depth) {
            book_safety_depth_ = static_cast<std::uint8_t>(book_safety_depth);
        });
        break;

    case UciOptionId::book_file:
        if (!value.empty()) {
            stop_and_suppress_active_search();
            opening_book_.set_file(value);
        }
        break;

    case UciOptionId::book_depth:
        apply_unsigned(*option, [this](std::uint64_t book_depth) {
            book_depth_ = static_cast<std::uint8_t>(book_depth);
        });
        break;

    case UciOptionId::clear_hash:
        stop_and_suppress_active_search();
        search_service_.clear_hash();
        break;

    case UciOptionId::show_wdl:
        apply_boolean([this](bool show_wdl) { show_wdl_ = show_wdl; });
        break;

    case UciOptionId::move_overhead:
        apply_unsigned(*option, [this](std::uint64_t overhead) {
            move_overhead_ms_ = static_cast<std::uint32_t>(overhead);
        });
        break;

    case UciOptionId::slow_mover:
        apply_unsigned(*option, [this](std::uint64_t slow_mover) {
            slow_mover_percent_ = static_cast<std::uint32_t>(slow_mover);
        });
        break;

    case UciOptionId::limit_strength:
        apply_boolean([this](bool limit_strength) { limit_strength_ = limit_strength; });
        break;

    case UciOptionId::elo:
        apply_unsigned(*option, [this](std::uint64_t elo) {
            elo_ = static_cast<std::uint32_t>(elo);
        });
        break;

    case UciOptionId::strength_mode: {
        bool strength_mode = false;
        if (parse_boolean(value, strength_mode) && strength_mode_ != strength_mode) {
            stop_and_suppress_active_search();
            strength_mode_ = strength_mode;
        }
        break;
    }

    case UciOptionId::syzygy_path:
        stop_and_suppress_active_search();
        syzygy_path_ = value;
        rebuild_syzygy();
        break;

    case UciOptionId::syzygy_probe_depth:
        apply_unsigned(*option, [this](std::uint64_t depth) {
            syzygy_probe_depth_ = static_cast<std::uint8_t>(depth);
            rebuild_syzygy();
        });
        break;

    case UciOptionId::syzygy_probe_limit:
        apply_unsigned(*option, [this](std::uint64_t limit) {
            syzygy_probe_limit_ = static_cast<std::uint8_t>(limit);
            rebuild_syzygy();
        });
        break;

    case UciOptionId::syzygy_50_move_rule:
        apply_boolean([this](bool fifty_move_rule) {
            syzygy_50_move_rule_ = fifty_move_rule;
            rebuild_syzygy();
        });
        break;

    case UciOptionId::eval_file: {
        if (value.empty()) {
            // The advertised default is empty: keep the boot-time evaluator
            // rather than failing a GUI that replays every default value.
            break;
        }
        // Relative paths resolve against the engine directory so a GUI can
        // pass a bare file name that sits next to koi-engine.exe.
        std::filesystem::path resolved = value;
        if (resolved.is_relative() && !executable_directory_.empty()) {
            resolved = executable_directory_ / resolved;
        }
        stop_and_suppress_active_search();
        const std::expected<NnueNetwork, NnueError> network = NnueLoader::load_file(resolved);
        if (!network.has_value()) {
            debug_event("EvalFile rejected " + resolved.string() + ": " +
                        network.error().message);
            std::lock_guard lock(output_mutex_);
            output_ << "info string EvalFile rejected: " << network.error().message << '\n'
                    << std::flush;
            break;
        }
        auto weights = std::make_shared<const NnueNetwork>(std::move(*network));
        search_service_.set_evaluator(std::make_shared<NnueEvaluator>(std::move(weights)));
        eval_file_ = value;
        debug_json_event("eval_file", "\"path\":" + debug_quoted(resolved.string()) +
                                           ",\"enabled\":true");
        std::lock_guard lock(output_mutex_);
        output_ << "info string NNUE enabled from " << resolved.string() << '\n' << std::flush;
        break;
    }

    case UciOptionId::debug:
        apply_boolean([this](bool debug) {
            debug_enabled_ = debug;
            configure_debug_file();
        });
        break;

    case UciOptionId::debug_file:
        stop_and_suppress_active_search();
        debug_file_path_ = value;
        if (debug_enabled_) {
            configure_debug_file();
        }
        break;
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
        limits.movetime = kBareGoFallback;
    }
    stop_and_suppress_active_search();

    if (limits.ponder) {
        ponder_origin_ = position_;
        ponder_root_ = position_;
        ponder_limits_ = limits;
        ponder_predicted_move_.reset();
        active_ponder_ = true;
    }
    start_search(position_, std::move(limits));
}

void UciController::handle_ponderhit() {
    SearchLimits base_limits;
    {
        std::lock_guard lock(output_mutex_);
        if (!active_ponder_ || !ponder_limits_.has_value()) {
            return;
        }
        base_limits = *ponder_limits_;
    }

    SearchLimits converted = base_limits;
    converted.ponder = false;
    const bool has_side_to_move_clock = position_.side_to_move() == Color::white ?
        converted.white_clock.has_value() : converted.black_clock.has_value();
    const bool has_normal_limit = converted.depth.has_value() || converted.nodes.has_value() ||
        converted.movetime.has_value() || has_side_to_move_clock || converted.infinite;
    if (!has_normal_limit) {
        converted.movetime = kBareGoFallback;
    }

    if (active_search_.has_value() && active_search_->running()) {
        // Standard UCI continuation: the pondering search already searches the
        // exact root the GUI ponders after, so the ponderhit is converted in
        // place. The worker keeps its TT-warm state, identity, and generation;
        // only time management switches from unbounded to the real budget.
        active_search_->request_ponderhit(std::move(converted));
        std::uint64_t generation = 0;
        {
            std::lock_guard lock(output_mutex_);
            ponder_origin_.reset();
            ponder_root_.reset();
            ponder_limits_.reset();
            ponder_predicted_move_.reset();
            ponder_expected_move_.reset();
            active_ponder_ = false;
            state_ = ControllerState::Searching;
            generation = generation_;
        }
        debug_json_event("ponderhit",
                         "\"continued\":true,\"generation\":" + std::to_string(generation));
        return;
    }

    // No running search to convert (for example a node-limited ponder that
    // already completed). The stale ponder search is suppressed and a fresh,
    // bounded search of the real position answers this move so the GUI always
    // receives exactly one bestmove.
    debug_event("ponderhit without a running ponder search; searching the current position");
    stop_and_suppress_active_search();
    converted.search_moves_specified = false;
    converted.search_moves.clear();
    start_search(position_, converted, true);
}

void UciController::start_search(GameState root, SearchLimits limits, bool skip_book) {
    const std::uint64_t generation = begin_generation();
    {
        std::lock_guard lock(output_mutex_);
        state_ = limits.ponder ? ControllerState::Pondering : ControllerState::Searching;
    }
    const auto completion_once = std::make_shared<CompletionOnce>();
    debug_event("search start generation " + std::to_string(generation));
    debug_event("search root generation " + std::to_string(generation) +
                " key " + std::to_string(root.position_key()) +
                " fen " + root.fen());
    const bool book_eligible = !skip_book && !analyse_mode_ && multi_pv_ == 1 && !limits.infinite &&
        !limits.ponder && !limits.search_moves_specified;
    std::optional<BookChoice> book_choice;
    std::uint32_t ply = 0;
    if (book_eligible) {
        ply = root_ply(root);
        book_choice = opening_book_.choose(root, ply, own_book_, book_depth_, random_seed_,
                                           book_random_, book_safety_, book_safety_depth_);
    }
    debug_json_event(
        "go",
        "\"message\":" + debug_quoted(
            "go_record generation=" + std::to_string(generation) +
            " root_fen=" + root.fen()) +
            ",\"generation\":" + std::to_string(generation) +
            ",\"root_fen\":" + debug_quoted(root.fen()) +
            ",\"root_key\":" + debug_quoted(debug_hex(root.position_key())) +
            ",\"limits\":" + debug_quoted(debug_limits(limits)) +
            ",\"threads\":" + std::to_string(threads_) +
            ",\"source\":" + debug_quoted(book_choice.has_value() ? "book" : "search"));
    if (book_choice.has_value()) {
        debug_event("book completion candidate generation " + std::to_string(generation) +
                    " move " + book_choice->move.uci() + " root " + root.fen());
        write_book_completion(generation, root, limits, *book_choice, ply, completion_once);
        return;
    }
    const GameState search_root = root;
    const bool is_ponder_search = limits.ponder;
    const bool allow_ponder_move = !limits.search_moves_specified;

    SearchEventSink sink;
    sink.on_info = [this, generation, search_root, is_ponder_search](const SearchInfo& info) {
        SearchInfo safe_info = info;
        safe_info.pv = legal_pv_prefix(search_root, info.pv);
        if (safe_info.pv.size() != info.pv.size()) {
            debug_event("invalid PV prefix suppressed generation " + std::to_string(generation));
        }
        if (safe_info.multipv == 1 && safe_info.pv.size() >= 2) {
            GameState after_best = search_root;
            if (after_best.make_move(safe_info.pv[0]) && after_best.is_legal(safe_info.pv[1])) {
                std::lock_guard lock(output_mutex_);
                if (generation == generation_) {
                    principal_variation_best_move_ = safe_info.pv[0];
                    principal_variation_ponder_move_ = safe_info.pv[1];
                    if (is_ponder_search && !ponder_expected_move_.has_value()) {
                        ponder_root_ = std::move(after_best);
                        ponder_predicted_move_ = safe_info.pv[0];
                        ponder_expected_move_ = safe_info.pv[1];
                    }
                }
            }
        }
        write_search_info(generation, safe_info);
    };
    sink.on_complete = [this, generation, allow_ponder_move, search_root, limits, completion_once](
                           const SearchResult& result) {
        debug_event("search complete generation " + std::to_string(generation));
        CompletionCandidate candidate;
        candidate.best_move = result.best_move;
        candidate.pv = result.pv;
        candidate.ponder_move = result.ponder_move;
        candidate.identity = result.identity;
        candidate.source = result.stats.tbhits != 0 ? CompletionSource::tablebase :
            (limits.ponder ? CompletionSource::ponder : CompletionSource::search);
        const SearchRequestIdentity expected = SearchRequestIdentity::from(search_root, generation);
        std::string last_command;
        {
            std::lock_guard lock(output_mutex_);
            if (generation == generation_ && allow_ponder_move && candidate.best_move.has_value() &&
                candidate.best_move == principal_variation_best_move_) {
                candidate.ponder_move = principal_variation_ponder_move_;
            }
            last_command = last_command_;
        }
        const CompletionValidation validation = completion_gate_.validate(
            search_root, limits, expected, candidate);
        const auto disposition_name = [](CompletionDisposition disposition) {
            switch (disposition) {
            case CompletionDisposition::emit: return "emit";
            case CompletionDisposition::suppress_stale: return "suppress_stale";
            case CompletionDisposition::fallback: return "fallback";
            case CompletionDisposition::quarantine: return "quarantine";
            }
            return "unknown";
        };
        const std::string source = result.stats.tbhits != 0 ? "tablebase" :
            (limits.ponder ? "ponder" : "search");
        debug_json_event(
            "completion_validation",
            "\"message\":" + debug_quoted(
                "completion_record generation=" + std::to_string(generation)) +
                ",\"generation\":" + std::to_string(generation) +
                ",\"result_generation\":" + std::to_string(result.identity.generation) +
                ",\"root_fen\":" + debug_quoted(search_root.fen()) +
                ",\"root_key\":" + debug_quoted(debug_hex(search_root.position_key())) +
                ",\"result_root_key\":" + debug_quoted(debug_hex(result.identity.root_key)) +
                ",\"root_key_match\":" +
                    (result.identity.root_key == search_root.position_key() ? "true" : "false") +
                ",\"root_fen_match\":" +
                    (result.identity.root_fen == search_root.fen() ? "true" : "false") +
                ",\"identity_match\":" + (validation.identity_match ? "true" : "false") +
                ",\"candidate\":" + debug_quoted(
                    candidate.best_move.has_value() ? candidate.best_move->uci() : "0000") +
                ",\"native_legal\":" + (validation.native_legal ? "true" : "false") +
                ",\"shadow_legal\":" + (validation.shadow_legal ? "true" : "false") +
                ",\"searchmoves_legal\":" + (validation.searchmoves_legal ? "true" : "false") +
                ",\"pv_legal\":" + (validation.pv_legal ? "true" : "false") +
                ",\"ponder_legal\":" + (validation.ponder_legal ? "true" : "false") +
                ",\"fallback_used\":" + (validation.fallback_used ? "true" : "false") +
                ",\"validated\":" + debug_quoted(
                    validation.best_move.has_value() ? validation.best_move->uci() : "0000") +
                ",\"source\":" + debug_quoted(source) +
                ",\"disposition\":" + debug_quoted(disposition_name(validation.disposition)) +
                ",\"reason\":" + debug_quoted(validation.reason) +
                ",\"completed\":" + (result.completed ? "true" : "false") +
                ",\"cancelled\":" + (result.cancelled ? "true" : "false") +
                 ",\"failed\":" + (result.failed ? "true" : "false") +
                 ",\"timing_reserve_ms\":" +
                     std::to_string(result.timing.reserve.count()) +
                 ",\"timing_usable_ms\":" +
                     std::to_string(result.timing.usable.count()) +
                 ",\"timing_soft_ms\":" +
                     std::to_string(result.timing.soft_budget.count()) +
                 ",\"timing_hard_ms\":" +
                     std::to_string(result.timing.hard_budget.count()) +
                 ",\"timing_horizon\":" + std::to_string(result.timing.horizon) +
                 ",\"timing_initial_hardness\":" +
                     std::to_string(result.timing.initial_hardness) +
                 ",\"timing_observed_hardness\":" +
                     std::to_string(result.timing.observed_hardness) +
                 ",\"timing_extended\":" +
                     (result.timing.extended_for_hard_position ? "true" : "false") +
                 ",\"timing_hard_deadline\":" +
                     (result.timing.hard_deadline_reached ? "true" : "false") +
                 ",\"last_command\":" + debug_quoted(last_command));
        if (validation.disposition == CompletionDisposition::suppress_stale) {
            debug_event("stale search completion suppressed generation " +
                        std::to_string(generation) + " result_generation " +
                        std::to_string(result.identity.generation) + " result_key " +
                        std::to_string(result.identity.root_key));
            return;
        }
        if (validation.fallback_used) {
            debug_event("illegal or shadow-rejected search move replaced generation " +
                        std::to_string(generation) + " fallback " +
                        (validation.best_move.has_value() ? validation.best_move->uci() : "0000"));
        }
        if (validation.disposition == CompletionDisposition::quarantine) {
            debug_event("fatal no common legal move for nonterminal search root generation " +
                        std::to_string(generation) + " fen " + search_root.fen());
            std::quick_exit(74);
        }
        if (!completion_once->try_claim()) {
            debug_event("duplicate search completion suppressed generation " +
                        std::to_string(generation));
            return;
        }
        SearchResult completed = result;
        completed.best_move = validation.best_move;
        completed.pv = validation.pv;
        completed.ponder_move = validation.ponder_move;
        std::string after_fen = search_root.fen();
        if (validation.best_move.has_value()) {
            GameState after_move = search_root;
            if (after_move.make_move(*validation.best_move)) {
                after_fen = after_move.fen();
            }
        }
        debug_json_event(
            "move",
            "\"generation\":" + std::to_string(generation) +
                ",\"root_fen\":" + debug_quoted(search_root.fen()) +
                ",\"root_key\":" + debug_quoted(debug_hex(search_root.position_key())) +
                ",\"move\":" + debug_quoted(
                    validation.best_move.has_value() ? validation.best_move->uci() : "0000") +
                ",\"after_fen\":" + debug_quoted(after_fen) +
                ",\"source\":" + debug_quoted(source));
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
    options.generation = generation;
    options.strength_mode = strength_mode_;
    options.syzygy = syzygy_;
    active_search_.emplace(search_service_.start(std::move(root), std::move(limits), std::move(sink), options));
}

void UciController::stop_active_search() {
    {
        std::lock_guard lock(output_mutex_);
        if (state_ != ControllerState::ShuttingDown) {
            state_ = ControllerState::Stopping;
        }
    }
    if (!active_search_.has_value()) {
        clear_ponder_state();
        std::lock_guard lock(output_mutex_);
        if (state_ != ControllerState::ShuttingDown) state_ = ControllerState::Idle;
        return;
    }

    debug_event("search cancellation requested");
    active_search_->stop();
    active_search_->wait();
    active_search_.reset();
    clear_ponder_state();
    std::lock_guard lock(output_mutex_);
    if (state_ != ControllerState::ShuttingDown) state_ = ControllerState::Idle;
}

void UciController::stop_and_suppress_active_search() {
    {
        std::lock_guard lock(output_mutex_);
        ++generation_;
        if (state_ != ControllerState::ShuttingDown) {
            state_ = ControllerState::Stopping;
        }
    }
    debug_event("search cancellation requested with generation suppression");
    if (active_search_.has_value()) {
        active_search_->stop();
        active_search_->wait();
        active_search_.reset();
    }
    clear_ponder_state();
    {
        std::lock_guard lock(output_mutex_);
        if (state_ != ControllerState::ShuttingDown) state_ = ControllerState::Idle;
    }
}

void UciController::clear_ponder_state() {
    ponder_origin_.reset();
    ponder_root_.reset();
    ponder_limits_.reset();
    ponder_predicted_move_.reset();
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
               "id author Koi Engine contributors\n";
    for (const UciOptionDescriptor& option : kUciOptions) {
        if (!option.advertised) {
            continue;
        }
        output_ << "option name " << option.name << " type ";
        switch (option.kind) {
        case UciOptionKind::spin:
            output_ << "spin default " << option.default_value << " min " << option.minimum
                    << " max " << option_maximum(option);
            break;
        case UciOptionKind::check:
            output_ << "check default " << option.default_value;
            break;
        case UciOptionKind::string:
            output_ << "string default " << option.default_value;
            break;
        case UciOptionKind::button:
            output_ << "button";
            break;
        }
        output_ << '\n';
    }
    output_ << "uciok\n" << std::flush;
}

void UciController::write_readyok() {
    // The production UCI controller starts with a one-megabyte bootstrap table
    // so a large default hash cannot consume a fast game's clock before the
    // GUI has delivered its options. Materialize the requested table while the
    // engine is idle; explicit Hash options have already resized it here.
    if (!active_search_.has_value()) {
        try {
            (void)search_service_.set_hash_size_mb(hash_mb_);
        } catch (...) {
            debug_event("deferred hash materialization failed; existing table preserved");
        }
    }
    std::lock_guard lock(output_mutex_);
    output_ << "readyok\n" << std::flush;
}

void UciController::write_search_info(std::uint64_t generation, const SearchInfo& info) {
    std::lock_guard lock(output_mutex_);
    if (generation != generation_ || info.depth <= 0) {
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
            << " hashfull " << search_service_.hashfull_permill()
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

void UciController::write_book_completion(std::uint64_t generation, const GameState& root,
                                           const SearchLimits& limits, const BookChoice& choice,
                                           std::uint32_t ply,
                                           const std::shared_ptr<CompletionOnce>& completion_once) {
    CompletionCandidate candidate;
    candidate.best_move = choice.move;
    candidate.pv = {choice.move};
    candidate.identity = SearchRequestIdentity::from(root, generation);
    candidate.source = CompletionSource::book;
    const CompletionValidation validation = completion_gate_.validate(
        root, limits, candidate.identity, candidate);
    std::string last_command;
    {
        std::lock_guard lock(output_mutex_);
        last_command = last_command_;
    }
    const auto disposition_name = [](CompletionDisposition disposition) {
        switch (disposition) {
        case CompletionDisposition::emit: return "emit";
        case CompletionDisposition::suppress_stale: return "suppress_stale";
        case CompletionDisposition::fallback: return "fallback";
        case CompletionDisposition::quarantine: return "quarantine";
        }
        return "unknown";
    };
    debug_json_event(
        "completion_validation",
        "\"message\":" + debug_quoted(
            "book_completion_record generation=" + std::to_string(generation)) +
            ",\"generation\":" + std::to_string(generation) +
            ",\"result_generation\":" + std::to_string(candidate.identity.generation) +
            ",\"root_fen\":" + debug_quoted(root.fen()) +
            ",\"root_key\":" + debug_quoted(debug_hex(root.position_key())) +
            ",\"result_root_key\":" + debug_quoted(debug_hex(candidate.identity.root_key)) +
            ",\"root_key_match\":true" +
            ",\"root_fen_match\":true" +
            ",\"identity_match\":" + (validation.identity_match ? "true" : "false") +
            ",\"candidate\":" + debug_quoted(choice.move.uci()) +
            ",\"native_legal\":" + (validation.native_legal ? "true" : "false") +
            ",\"shadow_legal\":" + (validation.shadow_legal ? "true" : "false") +
            ",\"searchmoves_legal\":" + (validation.searchmoves_legal ? "true" : "false") +
            ",\"pv_legal\":" + (validation.pv_legal ? "true" : "false") +
            ",\"ponder_legal\":" + (validation.ponder_legal ? "true" : "false") +
            ",\"fallback_used\":" + (validation.fallback_used ? "true" : "false") +
            ",\"validated\":" + debug_quoted(
                validation.best_move.has_value() ? validation.best_move->uci() : "0000") +
            ",\"source\":\"book\"" +
            ",\"disposition\":" + debug_quoted(disposition_name(validation.disposition)) +
            ",\"reason\":" + debug_quoted(validation.reason) +
            ",\"last_command\":" + debug_quoted(last_command));
    if (validation.disposition == CompletionDisposition::suppress_stale) {
        debug_event("book completion suppressed as stale generation " + std::to_string(generation));
        return;
    }
    if (validation.disposition == CompletionDisposition::quarantine) {
        debug_event("fatal no common legal move for nonterminal book root generation " +
                    std::to_string(generation) + " fen " + root.fen());
        std::quick_exit(74);
    }
    if (!completion_once->try_claim()) {
        debug_event("duplicate book completion suppressed generation " + std::to_string(generation));
        return;
    }
    if (!validation.best_move.has_value()) {
        // Defensive hardening: a validated book completion must never be
        // dereferenced without a move. Mirrors the search path, which answers
        // 0000 so a `go` always receives exactly one bestmove.
        debug_event("book completion without a validated move generation " +
                    std::to_string(generation) + "; answering 0000");
        std::lock_guard lock(output_mutex_);
        if (generation != generation_) {
            return;
        }
        output_ << "bestmove 0000\n" << std::flush;
        return;
    }
    debug_json_event(
        "move",
        "\"generation\":" + std::to_string(generation) +
            ",\"root_fen\":" + debug_quoted(root.fen()) +
            ",\"root_key\":" + debug_quoted(debug_hex(root.position_key())) +
            ",\"move\":" + debug_quoted(
                validation.best_move.has_value() ? validation.best_move->uci() : "0000") +
            ",\"source\":\"book\"" +
            ",\"book_depth\":" + std::to_string(ply));
    std::lock_guard lock(output_mutex_);
    if (generation != generation_) {
        return;
    }

    output_ << "info string book move " << validation.best_move->uci() << " depth " << ply << '\n'
             << "bestmove " << validation.best_move->uci() << '\n' << std::flush;
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
    debug_json_event("log", "\"message\":" + debug_quoted(message));
}

void UciController::debug_json_event(std::string event, std::string fields) noexcept {
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
        std::string message = "{\"event\":" + debug_quoted(event);
        if (!fields.empty()) {
            message += ',';
            message += fields;
        }
        message += "}\n";
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
