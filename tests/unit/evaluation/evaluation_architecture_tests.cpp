#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "koi/detail/evaluation_context.hpp"
#include "koi/detail/search_context.hpp"
#include "koi/evaluator.hpp"

#include "koi_test_support.hpp"

namespace {

using koi::test::require;

class CountingWorker final : public koi::EvaluatorWorker {
public:
    explicit CountingWorker(const int value) : value_(value) {}

    int evaluate(const koi::GameState&, const koi::Color) override {
        return value_;
    }

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
        return std::make_unique<CountingWorker>(17);
    }

    mutable int worker_creations = 0;
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

} // namespace

int main(int argc, char** argv) {
    const std::vector<koi::test::TestCase> tests{
        {"worker ownership", test_worker_context_owns_one_private_worker_per_search_context},
        {"base fallback", test_worker_context_preserves_base_evaluator_fallback},
        {"search evaluation boundary", test_search_context_owns_evaluation_execution_boundary},
    };

    return koi::test::run_tests(tests, argc, argv);
}
