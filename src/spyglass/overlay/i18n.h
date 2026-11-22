#pragma once

namespace spyglass {

enum class Language : int {
    English = 0,
    Japanese = 1,
};

void set_language(Language language);

[[nodiscard]] Language language();

[[nodiscard]] const char *tr(const char *english);

[[nodiscard]] const char *tr_label(const char *english);

} // namespace spyglass
