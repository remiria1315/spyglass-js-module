#include "spyglass/overlay/pane/modules_window.h"
#include "spyglass/overlay/i18n.h"

#include <cstddef>
#include <format>
#include <string>

#include <imgui.h>

#include "spyglass/core/output.h"
#include "spyglass/overlay/theme.h"

namespace spyglass {

void draw_modules_window(ModulesWindow &window, bool &open, const bool passive)
{
    const auto now = ImGui::GetTime();
    if (const auto serial = script::log_serial(); serial != window.seen) {
        window.seen = serial;
        window.lines = script::log_lines();
        window.touched = now;
        window.refreshed = -1.0;
    }
    if (now - window.refreshed > 1.0) {
        window.modules = script::modules();
        window.refreshed = now;
    }

    ImGui::SetNextWindowSize(ImVec2{560.0F, 360.0F}, ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(tr_label("Spyglass: modules"), &open,
                      passive ? ImGuiWindowFlags_NoInputs : ImGuiWindowFlags_None)) {
        ImGui::End();
        return;
    }

    if (ImGui::Button(tr_label("Open Folder"))) {
        script::execute("folder");
    }
    ImGui::SameLine();
    ImGui::TextColored(palette().muted, "%s", path_text(script::modules_directory()).c_str());

    constexpr auto flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("modules", 3, flags)) {
        ImGui::TableSetupColumn(tr_label("Module"), ImGuiTableColumnFlags_WidthStretch, 2.0F);
        ImGui::TableSetupColumn(tr_label("State"), ImGuiTableColumnFlags_WidthStretch, 3.0F);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableHeadersRow();

        int index = 0;
        for (const auto &module : window.modules) {
            ImGui::TableNextRow();
            ImGui::PushID(index++);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(module.name.c_str());
            ImGui::TableNextColumn();
            if (!module.error.empty()) {
                ImGui::TextColored(palette().bad_packet, "%s", module.error.substr(0, module.error.find('\n')).c_str());
            }
            else {
                ImGui::TextColored(
                    module.loaded ? palette().filter_active : palette().muted, "%s",
                    tr(module.loaded ? "enabled" : (module.enabled ? "enabled, not loaded" : "disabled")));
            }
            ImGui::TableNextColumn();
            if (ImGui::SmallButton(tr_label(module.enabled ? "Disable" : "Enable"))) {
                script::execute(std::format("{} {}", module.enabled ? "disable" : "enable", module.name));
            }
            if (module.enabled) {
                ImGui::SameLine();
                if (ImGui::SmallButton(tr_label("Reload"))) {
                    script::execute(std::format("reload {}", module.name));
                }
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    if (ImGui::BeginChild("log", ImVec2{0.0F, 0.0F}, ImGuiChildFlags_Borders)) {
        for (const auto &line : window.lines) {
            const auto text = line.module.empty() ? line.text : std::format("[{}] {}", line.module, line.text);
            if (line.error) {
                ImGui::TextColored(palette().bad_packet, "%s", text.c_str());
            }
            else {
                ImGui::TextUnformatted(text.c_str());
            }
        }
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0F) {
            ImGui::SetScrollHereY(1.0F);
        }
    }
    ImGui::EndChild();
    ImGui::End();
}

}  // namespace spyglass
