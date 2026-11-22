#pragma once

#include "spyglass/overlay/capture.h"
#include "spyglass/overlay/i18n.h"
#include "spyglass/overlay/options.h"
#include "spyglass/overlay/theme.h"

namespace spyglass {

struct Settings {
    Language language{Language::English};
    Theme theme{Theme::Dark};
    float zoom{1.0F};
    TimeFormat time_format{TimeFormat::SinceFirst};
    bool details_pane{true};
    bool bytes_pane{true};
    bool colorize{true};
    bool auto_scroll{true};
    CaptureOptions capture;

    bool operator==(const Settings &other) const = default;
};

[[nodiscard]] Settings read_settings();

void write_settings(const Settings &settings);

} // namespace spyglass
