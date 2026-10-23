#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "spyglass/overlay/store.h"

namespace spyglass::script {

enum class Side : std::uint8_t { Client, Server };

struct Event {
    Direction direction{Direction::Inbound};
    int id{-1};
    std::string_view name;
    std::uint8_t sub_id{0};
    std::string_view body;
    Side side{Side::Client};
    int player{-1};
};

struct Verdict {
    bool cancelled{false};
    std::optional<std::string> body;
};

struct LogLine {
    std::string module;
    std::string text;
    bool error{false};
};

struct ModuleInfo {
    std::string name;
    bool enabled{false};
    bool loaded{false};
    std::vector<std::string> permissions;
    std::string error;
};

using Sender = bool (*)(Side side, Direction direction, int id, std::string_view body, int player);

void set_sender(Sender sender);

void start(std::vector<std::string> packet_names);

[[nodiscard]] bool wants(Side side, Direction direction, int id);

Verdict dispatch(const Event &event, const std::function<nlohmann::ordered_json()> &fields);

void tick();

bool handle_chat(std::string_view message);

void execute(std::string_view arguments);

[[nodiscard]] std::filesystem::path modules_directory();

[[nodiscard]] std::vector<ModuleInfo> modules();

[[nodiscard]] std::vector<LogLine> log_lines();

[[nodiscard]] std::uint64_t log_serial();

[[nodiscard]] bool take_attention();

}  // namespace spyglass::script
