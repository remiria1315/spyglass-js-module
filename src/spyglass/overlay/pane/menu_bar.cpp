#include "spyglass/overlay/pane/menu_bar.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>

#include <imgui.h>

#include "spyglass/network.h"
#include "spyglass/overlay/capture.h"
#include "spyglass/overlay/export.h"
#include "spyglass/overlay/filter.h"
#include "spyglass/overlay/i18n.h"
#include "spyglass/overlay/navigate.h"
#include "spyglass/overlay/options.h"
#include "spyglass/overlay/pane/packet_list.h"
#include "spyglass/overlay/report.h"
#include "spyglass/overlay/theme.h"
#include "spyglass/error.h"
#include "spyglass/signature.h"

namespace spyglass {
namespace {

constexpr std::array<std::pair<const char *, StatisticsTab>, 4> kStatisticsTabs{{
    {"Capture Properties", StatisticsTab::Properties},
    {"Packet Types", StatisticsTab::Types},
    {"Packet Lengths", StatisticsTab::Lengths},
    {"I/O Graph", StatisticsTab::Graph},
}};

void time_format(ViewOptions &options, const TimeFormat format)
{
    if (ImGui::MenuItem(tr_label(kTimeFormatNames[static_cast<std::size_t>(format)]), nullptr,
                        options.time_format == format)) {
        options.time_format = format;
    }
}

}  // namespace

void draw_menu_bar(Capture &capture, Filter &filter, PacketList &list, ViewOptions &options)
{
    if (!ImGui::BeginMenuBar()) {
        return;
    }

    auto open_goto = false;

    if (ImGui::BeginMenu(tr_label("File"))) {
        if (ImGui::MenuItem(tr_label("Export Packets..."))) {
            options.export_dialog = true;
            options.export_command = ExportCommand::Packets;
        }
        if (ImGui::MenuItem(tr_label("Export Selected Packet Bytes..."), nullptr, false, capture.selected() != 0)) {
            options.export_dialog = true;
            options.export_command = ExportCommand::SelectedBytes;
        }
        ImGui::Separator();
        if (ImGui::MenuItem(tr_label("Export Session Summary..."))) {
            options.export_dialog = true;
            options.export_command = ExportCommand::Summary;
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu(tr_label("Edit"))) {
        ImGui::MenuItem(tr_label("Find Packet"), nullptr, &options.find_bar);

        ImGui::Separator();
        const auto selected = capture.selected();
        const auto marked = list.marks.contains(selected);
        if (ImGui::MenuItem(tr_label(marked ? "Unmark Packet" : "Mark Packet"), nullptr, false, selected != 0)) {
            if (marked) {
                list.marks.erase(selected);
            }
            else {
                list.marks.insert(selected);
            }
        }
        if (ImGui::MenuItem(tr_label("Mark All Displayed"))) {
            capture.visit(0, [&](const Record &record) {
                if (filter.matches(record)) {
                    list.marks.insert(record.number);
                }
                return true;
            });
        }
        if (ImGui::MenuItem(tr_label("Unmark All"), nullptr, false, !list.marks.empty())) {
            list.marks.clear();
        }
        if (ImGui::MenuItem(tr_label("Next Mark"), nullptr, false, !list.marks.empty())) {
            jump(capture, filter, list, Jump::NextMark);
        }
        if (ImGui::MenuItem(tr_label("Previous Mark"), nullptr, false, !list.marks.empty())) {
            jump(capture, filter, list, Jump::PreviousMark);
        }

        ImGui::Separator();
        const auto referenced = list.reference == selected;
        if (ImGui::MenuItem(tr_label(referenced ? "Unset Time Reference" : "Set Time Reference"), nullptr, false,
                            selected != 0)) {
            list.reference = referenced ? 0 : selected;
        }
        if (ImGui::MenuItem(tr_label("Unset All Time References"), nullptr, false, list.reference != 0)) {
            list.reference = 0;
        }

        ImGui::Separator();
        if (ImGui::BeginMenu(tr_label("Copy"))) {
            if (ImGui::MenuItem(tr_label("Row"), nullptr, false, selected != 0)) {
                if (const auto selection = capture.selected_details()) {
                    ImGui::SetClipboardText(report_row(selection->record).c_str());
                }
            }
            if (ImGui::MenuItem(tr_label("Packet Details"), nullptr, false, selected != 0)) {
                if (const auto selection = capture.selected_details()) {
                    ImGui::SetClipboardText(report_details(*selection, true).c_str());
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem(tr_label("All Displayed Rows"))) {
                std::string text;
                capture.visit(0, [&](const Record &record) {
                    if (filter.matches(record)) {
                        text += report_row(record);
                        text += '\n';
                    }
                    return true;
                });
                ImGui::SetClipboardText(text.c_str());
            }
            if (ImGui::MenuItem(tr_label("All Displayed Rows as CSV"))) {
                std::string text{kCsvHeader};
                capture.visit(0, [&](const Record &record) {
                    if (filter.matches(record)) {
                        text += report_csv(capture.details(record.number));
                    }
                    return true;
                });
                ImGui::SetClipboardText(text.c_str());
            }
            ImGui::EndMenu();
        }

        ImGui::Separator();
        ImGui::MenuItem(tr_label("Settings..."), nullptr, &options.settings_window);
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu(tr_label("View"))) {
        ImGui::MenuItem(tr_label("Packet Details"), nullptr, &options.details_pane);
        ImGui::MenuItem(tr_label("Packet Bytes"), nullptr, &options.bytes_pane);

        ImGui::Separator();
        if (ImGui::BeginMenu(tr_label("Time Display Format"))) {
            time_format(options, TimeFormat::SinceFirst);
            time_format(options, TimeFormat::SincePrevious);
            time_format(options, TimeFormat::SincePreviousDisplayed);
            time_format(options, TimeFormat::TimeOfDay);
            ImGui::EndMenu();
        }

        ImGui::Separator();
        if (ImGui::MenuItem(tr_label("Zoom In"), "Ctrl++")) {
            step_zoom(options, 1);
        }
        if (ImGui::MenuItem(tr_label("Zoom Out"), "Ctrl+-")) {
            step_zoom(options, -1);
        }
        if (ImGui::MenuItem(tr_label("Normal Size"), "Ctrl+0")) {
            options.font_scale = 1.0F;
        }

        ImGui::Separator();
        if (ImGui::MenuItem(tr_label("Expand All"), nullptr, false, options.details_pane)) {
            options.expand_details = true;
        }
        if (ImGui::MenuItem(tr_label("Collapse All"), nullptr, false, options.details_pane)) {
            options.collapse_details = true;
        }

        ImGui::Separator();
        ImGui::MenuItem(tr_label("Colorize Packet List"), nullptr, &options.colorize);
        if (ImGui::MenuItem(tr_label("Resize Columns to Fit"))) {
            options.resize_columns = true;
        }

        ImGui::Separator();
        if (ImGui::MenuItem(tr_label("Show Packet in New Window"), nullptr, false, capture.selected() != 0)) {
            options.detach = capture.selected();
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu(tr_label("Go"))) {
        if (ImGui::MenuItem(tr_label("Go to Packet..."))) {
            open_goto = true;
        }
        ImGui::Separator();
        if (ImGui::MenuItem(tr_label("First Packet"))) {
            jump(capture, filter, list, Jump::First);
        }
        if (ImGui::MenuItem(tr_label("Last Packet"))) {
            jump(capture, filter, list, Jump::Last);
        }

        ImGui::Separator();
        if (ImGui::MenuItem(tr_label("Next Failed Decode"))) {
            jump(capture, filter, list, Jump::NextFailed);
        }
        if (ImGui::MenuItem(tr_label("Previous Failed Decode"))) {
            jump(capture, filter, list, Jump::PreviousFailed);
        }

        const auto selected = capture.at_number(capture.selected());
        char next[192];
        char previous[192];
        if (!selected.name.empty()) {
            std::snprintf(next, sizeof(next), tr("Next %.*s"), static_cast<int>(selected.name.size()),
                          selected.name.data());
            std::snprintf(previous, sizeof(previous), tr("Previous %.*s"), static_cast<int>(selected.name.size()),
                          selected.name.data());
        }
        else if (selected.id >= 0) {
            std::snprintf(next, sizeof(next), tr("Next id %d"), selected.id);
            std::snprintf(previous, sizeof(previous), tr("Previous id %d"), selected.id);
        }
        else {
            std::snprintf(next, sizeof(next), "%s", tr("Next of This Packet"));
            std::snprintf(previous, sizeof(previous), "%s", tr("Previous of This Packet"));
        }
        ImGui::BeginDisabled(selected.id < 0);
        if (ImGui::MenuItem(next)) {
            jump(capture, filter, list, Jump::NextSameId);
        }
        if (ImGui::MenuItem(previous)) {
            jump(capture, filter, list, Jump::PreviousSameId);
        }
        ImGui::EndDisabled();

        ImGui::Separator();
        if (ImGui::MenuItem(tr_label("Back"), nullptr, false, list.history_at > 0)) {
            jump(capture, filter, list, Jump::Back);
        }
        if (ImGui::MenuItem(tr_label("Forward"), nullptr, false, list.history_at + 1 < list.history.size())) {
            jump(capture, filter, list, Jump::Forward);
        }

        ImGui::Separator();
        if (ImGui::MenuItem(tr_label("Auto Scroll"), nullptr, &options.auto_scroll) && options.auto_scroll) {
            jump(capture, filter, list, Jump::Last);
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu(tr_label("Analyze"))) {
        ImGui::MenuItem(tr_label("Expert Information"), nullptr, &options.expert_window);
        ImGui::MenuItem(tr_label("Interpret Selection"), nullptr, &options.inspector);
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu(tr_label("Modules"))) {
        ImGui::MenuItem(tr_label("Module Manager"), nullptr, &options.modules_window);
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu(tr_label("Statistics"))) {
        for (const auto &[label, wanted] : kStatisticsTabs) {
            if (ImGui::MenuItem(tr_label(label))) {
                options.statistics_window = true;
                options.statistics_select = true;
                options.statistics_tab = wanted;
            }
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu(tr_label("Capture"))) {
        if (ImGui::MenuItem(tr_label("Start"), nullptr, false, !capture.running())) {
            capture.start();
        }
        if (ImGui::MenuItem(tr_label("Stop"), nullptr, false, capture.running())) {
            capture.stop();
        }
        if (ImGui::MenuItem(tr_label("Restart"))) {
            capture.restart();
        }
        ImGui::Separator();
        ImGui::MenuItem(tr_label("Options"), nullptr, &options.settings_window);
        ImGui::MenuItem(tr_label("Filter"), nullptr, &options.filter_window);
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu(tr_label("Help"))) {
        const auto failures = errors().size();
        char label[64];
        if (failures == 0) {
            std::snprintf(label, sizeof(label), "%s", tr("Errors"));
        }
        else {
            std::snprintf(label, sizeof(label), tr("Errors (%zu)"), failures);
        }
        ImGui::MenuItem(label, nullptr, &options.errors_window, failures != 0);
        ImGui::MenuItem(tr_label("About Spyglass"), nullptr, &options.about_window);
        ImGui::EndMenu();
    }

    ImGui::EndMenuBar();

    if (open_goto) {
        options.goto_number[0] = '\0';
        ImGui::OpenPopup(tr_label("Go to Packet"));
    }
    if (ImGui::BeginPopupModal(tr_label("Go to Packet"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (ImGui::IsWindowAppearing()) {
            ImGui::SetKeyboardFocusHere();
        }
        ImGui::SetNextItemWidth(160.0F);
        auto go = ImGui::InputText("##number", options.goto_number, sizeof(options.goto_number),
                                   ImGuiInputTextFlags_CharsDecimal | ImGuiInputTextFlags_EnterReturnsTrue);
        go = ImGui::Button(tr_label("Go")) || go;
        ImGui::SameLine();
        const auto cancel = ImGui::Button(tr_label("Cancel"));
        if (go) {
            if (const auto number = std::strtoull(options.goto_number, nullptr, 10); number != 0) {
                show_packet(capture, list, number);
            }
        }
        if (go || cancel) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void draw_about_window(bool &open)
{
    if (ImGui::Begin(tr_label("Spyglass: about"), &open, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Spyglass %s", SPYGLASS_VERSION);
        ImGui::TextColored(palette().muted, tr("A packet capture that runs inside the Minecraft: Bedrock client"));

        ImGui::Separator();
        const auto &signature = signatures();
        ImGui::Text(tr("Client: %.*s"), static_cast<int>(signature.name.size()), signature.name.data());
        const auto &names = packet_names();
        ImGui::Text(tr("Highest packet id: %zu"), names.empty() ? 0 : names.size() - 1);
        std::size_t named = 0;
        for (const auto &name : names) {
            named += name.empty() ? 0 : 1;
        }
        ImGui::Text(tr("Packets named: %zu"), named);

        ImGui::Separator();
        const auto &hooked = hooks();
        ImGui::Text("MinecraftPackets::createPacket   %p", hooked.create_packet);
        ImGui::Text("Packet::read and write           %zu classes", hooked.packet_classes);
    }
    ImGui::End();
}

}  // namespace spyglass
