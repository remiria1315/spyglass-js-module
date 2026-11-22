#include "spyglass/overlay/pane/status_bar.h"
#include "spyglass/overlay/i18n.h"

#include <algorithm>

#include <imgui.h>

#include "spyglass/overlay/capture.h"
#include "spyglass/overlay/pane/packet_bytes.h"
#include "spyglass/overlay/pane/packet_list.h"
#include "spyglass/overlay/theme.h"

namespace spyglass {

void draw_status_bar(const Capture &capture, const PacketList &list, const BytesView &bytes)
{
    const auto counters = capture.counters();
    const auto total = static_cast<unsigned long long>(counters.total);
    const auto bad = static_cast<unsigned long long>(counters.bad);
    const double share = total == 0 ? 0.0 : (100.0 * static_cast<double>(bad)) / static_cast<double>(total);

    ImGui::TextColored(palette().muted, tr("Packets: %llu"), total);
    ImGui::SameLine();
    ImGui::TextColored(palette().muted, "|");
    ImGui::SameLine();
    ImGui::TextColored(bad == 0 ? palette().muted : palette().bad_packet, tr("Bad: %llu (%.1f%%)"), bad, share);

    if (counters.rejected != 0) {
        ImGui::SameLine();
        ImGui::TextColored(palette().muted, "|");
        ImGui::SameLine();
        ImGui::TextColored(palette().find_hit, tr("Not captured: %llu"),
                           static_cast<unsigned long long>(counters.rejected));
    }

    if (counters.dropped != 0) {
        ImGui::SameLine();
        ImGui::TextColored(palette().muted, "|");
        ImGui::SameLine();
        ImGui::TextColored(palette().bad_packet, tr("Dropped: %llu"),
                           static_cast<unsigned long long>(counters.dropped));
    }

    ImGui::SameLine();
    ImGui::TextColored(palette().muted, "|");
    ImGui::SameLine();
    ImGui::TextColored(palette().muted, tr("On disk: %.1f MB"),
                       static_cast<double>(counters.stored_bytes) / (1024.0 * 1024.0));

    if (list.applied.active()) {
        const auto retained = list.newest == 0 ? 0ULL : static_cast<unsigned long long>(list.newest - list.oldest + 1);
        ImGui::SameLine();
        ImGui::TextColored(palette().muted, "|");
        ImGui::SameLine();
        ImGui::TextColored(palette().muted, tr("Displayed: %llu of %llu"),
                           static_cast<unsigned long long>(list.displayed), retained);
    }

    if (bytes.selected) {
        const auto first = std::min(bytes.anchor, bytes.cursor);
        const auto last = std::max(bytes.anchor, bytes.cursor);
        ImGui::SameLine();
        ImGui::TextColored(palette().muted, "|");
        ImGui::SameLine();
        ImGui::TextColored(palette().muted, tr("Selected: 0x%zX..0x%zX (%zu bytes)"), first, last, last - first + 1);
    }
}

}  // namespace spyglass
