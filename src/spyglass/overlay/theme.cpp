#include "spyglass/overlay/theme.h"

#include <array>
#include <cstddef>

namespace spyglass {
namespace {

struct ColorEntry {
    ImGuiCol index;
    ImVec4 dark;
    ImVec4 light;
};

struct PaddingEntry {
    ImGuiStyleVar index;
    float x;
    float y;
};

struct SizeEntry {
    ImGuiStyleVar index;
    float value;
};

constexpr ImVec4 rgb(const unsigned hex, const float alpha = 1.0F)
{
    return ImVec4{static_cast<float>((hex >> 16U) & 0xFFU) / 255.0F, static_cast<float>((hex >> 8U) & 0xFFU) / 255.0F,
                  static_cast<float>(hex & 0xFFU) / 255.0F, alpha};
}

constexpr unsigned kDarkAccent = 0x4C8DFF;
constexpr unsigned kLightAccent = 0x2F6FED;

constexpr std::array kColors = std::to_array<ColorEntry>({
    {ImGuiCol_Text, rgb(0xE6E8EC), rgb(0x1D2127)},
    {ImGuiCol_TextDisabled, rgb(0x7B8290), rgb(0x8A919E)},
    {ImGuiCol_WindowBg, rgb(0x121418), rgb(0xF4F5F7)},
    {ImGuiCol_ChildBg, rgb(0x121418, 0.0F), rgb(0xF4F5F7, 0.0F)},
    {ImGuiCol_PopupBg, rgb(0x1A1D24), rgb(0xFFFFFF)},
    {ImGuiCol_Border, rgb(0x2A2F39), rgb(0xD5D9E0)},
    {ImGuiCol_BorderShadow, rgb(0x000000, 0.0F), rgb(0x000000, 0.0F)},
    {ImGuiCol_FrameBg, rgb(0x1E222A), rgb(0xFFFFFF)},
    {ImGuiCol_FrameBgHovered, rgb(0x272C36), rgb(0xEEF1F6)},
    {ImGuiCol_FrameBgActive, rgb(0x303642), rgb(0xE4E9F2)},
    {ImGuiCol_TitleBg, rgb(0x15181D), rgb(0xE9ECF2)},
    {ImGuiCol_TitleBgActive, rgb(0x1B1F26), rgb(0xDFE4EC)},
    {ImGuiCol_TitleBgCollapsed, rgb(0x15181D), rgb(0xE9ECF2)},
    {ImGuiCol_MenuBarBg, rgb(0x171A20), rgb(0xECEEF3)},
    {ImGuiCol_ScrollbarBg, rgb(0x121418, 0.0F), rgb(0xF4F5F7, 0.0F)},
    {ImGuiCol_ScrollbarGrab, rgb(0x343A46), rgb(0xC3C9D4)},
    {ImGuiCol_ScrollbarGrabHovered, rgb(0x444C5B), rgb(0xAAB2C1)},
    {ImGuiCol_ScrollbarGrabActive, rgb(0x566074), rgb(0x8F99AB)},
    {ImGuiCol_CheckMark, rgb(kDarkAccent), rgb(kLightAccent)},
    {ImGuiCol_SliderGrab, rgb(kDarkAccent), rgb(kLightAccent)},
    {ImGuiCol_SliderGrabActive, rgb(0x6EA1FF), rgb(0x2459C9)},
    {ImGuiCol_Button, rgb(0x262B35), rgb(0xE8EBF1)},
    {ImGuiCol_ButtonHovered, rgb(0x313846), rgb(0xDDE2EB)},
    {ImGuiCol_ButtonActive, rgb(0x3B4455), rgb(0xD0D7E3)},
    {ImGuiCol_Header, rgb(kDarkAccent, 0.22F), rgb(kLightAccent, 0.18F)},
    {ImGuiCol_HeaderHovered, rgb(kDarkAccent, 0.36F), rgb(kLightAccent, 0.28F)},
    {ImGuiCol_HeaderActive, rgb(kDarkAccent, 0.52F), rgb(kLightAccent, 0.40F)},
    {ImGuiCol_Separator, rgb(0x2A2F39), rgb(0xD5D9E0)},
    {ImGuiCol_SeparatorHovered, rgb(kDarkAccent, 0.78F), rgb(kLightAccent, 0.78F)},
    {ImGuiCol_SeparatorActive, rgb(kDarkAccent), rgb(kLightAccent)},
    {ImGuiCol_ResizeGrip, rgb(kDarkAccent, 0.20F), rgb(kLightAccent, 0.20F)},
    {ImGuiCol_ResizeGripHovered, rgb(kDarkAccent, 0.67F), rgb(kLightAccent, 0.67F)},
    {ImGuiCol_ResizeGripActive, rgb(kDarkAccent, 0.95F), rgb(kLightAccent, 0.95F)},
    {ImGuiCol_InputTextCursor, rgb(0xE6E8EC), rgb(0x1D2127)},
    {ImGuiCol_Tab, rgb(0x1B1F26), rgb(0xE6E9EF)},
    {ImGuiCol_TabHovered, rgb(kDarkAccent, 0.55F), rgb(kLightAccent, 0.45F)},
    {ImGuiCol_TabSelected, rgb(0x2A3140), rgb(0xFFFFFF)},
    {ImGuiCol_TabSelectedOverline, rgb(kDarkAccent), rgb(kLightAccent)},
    {ImGuiCol_TabDimmed, rgb(0x171A20), rgb(0xECEEF3)},
    {ImGuiCol_TabDimmedSelected, rgb(0x222733), rgb(0xF7F8FA)},
    {ImGuiCol_TabDimmedSelectedOverline, rgb(kDarkAccent, 0.5F), rgb(kLightAccent, 0.5F)},
    {ImGuiCol_PlotLines, rgb(kDarkAccent), rgb(kLightAccent)},
    {ImGuiCol_PlotLinesHovered, rgb(0x6EA1FF), rgb(0x2459C9)},
    {ImGuiCol_PlotHistogram, rgb(kDarkAccent), rgb(kLightAccent)},
    {ImGuiCol_PlotHistogramHovered, rgb(0x6EA1FF), rgb(0x2459C9)},
    {ImGuiCol_TableHeaderBg, rgb(0x1B1F26), rgb(0xE9ECF2)},
    {ImGuiCol_TableBorderStrong, rgb(0x2A2F39), rgb(0xD5D9E0)},
    {ImGuiCol_TableBorderLight, rgb(0x22262E), rgb(0xE1E5EC)},
    {ImGuiCol_TableRowBg, rgb(0x000000, 0.0F), rgb(0x000000, 0.0F)},
    {ImGuiCol_TableRowBgAlt, rgb(0xFFFFFF, 0.03F), rgb(0x000000, 0.03F)},
    {ImGuiCol_TextLink, rgb(0x6EA1FF), rgb(0x2459C9)},
    {ImGuiCol_TextSelectedBg, rgb(kDarkAccent, 0.35F), rgb(kLightAccent, 0.30F)},
    {ImGuiCol_DragDropTarget, rgb(kDarkAccent), rgb(kLightAccent)},
    {ImGuiCol_NavCursor, rgb(kDarkAccent), rgb(kLightAccent)},
    {ImGuiCol_ModalWindowDimBg, rgb(0x000000, 0.5F), rgb(0x000000, 0.3F)},
});

constexpr std::array kPaddings = std::to_array<PaddingEntry>({
    {ImGuiStyleVar_WindowPadding, 10.0F, 8.0F},
    {ImGuiStyleVar_FramePadding, 8.0F, 4.0F},
    {ImGuiStyleVar_ItemSpacing, 8.0F, 5.0F},
    {ImGuiStyleVar_ItemInnerSpacing, 6.0F, 4.0F},
    {ImGuiStyleVar_CellPadding, 6.0F, 3.0F},
});

constexpr std::array kSizes = std::to_array<SizeEntry>({
    {ImGuiStyleVar_WindowRounding, 10.0F},
    {ImGuiStyleVar_ChildRounding, 8.0F},
    {ImGuiStyleVar_FrameRounding, 6.0F},
    {ImGuiStyleVar_PopupRounding, 8.0F},
    {ImGuiStyleVar_ScrollbarRounding, 10.0F},
    {ImGuiStyleVar_GrabRounding, 6.0F},
    {ImGuiStyleVar_TabRounding, 6.0F},
    {ImGuiStyleVar_ScrollbarSize, 12.0F},
    {ImGuiStyleVar_GrabMinSize, 10.0F},
    {ImGuiStyleVar_IndentSpacing, 18.0F},
});

constexpr std::array kBorders = std::to_array<SizeEntry>({
    {ImGuiStyleVar_WindowBorderSize, 1.0F},
    {ImGuiStyleVar_ChildBorderSize, 0.0F},
    {ImGuiStyleVar_PopupBorderSize, 1.0F},
    {ImGuiStyleVar_FrameBorderSize, 0.0F},
    {ImGuiStyleVar_TabBarOverlineSize, 2.0F},
});

constexpr Palette kDarkPalette{
    .muted = ImVec4{0.62F, 0.64F, 0.68F, 1.0F},
    .bad_packet = ImVec4{0.94F, 0.45F, 0.40F, 1.0F},
    .bad_row = ImVec4{0.45F, 0.13F, 0.13F, 0.55F},
    .find_hit = ImVec4{0.95F, 0.76F, 0.24F, 1.0F},
    .filter_active = ImVec4{0.18F, 0.38F, 0.24F, 1.0F},
    .marked_row = ImVec4{0.20F, 0.26F, 0.46F, 0.75F},
};

constexpr Palette kLightPalette{
    .muted = ImVec4{0.42F, 0.45F, 0.50F, 1.0F},
    .bad_packet = ImVec4{0.78F, 0.18F, 0.15F, 1.0F},
    .bad_row = ImVec4{0.98F, 0.80F, 0.78F, 0.85F},
    .find_hit = ImVec4{0.80F, 0.55F, 0.0F, 1.0F},
    .filter_active = ImVec4{0.72F, 0.90F, 0.76F, 1.0F},
    .marked_row = ImVec4{0.74F, 0.82F, 0.98F, 0.85F},
};

Palette g_palette = kDarkPalette;

} // namespace

const Palette &palette()
{
    return g_palette;
}

ThemeScope::ThemeScope(const Theme theme, const float zoom)
{
    const auto dark = theme == Theme::Dark;
    g_palette = dark ? kDarkPalette : kLightPalette;

    for (const auto &entry : kColors) {
        ImGui::PushStyleColor(entry.index, dark ? entry.dark : entry.light);
    }

    const auto scale = zoom * ImGui::GetStyle().FontScaleDpi;
    for (const auto &entry : kPaddings) {
        ImGui::PushStyleVar(entry.index, ImVec2{entry.x * scale, entry.y * scale});
    }
    for (const auto &entry : kSizes) {
        ImGui::PushStyleVar(entry.index, entry.value * scale);
    }
    for (const auto &entry : kBorders) {
        ImGui::PushStyleVar(entry.index, entry.value);
    }

    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * zoom);
}

ThemeScope::~ThemeScope()
{
    ImGui::PopFont();
    ImGui::PopStyleVar(static_cast<int>(kPaddings.size() + kSizes.size() + kBorders.size()));
    ImGui::PopStyleColor(static_cast<int>(kColors.size()));
}

} // namespace spyglass
