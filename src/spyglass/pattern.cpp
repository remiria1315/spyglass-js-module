#include "spyglass/pattern.h"

#include <cstddef>
#include <format>
#include <span>
#include <stdexcept>
#include <vector>

#include <libhat/scanner.hpp>
#include <libhat/signature.hpp>
#include <libhat/process.hpp>

namespace spyglass {

void *find(const std::string_view pattern)
{
    const auto signature = hat::parse_signature(pattern);
    if (!signature.has_value()) {
        throw std::runtime_error{"malformed pattern"};
    }

    const std::span<const std::byte> text = hat::process::get_process_module().get_section_data(".text");

    std::vector<const std::byte *> matches;
    for (const auto match : hat::find_all_pattern(text.begin(), text.end(), signature.value())) {
        matches.push_back(match.get());
    }

    if (matches.empty()) {
        throw std::runtime_error{"pattern not found, this client build is unsupported"};
    }
    if (matches.size() > 1) {
        throw std::runtime_error{std::format("pattern matches {} places, refusing to guess", matches.size())};
    }
    return const_cast<std::byte *>(matches.front());
}

void *find_within(const void *begin, const std::size_t size, const std::string_view pattern)
{
    const auto signature = hat::parse_signature(pattern);
    if (!signature.has_value()) {
        throw std::runtime_error{"malformed pattern"};
    }

    const std::span<const std::byte> region{static_cast<const std::byte *>(begin), size};
    std::vector<const std::byte *> matches;
    for (const auto match : hat::find_all_pattern(region.begin(), region.end(), signature.value())) {
        matches.push_back(match.get());
    }

    if (matches.size() != 1) {
        throw std::runtime_error{std::format("pattern matches {} places in the function, expected one", matches.size())};
    }
    return const_cast<std::byte *>(matches.front());
}

}  // namespace spyglass
