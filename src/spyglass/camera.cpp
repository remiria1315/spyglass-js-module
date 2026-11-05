#include "spyglass/camera.h"

#include <algorithm>
#include <atomic>
#include <numbers>

#ifdef _WIN32
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <unordered_set>
#include <vector>

#include <Windows.h>

#include "spyglass/core/output.h"
#include "spyglass/detail.h"
#include "spyglass/hook.h"
#include "spyglass/signature.h"
#endif

namespace spyglass {
namespace {

std::atomic<float> g_fov_degrees{0.0F};

}  // namespace

std::optional<float> camera_fov()
{
    const auto degrees = g_fov_degrees.load(std::memory_order_relaxed);
    return degrees > 0.0F ? std::optional{degrees} : std::nullopt;
}

#ifdef _WIN32

namespace {

static_assert(sizeof(std::optional<float>) == 8);

std::atomic<const void *> g_host{nullptr};
std::atomic<std::chrono::steady_clock::time_point> g_host_seen{};

struct CameraState {
    std::byte before_position_[0x610];
    float position_[3];
    std::byte before_view_[0xf10 - 0x610 - 12];
    float view_[16];
};

static_assert(offsetof(CameraState, position_) == 0x610);
static_assert(offsetof(CameraState, view_) == 0xf10);

void *g_read = nullptr;
void *g_get_fov = nullptr;

struct FovHost {
    float getFov(float partial_tick, bool apply_effects);
};

struct FovSource {
    std::optional<float> read();
};

float FovHost::getFov(const float partial_tick, const bool apply_effects)
{
    g_host.store(this, std::memory_order_relaxed);
    g_host_seen.store(std::chrono::steady_clock::now(), std::memory_order_relaxed);
    const auto fov = SPYGLASS_CALL_ORIGINAL(&FovHost::getFov, g_get_fov, this, partial_tick, apply_effects);

    static unsigned calls = 0;
    const auto trigger = output_directory() / "camera_scan";
    if (++calls % 120 != 0 || !std::filesystem::exists(trigger)) {
        return fov;
    }
    double want[3]{};
    std::ifstream{trigger} >> want[0] >> want[1] >> want[2];
    std::filesystem::remove(trigger);

    struct Node {
        const std::byte *base;
        int depth;
        std::string path;
    };
    std::vector<Node> nodes{{reinterpret_cast<const std::byte *>(this), 0, "host"}};
    std::unordered_set<const void *> seen{this};
    std::ofstream out{output_directory() / "camera_scan.txt"};
    out << std::format("looking for {} {} {}\n", want[0], want[1], want[2]);

    int matches = 0;
    for (std::size_t i = 0; i < nodes.size() && i < 30000 && matches < 80; ++i) {
        const auto node = nodes[i];
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(node.base, &info, sizeof(info)) == 0 || info.State != MEM_COMMIT ||
            (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) {
            continue;
        }
        const auto region_end = static_cast<const std::byte *>(info.BaseAddress) + info.RegionSize;
        const auto limit = std::min<std::size_t>(0x1800, static_cast<std::size_t>(region_end - node.base));

        for (std::size_t offset = 0; offset + 12 <= limit; offset += 4) {
            float p[3];
            std::memcpy(p, node.base + offset, sizeof(p));
            if (std::abs(p[0] - want[0]) > 0.3 || std::abs(p[2] - want[2]) > 0.3 || std::abs(p[1] - want[1]) > 2.0) {
                continue;
            }
            matches++;
            out << std::format("{} -> +{:#x}: {:.5f} {:.5f} {:.5f}\n", node.path, offset, p[0], p[1], p[2]);
        }

        for (std::size_t offset = 0; node.depth < 4 && nodes.size() < 30000 && offset + 8 <= limit; offset += 8) {
            std::uintptr_t value = 0;
            std::memcpy(&value, node.base + offset, sizeof(value));
            if (value >= 0x10000 && value < 0x00007fffffffffffULL &&
                seen.insert(reinterpret_cast<const void *>(value)).second) {
                nodes.push_back({reinterpret_cast<const std::byte *>(value), node.depth + 1,
                                 std::format("{} -> +{:#x}", node.path, offset)});
            }
        }
    }
    out << std::format("{} match(es), {} objects visited\n", matches, std::min<std::size_t>(nodes.size(), 30000));
    return fov;
}

std::optional<float> FovSource::read()
{
    const auto fov = SPYGLASS_CALL_ORIGINAL(&FovSource::read, g_read, this);
    if (fov) {
        g_fov_degrees.store(*fov * (180.0F / std::numbers::pi_v<float>), std::memory_order_relaxed);
    }
    return fov;
}

}  // namespace

std::optional<CameraPose> camera_pose()
{
    const auto *const host = static_cast<const std::byte *>(g_host.load(std::memory_order_relaxed));
    const auto fov = camera_fov();
    if (host == nullptr || !fov || std::chrono::steady_clock::now() - g_host_seen.load(std::memory_order_relaxed) > std::chrono::seconds{2}) {
        return std::nullopt;
    }

    float position[3];
    float view[16];
    std::memcpy(position, host + offsetof(CameraState, position_), sizeof(position));
    std::memcpy(view, host + offsetof(CameraState, view_), sizeof(view));

    const auto forward_x = -view[2];
    const auto forward_y = -view[6];
    const auto forward_z = -view[10];
    constexpr auto kDegrees = 180.0F / std::numbers::pi_v<float>;
    const CameraPose pose{position[0],
                          position[1],
                          position[2],
                          std::atan2(-forward_x, forward_z) * kDegrees,
                          std::asin(std::clamp(-forward_y, -1.0F, 1.0F)) * kDegrees,
                          *fov};
    const auto finite = std::isfinite(pose.x) && std::isfinite(pose.y) && std::isfinite(pose.z) &&
                        std::isfinite(pose.yaw) && std::isfinite(pose.pitch);
    return finite ? std::optional{pose} : std::nullopt;
}

void install_camera_hook()
{
    install_hook("FovSource::read", kCameraFov, detail::fp_cast(&FovSource::read), &g_read);
    install_hook("FovHost::getFov", kCameraFovHost, detail::fp_cast(&FovHost::getFov), &g_get_fov);
}

#else

std::optional<CameraPose> camera_pose()
{
    return std::nullopt;
}

void install_camera_hook() {}

#endif

}  // namespace spyglass
