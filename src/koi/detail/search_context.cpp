#include "koi/detail/search_context.hpp"

namespace koi::detail {

SearchContext::~SearchContext() = default;

static_assert(SearchContext::stack_capacity() == SearchStack::kCapacity);

} // namespace koi::detail
