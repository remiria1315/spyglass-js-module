#include "spyglass/overlay/i18n.h"

#include <array>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace spyglass {
namespace {

struct TextHash {
    using is_transparent = void;

    std::size_t operator()(const std::string_view text) const noexcept
    {
        return std::hash<std::string_view>{}(text);
    }
};

const std::unordered_map<std::string_view, const char *> g_japanese{
#include "spyglass/overlay/ja/common.inc"
#include "spyglass/overlay/ja/expert_window.inc"
#include "spyglass/overlay/ja/export_dialog.inc"
#include "spyglass/overlay/ja/filter_window.inc"
#include "spyglass/overlay/ja/menu_bar.inc"
#include "spyglass/overlay/ja/modules_window.inc"
#include "spyglass/overlay/ja/packet_bytes.inc"
#include "spyglass/overlay/ja/packet_details.inc"
#include "spyglass/overlay/ja/packet_list.inc"
#include "spyglass/overlay/ja/settings_window.inc"
#include "spyglass/overlay/ja/statistics_window.inc"
#include "spyglass/overlay/ja/status_bar.inc"
#include "spyglass/overlay/ja/toolbar.inc"
#include "spyglass/overlay/ja/view.inc"
};

Language g_language = Language::English;
std::array<std::unordered_map<std::string, std::string, TextHash, std::equal_to<>>, 2> g_labels;

} // namespace

void set_language(const Language language)
{
    g_language = language;
}

Language language()
{
    return g_language;
}

const char *tr(const char *english)
{
    if (g_language == Language::English) {
        return english;
    }
    const auto found = g_japanese.find(std::string_view{english});
    return found == g_japanese.end() ? english : found->second;
}

const char *tr_label(const char *english)
{
    auto &labels = g_labels[static_cast<std::size_t>(g_language)];
    if (const auto cached = labels.find(std::string_view{english}); cached != labels.end()) {
        return cached->second.c_str();
    }
    return labels.emplace(english, std::string{tr(english)} + "###" + english).first->second.c_str();
}

} // namespace spyglass
