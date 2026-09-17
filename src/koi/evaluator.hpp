#pragma once

#include <cstdint>
#include <memory>

#include "koi/game_state.hpp"

namespace koi {

class EvaluatorWorker {
public:
    virtual ~EvaluatorWorker() = default;
    [[nodiscard]] virtual int evaluate(const GameState&, Color) = 0;

    // Advisory search-lifecycle notifications. Stateful evaluators can use
    // them to maintain incremental state instead of recomputing from scratch.
    // They are hints only: an evaluator must still produce a correct result
    // when any notification is skipped, so implementations may treat them as
    // "maybe update" and verify against the position key before trusting any
    // cached state. `parent_key` is the position key before the move; `ply`
    // is the ply of the parent position, and the child ply is `ply + 1`.
    virtual void on_make_move(const GameState&, const MoveMetadata&, int /*ply*/,
                              std::uint64_t /*parent_key*/) {}
    virtual void on_unmake_move(int /*child_ply*/) {}
    virtual void on_make_null_move(const GameState&, int /*ply*/,
                                   std::uint64_t /*parent_key*/) {}
    virtual void on_unmake_null_move(int /*child_ply*/) {}
};

class Evaluator {
public:
    virtual ~Evaluator() = default;
    [[nodiscard]] virtual int evaluate(const GameState&, Color perspective) const = 0;

    // A worker is optional. Stateful evaluators can use it to keep accumulator
    // or cache state local to one search context; stateless evaluators retain
    // the existing shared evaluation path.
    [[nodiscard]] virtual std::unique_ptr<EvaluatorWorker> create_worker() const {
        return {};
    }

    // Search may invoke an evaluator concurrently when Threads > 1. The
    // default keeps third-party evaluators safe by having the search service
    // serialize calls unless an implementation explicitly opts in.
    [[nodiscard]] virtual bool supports_concurrent_evaluation() const noexcept {
        return false;
    }
};

} // namespace koi
