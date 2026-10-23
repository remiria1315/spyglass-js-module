#pragma once

#include <cstddef>
#include <string_view>

namespace spyglass {

void *find(std::string_view pattern);

void *find_within(const void *begin, std::size_t size, std::string_view pattern);

}  // namespace spyglass
