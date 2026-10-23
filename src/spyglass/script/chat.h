#pragma once

#include <optional>
#include <string_view>

namespace spyglass::script {

std::optional<std::string_view> chat_command(std::string_view body);

}  // namespace spyglass::script
