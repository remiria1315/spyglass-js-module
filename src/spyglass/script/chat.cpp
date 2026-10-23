#include "spyglass/script/chat.h"

#include <cstddef>
#include <cstdint>

namespace spyglass::script {

std::optional<std::string_view> chat_command(const std::string_view body)
{
    constexpr std::string_view kPrefix = "!!module";

    for (auto start = body.find(kPrefix); start != std::string_view::npos; start = body.find(kPrefix, start + 1)) {
        for (std::size_t width = 1; width <= 3 && width <= start; ++width) {
            std::uint32_t length = 0;
            auto valid = true;
            for (std::size_t i = 0; i < width && valid; ++i) {
                const auto byte = static_cast<std::uint8_t>(body[start - width + i]);
                const auto last = i + 1 == width;
                valid = ((byte & 0x80U) != 0) != last;
                length |= static_cast<std::uint32_t>(byte & 0x7FU) << (7 * i);
            }
            if (valid && length >= kPrefix.size() && length <= body.size() - start) {
                return body.substr(start, length);
            }
        }
    }
    return std::nullopt;
}

}  // namespace spyglass::script
