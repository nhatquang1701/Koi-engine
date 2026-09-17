#include <iostream>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "koi/detail/evaluation_context.hpp"
#include "koi/detail/search_context.hpp"
#include "koi/evaluator.hpp"
#include "koi/move.hpp"

#include "koi_test_support.hpp"

namespace {

using koi::test::require;

class CountingWorker final : public koi::EvaluatorWorker {
public:
    explicit CountingWorker(const int value) : value_(value) {}

    int evaluate(const koi::GameState&, const koi::Color) override {
        return value_;
    }

    void on_make_move(const koi::GameState&, const koi::MoveMetadata&, const int,
                      const std::uint64_t) override {
        ++make_moves;
    }

    void on_unmake_move(const int) override {
        ++unmake_moves;
    }

    void on_make_null_move(const koi::GameState&, const int, const std::uint64_t) override {
        ++make_nulls;
    }

    void on_unmake_null_move(const int) override {
        ++unmake_nulls;
    }

    int make_moves = 0;
    int unmake_moves = 0;
    int make_nulls = 0;
    int unmake_nulls = 0;

private:
    int value_;
};

class WorkerEvaluator final : public koi::Evaluator {
public:
    int evaluate(const koi::GameState&, const koi::Color) const override {
        return 3;
    }

    std::unique_ptr<koi::EvaluatorWorker> create_worker() const override {
        ++worker_creations;
        auto worker = std::make_unique<CountingWorker>(17);
        last_worker = worker.get();
        return worker;
    }

    mutable int worker_creations = 0;
    mutable CountingWorker* last_worker = nullptr;
};

class BaseOnlyEvaluator final : public koi::Evaluator {
public:
    int evaluate(const koi::GameState&, const koi::Color) const override {
        return 9;
    }
};

void test_worker_context_owns_one_private_worker_per_search_context() {
    WorkerEvaluator evaluator;
    koi::detail::EvaluationContext first(evaluator, nullptr);
    koi::detail::EvaluationContext second(evaluator, nullptr);
    const koi::GameState root = koi::GameState::startpos();

    require(first.has_private_worker() && second.has_private_worker(),
            "worker-capable evaluators must create private evaluation workers");
    require(evaluator.worker_creations == 2,
            "each evaluation context must create exactly one worker");
    require(first.evaluate(root, koi::Color::white) == 17 &&
                second.evaluate(root, koi::Color::black) == 17,
            "evaluation contexts must call their private workers");
}

void test_worker_context_preserves_base_evaluator_fallback() {
    BaseOnlyEvaluator evaluator;
    koi::detail::EvaluationContext context(evaluator, nullptr);
    require(!context.has_private_worker(),
            "evaluators without a worker capability must use the base path");
    require(context.evaluate(koi::GameState::startpos(), koi::Color::white) == 9,
            "the base evaluator must remain authoritative without a worker");
}

void test_search_context_owns_evaluation_execution_boundary() {
    require(koi::detail::SearchContext::has_evaluation_context(),
            "search context must expose a private evaluation-context boundary");
}

void test_evaluation_context_forwards_advisory_notifications() {
    WorkerEvaluator evaluator;
    koi::detail::EvaluationContext context(evaluator, nullptr);
    require(evaluator.last_worker != nullptr,
            "notification forwarding requires a private worker");

    koi::GameState state = koi::GameState::startpos();
    const auto move = koi::Move::parse_uci("e2e4");
    require(move.has_value(), "notification fixture move must parse");
    const auto metadata = state.describe_move(*move);
    require(metadata.has_value(), "notification fixture move must describe");

    context.notify_make_move(state, *metadata, 0, state.position_key());
    context.notify_unmake_move(1);
    context.notify_make_null_move(state, 1, state.position_key());
    context.notify_unmake_null_move(2);
    require(evaluator.last_worker->make_moves == 1 &&
                evaluator.last_worker->unmake_moves == 1 &&
                evaluator.last_worker->make_nulls == 1 &&
                evaluator.last_worker->unmake_nulls == 1,
            "evaluation context must forward every advisory notification");

    BaseOnlyEvaluator base_only;
    koi::detail::EvaluationContext base_context(base_only, nullptr);
    base_context.notify_make_move(state, *metadata, 0, state.position_key());
    base_context.notify_unmake_move(1);
    base_context.notify_make_null_move(state, 1, state.position_key());
    base_context.notify_unmake_null_move(2);
}

} // namespace

int main(int argc, char** argv) {
    const std::vector<koi::test::TestCase> tests{
        {"worker ownership", test_worker_context_owns_one_private_worker_per_search_context},
        {"base fallback", test_worker_context_preserves_base_evaluator_fallback},
        {"search evaluation boundary", test_search_context_owns_evaluation_execution_boundary},
        {"advisory notification forwarding",
         test_evaluation_context_forwards_advisory_notifications},
    };

    return koi::test::run_tests(tests, argc, argv);
}
