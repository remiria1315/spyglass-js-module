#pragma once

#include <cstdint>
#include <vector>

#include "spyglass/overlay/capture.h"
#include "spyglass/overlay/filter.h"
#include "spyglass/overlay/options.h"
#include "spyglass/overlay/pane/expert_window.h"
#include "spyglass/overlay/pane/export_dialog.h"
#include "spyglass/overlay/pane/filter_window.h"
#include "spyglass/overlay/pane/modules_window.h"
#include "spyglass/overlay/pane/packet_bytes.h"
#include "spyglass/overlay/pane/packet_list.h"
#include "spyglass/overlay/settings.h"

namespace spyglass {

struct DetachedPacket {
    std::uint64_t number{0};
    bool open{true};
    BytesView bytes;
};

class View {
public:
    static View &getInstance();

    void draw();

    void onPacketSend(Incoming incoming);
    void onPacketReceive(Incoming incoming);
    [[nodiscard]] bool wants_fields(int id, Direction direction) const;

    void toggle() noexcept { visible_ = !visible_; }
    [[nodiscard]] bool visible() const noexcept { return visible_; }
    void set_interactive(const bool interactive) noexcept { interactive_ = interactive; }

  private:
    View();

    Capture capture_;
    Filter filter_;
    ViewOptions options_;
    bool visible_{true};
    bool interactive_{true};
    float list_share_{0.45F};
    float details_share_{0.30F};
    PacketList list_;
    FilterWindow filter_window_;
    ExpertWindow expert_window_;
    ExportDialog export_dialog_;
    ModulesWindow modules_window_;
    BytesView bytes_view_;
    std::vector<DetachedPacket> detached_;
    Settings saved_;
    bool unsaved_{false};
    double changed_at_{0.0};
};

}  // namespace spyglass
