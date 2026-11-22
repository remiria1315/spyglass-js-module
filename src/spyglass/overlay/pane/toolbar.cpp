#include "spyglass/overlay/pane/toolbar.h"
#include "spyglass/overlay/i18n.h"

#include <algorithm>

#include <imgui.h>

#include "spyglass/overlay/capture.h"
#include "spyglass/overlay/filter.h"
#include "spyglass/overlay/theme.h"

namespace spyglass {

void draw_toolbar(Capture &capture, const Filter &filter, bool &filter_open)
{
    ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);

    ImGui::BeginDisabled(capture.running());
    if (ImGui::Button(tr_label("Start"))) {
        capture.start();
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(!capture.running());
    if (ImGui::Button(tr_label("Stop"))) {
        capture.stop();
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    if (ImGui::Button(tr_label("Restart"))) {
        capture.restart();
    }

    ImGui::PopItemFlag();

    const auto active = filter.active();
    const auto *const label = tr_label(active ? "Filter *" : "Filter");
    const float width = ImGui::CalcTextSize(label, nullptr, true).x + (2.0F * ImGui::GetStyle().FramePadding.x);
    const float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;

    ImGui::SameLine();
    ImGui::SameLine(std::max(ImGui::GetCursorPosX(), right - width));
    if (active) {
        ImGui::PushStyleColor(ImGuiCol_Button, palette().filter_active);
    }
    if (ImGui::Button(label)) {
        filter_open = !filter_open;
    }
    if (active) {
        ImGui::PopStyleColor();
    }
}

}  // namespace spyglass
