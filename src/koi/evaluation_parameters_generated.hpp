#pragma once

#include <cstddef>
#include <string_view>

namespace koi {

struct EvaluationParameterMetadata {
    std::string_view format_version;
    std::string_view parameter_version;
    std::string_view train_corpus_sha256;
    std::string_view validation_corpus_sha256;
    std::string_view holdout_corpus_sha256;
    std::size_t train_position_count = 0;
    std::size_t validation_position_count = 0;
    std::size_t holdout_position_count = 0;
};

// This header is deliberately compiled into the engine. Runtime evaluation
// never opens a generated parameter file; the corpus fields are provenance
// metadata for offline tuning artifacts.
inline constexpr EvaluationParameterMetadata kClassicalEvaluationParameterMetadata{
    "koi-evaluation-parameters-v1",
    "classical-eval-v7-opening-queen-discipline",
    "builtin",
    "builtin",
    "builtin",
    0,
    0,
    0,
};

inline constexpr const auto& kGeneratedEvaluationParameterMetadata =
    kClassicalEvaluationParameterMetadata;

} // namespace koi
