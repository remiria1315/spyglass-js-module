#include "spyglass/overlay/pane/settings_window.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

#include <imgui.h>

#include "spyglass/overlay/capture.h"
#include "spyglass/overlay/i18n.h"
#include "spyglass/overlay/options.h"
#include "spyglass/overlay/theme.h"

namespace spyglass {

void draw_settings_window(Capture &capture, ViewOptions &options)
{
    if (ImGui::Begin(tr_label("Spyglass: settings"), &options.settings_window, ImGuiWindowFlags_AlwaysAutoResize)) {
        const auto width = ImGui::GetFontSize() * 14.0F;
        const auto &muted = palette().muted;

        ImGui::SeparatorText(tr("General"));
        auto language = static_cast<int>(options.language);
        ImGui::SetNextItemWidth(width);
        if (ImGui::Combo(tr_label("Language"), &language, "English\0日本語\0")) {
            options.language = static_cast<Language>(language);
        }

        ImGui::SeparatorText(tr("Appearance"));
        constexpr std::array<const char *, 2> kThemeNames{"Dark", "Light"};
        ImGui::SetNextItemWidth(width);
        if (ImGui::BeginCombo(tr_label("Theme"), tr(kThemeNames[static_cast<std::size_t>(options.theme)]))) {
            for (std::size_t index = 0; index < kThemeNames.size(); ++index) {
                const auto theme = static_cast<Theme>(index);
                if (ImGui::Selectable(tr_label(kThemeNames[index]), options.theme == theme)) {
                    options.theme = theme;
                }
            }
            ImGui::EndCombo();
        }

        auto percent = options.font_scale * 100.0F;
        ImGui::SetNextItemWidth(width);
        if (ImGui::SliderFloat(tr_label("Zoom"), &percent, kSmallestZoom * 100.0F, kLargestZoom * 100.0F, "%.0f%%",
                               ImGuiSliderFlags_AlwaysClamp)) {
            options.font_scale = std::round(percent / 10.0F) / 10.0F;
        }
        ImGui::SameLine();
        if (ImGui::Button(tr_label("Reset zoom"))) {
            options.font_scale = 1.0F;
        }
        ImGui::TextColored(muted, "%s", tr("Ctrl++ and Ctrl+- change the zoom, Ctrl+0 resets it."));

        ImGui::SeparatorText(tr("Packet list"));
        ImGui::SetNextItemWidth(width * 1.6F);
        if (ImGui::BeginCombo(tr_label("Time display format"),
                              tr(kTimeFormatNames[static_cast<std::size_t>(options.time_format)]))) {
            for (std::size_t index = 0; index < kTimeFormatNames.size(); ++index) {
                const auto format = static_cast<TimeFormat>(index);
                if (ImGui::Selectable(tr_label(kTimeFormatNames[index]), options.time_format == format)) {
                    options.time_format = format;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::Checkbox(tr_label("Colorize Packet List"), &options.colorize);
        ImGui::Checkbox(tr_label("Auto Scroll"), &options.auto_scroll);
        ImGui::Checkbox(tr_label("Packet Details"), &options.details_pane);
        ImGui::Checkbox(tr_label("Packet Bytes"), &options.bytes_pane);

        ImGui::SeparatorText(tr("Capture"));
        constexpr std::size_t kBlockBytes = kBlockEntries * sizeof(Entry);
        auto settings = capture.options();
        auto index = static_cast<int>((settings.store.resident_blocks * kBlockBytes) / (1024 * 1024));
        auto queue = static_cast<int>(settings.store.queue_bytes / (1024 * 1024));

        ImGui::TextColored(muted, "%s",
                           tr("Packets stream to a capture file and are read back "
                              "as they are shown, so\n"
                              "nothing is dropped for age and memory does not grow "
                              "with the session."));
        ImGui::TextColored(muted, "%s", capture.store().path().string().c_str());

        ImGui::SetNextItemWidth(width);
        ImGui::InputInt(tr_label("Index cache (MB)"), &index, 16, 128);
        ImGui::SetNextItemWidth(width);
        ImGui::InputInt(tr_label("Writer queue (MB)"), &queue, 1, 8);
        ImGui::TextColored(muted, "%s",
                           tr("The queue holds packets the writer has not reached "
                              "yet. A packet that\n"
                              "arrives when it is full is dropped and counted in "
                              "the status bar."));

        ImGui::Checkbox(tr_label("Capture sent packets"), &settings.outbound);
        ImGui::TextColored(muted, "%s", tr("Sent packets are never stored while this is off."));

        settings.store.resident_blocks =
            std::max<std::size_t>(2, (static_cast<std::size_t>(std::max(index, 1)) * 1024 * 1024) / kBlockBytes);
        settings.store.queue_bytes = static_cast<std::size_t>(std::max(queue, 1)) * 1024 * 1024;
        capture.set_options(settings);
    }
    ImGui::End();
}

} // namespace spyglass
