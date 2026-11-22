#include "spyglass/overlay/settings.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <system_error>

#include <nlohmann/json.hpp>

#include "spyglass/core/output.h"
#include "spyglass/error.h"

namespace spyglass {
namespace {

std::filesystem::path settings_path()
{
    return output_directory() / "settings.json";
}

} // namespace

void step_zoom(ViewOptions &options, const int steps)
{
    const auto stepped = std::round((options.font_scale + (static_cast<float>(steps) * kZoomStep)) * 10.0F) / 10.0F;
    options.font_scale = std::clamp(stepped, kSmallestZoom, kLargestZoom);
}

Settings read_settings()
{
    Settings settings;
    std::ifstream file{settings_path()};
    if (!file) {
        return settings;
    }

    try {
        const auto json = nlohmann::json::parse(file);
        const auto language = json.value("language", static_cast<int>(settings.language));
        const auto theme = json.value("theme", static_cast<int>(settings.theme));
        const auto time_format = json.value("time_format", static_cast<int>(settings.time_format));
        if (language >= static_cast<int>(Language::English) && language <= static_cast<int>(Language::Japanese)) {
            settings.language = static_cast<Language>(language);
        }
        if (theme >= static_cast<int>(Theme::Dark) && theme <= static_cast<int>(Theme::Light)) {
            settings.theme = static_cast<Theme>(theme);
        }
        if (time_format >= static_cast<int>(TimeFormat::SinceFirst) &&
            time_format <= static_cast<int>(TimeFormat::TimeOfDay)) {
            settings.time_format = static_cast<TimeFormat>(time_format);
        }
        settings.zoom = std::clamp(json.value("zoom", settings.zoom), kSmallestZoom, kLargestZoom);
        settings.details_pane = json.value("details_pane", settings.details_pane);
        settings.bytes_pane = json.value("bytes_pane", settings.bytes_pane);
        settings.colorize = json.value("colorize", settings.colorize);
        settings.auto_scroll = json.value("auto_scroll", settings.auto_scroll);
        settings.capture.outbound = json.value("capture_outbound", settings.capture.outbound);
        settings.capture.store.resident_blocks =
            std::max<std::size_t>(2, json.value("index_cache_blocks", settings.capture.store.resident_blocks));
        settings.capture.store.queue_bytes =
            std::max<std::size_t>(1024 * 1024, json.value("writer_queue_bytes", settings.capture.store.queue_bytes));
    }
    catch (const std::exception &failure) {
        report_error(std::format("settings.json was ignored: {}", failure.what()));
        return Settings{};
    }
    return settings;
}

void write_settings(const Settings &settings)
{
    const nlohmann::json json{
        {"language", static_cast<int>(settings.language)},
        {"theme", static_cast<int>(settings.theme)},
        {"zoom", settings.zoom},
        {"time_format", static_cast<int>(settings.time_format)},
        {"details_pane", settings.details_pane},
        {"bytes_pane", settings.bytes_pane},
        {"colorize", settings.colorize},
        {"auto_scroll", settings.auto_scroll},
        {"capture_outbound", settings.capture.outbound},
        {"index_cache_blocks", settings.capture.store.resident_blocks},
        {"writer_queue_bytes", settings.capture.store.queue_bytes},
    };

    const auto path = settings_path();
    auto temporary = path;
    temporary += ".tmp";
    {
        std::ofstream file{temporary, std::ios::trunc};
        file << json.dump(2) << '\n';
        if (!file) {
            report_error(std::format("settings.json could not be written to {}", temporary.string()));
            return;
        }
    }

    std::error_code ec;
    std::filesystem::rename(temporary, path, ec);
    if (ec) {
        report_error(std::format("settings.json could not be replaced: {}", ec.message()));
    }
}

} // namespace spyglass
