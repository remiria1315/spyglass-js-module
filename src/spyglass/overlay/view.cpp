#include "spyglass/overlay/view.h"

#include <algorithm>
#include <cstdio>
#include <ranges>
#include <utility>

#include <imgui.h>

#include "spyglass/error.h"
#include "spyglass/overlay/i18n.h"
#include "spyglass/overlay/pane/expert_window.h"
#include "spyglass/overlay/pane/export_dialog.h"
#include "spyglass/overlay/pane/filter_window.h"
#include "spyglass/overlay/pane/menu_bar.h"
#include "spyglass/overlay/pane/modules_window.h"
#include "spyglass/overlay/pane/packet_bytes.h"
#include "spyglass/overlay/pane/packet_details.h"
#include "spyglass/overlay/pane/packet_list.h"
#include "spyglass/overlay/pane/settings_window.h"
#include "spyglass/overlay/pane/statistics_window.h"
#include "spyglass/overlay/pane/status_bar.h"
#include "spyglass/overlay/pane/toolbar.h"
#include "spyglass/overlay/settings.h"
#include "spyglass/overlay/theme.h"
#include "spyglass/error.h"
#include "spyglass/reflect.h"
#include "spyglass/script/engine.h"

namespace spyglass {
namespace {

constexpr float kSplitterHeight = 6.0F;
constexpr float kPreferredMinimum = 60.0F;

void splitter(const char *id, float &share, const float usable)
{
    ImGui::InvisibleButton(id, ImVec2{-1.0F, kSplitterHeight});
    if (ImGui::IsItemActive() && usable > 0.0F) {
        share += ImGui::GetIO().MouseDelta.y / usable;
    }
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    }
}

}  // namespace

View::View() : saved_{read_settings()}
{
    options_.language = saved_.language;
    options_.theme = saved_.theme;
    options_.font_scale = saved_.zoom;
    options_.time_format = saved_.time_format;
    options_.details_pane = saved_.details_pane;
    options_.bytes_pane = saved_.bytes_pane;
    options_.colorize = saved_.colorize;
    options_.auto_scroll = saved_.auto_scroll;
    capture_.set_options(saved_.capture);
}

View &View::getInstance()
{
    static View view;
    return view;
}

bool View::wants_fields(const int id, const Direction direction) const
{
    return decode_mode(id) == DecodeMode::Eager && capture_.accepts(direction);
}

void View::onPacketSend(Incoming incoming)
{
    capture_.record(std::move(incoming), Direction::Outbound);
}

void View::onPacketReceive(Incoming incoming)
{
    capture_.record(std::move(incoming), Direction::Inbound);
}

void View::draw()
{
    set_language(options_.language);
    advance_export(export_dialog_.job, capture_);

    script::tick();
    script::render();

    const auto &io = ImGui::GetIO();
    if (visible_ && io.KeyCtrl && !io.KeyAlt) {
        if (ImGui::IsKeyPressed(ImGuiKey_Equal) || ImGui::IsKeyPressed(ImGuiKey_KeypadAdd)) {
            step_zoom(options_, 1);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Minus) || ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract)) {
            step_zoom(options_, -1);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_0, false) || ImGui::IsKeyPressed(ImGuiKey_Keypad0, false)) {
            options_.font_scale = 1.0F;
        }
    }

    const Settings current{
        .language = options_.language,
        .theme = options_.theme,
        .zoom = options_.font_scale,
        .time_format = options_.time_format,
        .details_pane = options_.details_pane,
        .bytes_pane = options_.bytes_pane,
        .colorize = options_.colorize,
        .auto_scroll = options_.auto_scroll,
        .capture = capture_.options(),
    };
    if (current != saved_) {
        saved_ = current;
        unsaved_ = true;
        changed_at_ = ImGui::GetTime();
    }
    else if (unsaved_ && ImGui::GetTime() - changed_at_ > 0.5) {
        write_settings(saved_);
        unsaved_ = false;
    }

    const ThemeScope theme{options_.theme, options_.font_scale};
    if (script::take_attention()) {
        options_.modules_window = true;
        modules_window_.touched = ImGui::GetTime();
    }
    if (options_.modules_window) {
        if (!visible_ && ImGui::GetTime() - modules_window_.touched > 8.0) {
            options_.modules_window = false;
        }
        else {
            draw_modules_window(modules_window_, options_.modules_window, !visible_);
        }
    }

    if (!visible_) {
        return;
    }

    if (const auto failures = errors(); !failures.empty() && options_.errors_window) {
        ImGui::SetNextWindowSize(ImVec2{520.0F, 0.0F}, ImGuiCond_FirstUseEver);
        if (ImGui::Begin(tr_label("Spyglass: errors"), &options_.errors_window)) {
            for (const auto &message : failures) {
                ImGui::TextColored(palette().bad_packet, "%s", message.c_str());
            }
        }
        ImGui::End();
    }

    if (options_.filter_window && interactive_) {
        draw_filter_window(capture_, filter_, filter_window_, options_.filter_window);
    }

    if (options_.settings_window) {
        draw_settings_window(capture_, options_);
    }

    if (options_.about_window) {
        draw_about_window(options_.about_window);
    }

    if (options_.expert_window) {
        draw_expert_window(capture_, list_, expert_window_, options_.expert_window);
    }

    if (options_.statistics_window) {
        draw_statistics_window(capture_, options_);
    }

    if (options_.export_dialog || export_dialog_.job.running || !export_dialog_.job.message.empty()) {
        draw_export_dialog(capture_, filter_, list_, bytes_view_, export_dialog_, options_);
    }

    ImGui::SetNextWindowSize(ImVec2{960.0F, 720.0F}, ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Spyglass", &visible_,
                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_MenuBar |
                         ImGuiWindowFlags_NoBringToFrontOnFocus)) {
        draw_menu_bar(capture_, filter_, list_, options_);
        draw_toolbar(capture_, filter_, options_.filter_window);
        if (options_.find_bar) {
            draw_find_bar(capture_, filter_, list_, options_);
        }
        ImGui::Separator();

        const float footer = (2.0F * ImGui::GetStyle().ItemSpacing.y) + ImGui::GetTextLineHeight();
        ImGui::BeginChild("panes", ImVec2{0.0F, -footer}, ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);

        const float pane_count = 1.0F + (options_.details_pane ? 1.0F : 0.0F) + (options_.bytes_pane ? 1.0F : 0.0F);
        const float splitter_count = pane_count - 1.0F;
        const float usable =
            std::max(0.0F, ImGui::GetContentRegionAvail().y - (splitter_count * kSplitterHeight) -
                               ((pane_count + splitter_count - 1.0F) * ImGui::GetStyle().ItemSpacing.y));
        const float smallest = std::min(kPreferredMinimum, usable / pane_count);

        const auto selected = capture_.selected_details();
        const auto *const record = selected ? &*selected : nullptr;
        const auto number = capture_.selected();

        if (splitter_count == 0.0F) {
            draw_packet_list(capture_, filter_, list_, options_, usable);
        }
        else {
            const float list_height = std::clamp(usable * list_share_, smallest, usable - (splitter_count * smallest));
            if (usable > 0.0F) {
                list_share_ = list_height / usable;
            }
            draw_packet_list(capture_, filter_, list_, options_, list_height);
            splitter("list_splitter", list_share_, usable);

            if (options_.details_pane && options_.bytes_pane) {
                const float details = std::clamp(usable * details_share_, smallest, usable - list_height - smallest);
                if (usable > 0.0F) {
                    details_share_ = details / usable;
                }
                draw_packet_details(capture_, record, options_, details);
                splitter("details_splitter", details_share_, usable);
                draw_packet_bytes(record, number, bytes_view_, options_, usable - list_height - details);
            }
            else if (options_.details_pane) {
                draw_packet_details(capture_, record, options_, usable - list_height);
            }
            else {
                draw_packet_bytes(record, number, bytes_view_, options_, usable - list_height);
            }
        }

        ImGui::EndChild();

        ImGui::Separator();
        draw_status_bar(capture_, list_, bytes_view_);
    }
    ImGui::End();

    if (options_.detach != 0) {
        const auto wanted = options_.detach;
        const auto already =
            std::ranges::any_of(detached_, [wanted](const DetachedPacket &open) { return open.number == wanted; });
        if (!already) {
            detached_.push_back({.number = wanted});
        }
        options_.detach = 0;
    }
    for (auto &open : detached_) {
        const auto number = static_cast<unsigned long long>(open.number);
        char title[96];
        const auto length = std::snprintf(title, sizeof(title), tr("Spyglass: packet %llu"), number);
        std::snprintf(title + length, sizeof(title) - static_cast<std::size_t>(length), "###packet%llu", number);
        ImGui::SetNextWindowSize(ImVec2{640.0F, 520.0F}, ImGuiCond_FirstUseEver);
        if (ImGui::Begin(title, &open.open)) {
            const auto pinned = capture_.details(open.number);
            const auto *const record = pinned.record.number == 0 ? nullptr : &pinned;
            draw_packet_details(capture_, record, options_, ImGui::GetContentRegionAvail().y * 0.4F);
            draw_packet_bytes(record, open.number, open.bytes, options_, ImGui::GetContentRegionAvail().y);
        }
        ImGui::End();
    }
    std::erase_if(detached_, [](const DetachedPacket &open) { return !open.open; });

    options_.expand_details = false;
    options_.collapse_details = false;
}

}  // namespace spyglass
