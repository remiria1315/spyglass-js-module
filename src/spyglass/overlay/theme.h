#pragma once

#include <imgui.h>

namespace spyglass {

enum class Theme : int {
    Dark = 0,
    Light = 1,
};

struct Palette {
    ImVec4 muted;
    ImVec4 bad_packet;
    ImVec4 bad_row;
    ImVec4 find_hit;
    ImVec4 filter_active;
    ImVec4 marked_row;
};

[[nodiscard]] const Palette &palette();

class ThemeScope {
  public:
    ThemeScope(Theme theme, float zoom);
    ~ThemeScope();

    ThemeScope(const ThemeScope &) = delete;
    ThemeScope &operator=(const ThemeScope &) = delete;
};

}  // namespace spyglass
