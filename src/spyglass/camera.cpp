#include "spyglass/camera.h"

#include <atomic>
#include <numbers>

#ifdef _WIN32
#include <algorithm>
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

void *g_read = nullptr;
void *g_get_fov = nullptr;

struct FovHost {
    float getFov(float partial_tick, bool apply_effects);
};

struct FovSource {
    std::optional<float> read();
};

std::optional<float> FovSource::read()
{
    const auto fov = SPYGLASS_CALL_ORIGINAL(&FovSource::read, g_read, this);
    if (fov) {
        g_fov_degrees.store(*fov * (180.0F / std::numbers::pi_v<float>), std::memory_order_relaxed);
    }
    return fov;
}

float FovHost::getFov(const float partial_tick, const bool apply_effects)
{
    const auto fov = SPYGLASS_CALL_ORIGINAL(&FovHost::getFov, g_get_fov, this, partial_tick, apply_effects);

    static unsigned calls = 0;
    const auto degrees = g_fov_degrees.load(std::memory_order_relaxed);
    if (++calls % 120 != 0 || degrees <= 0.0F || !std::filesystem::remove(output_directory() / "camera_scan")) {
        return fov;
    }

    struct Node {
        const std::byte *base;
        int depth;
        std::string path;
    };
    const double targets[2] = {1.0 / std::tan(degrees * std::numbers::pi / 360.0),
                               1.0 / std::tan(70.0 * std::numbers::pi / 360.0)};
    std::vector<Node> nodes{{reinterpret_cast<const std::byte *>(this), 0, "host"}};
    std::unordered_set<const void *> seen{this};
    std::ofstream out{output_directory() / "camera_scan.txt"};
    out << std::format("fov {} degrees, projection scale {} and {}\n", degrees, targets[0], targets[1]);

    int matches = 0;
    for (std::size_t i = 0; i < nodes.size() && i < 20000 && matches < 8; ++i) {
        const auto node = nodes[i];
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(node.base, &info, sizeof(info)) == 0 || info.State != MEM_COMMIT ||
            (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) {
            continue;
        }
        const auto region_end = static_cast<const std::byte *>(info.BaseAddress) + info.RegionSize;
        const auto limit = std::min<std::size_t>(0x2000, static_cast<std::size_t>(region_end - node.base));

        for (std::size_t offset = 0; offset + 0x30 <= limit; offset += 4) {
            float scale = 0.0F;
            std::memcpy(&scale, node.base + offset + 20, sizeof(scale));
            if (std::abs(scale - targets[0]) > 2e-3 && std::abs(scale - targets[1]) > 2e-3) {
                continue;
            }
            float m[12];
            std::memcpy(m, node.base + offset, sizeof(m));
            const auto flat = std::abs(m[1]) + std::abs(m[2]) + std::abs(m[3]) + std::abs(m[4]) + std::abs(m[6]) +
                              std::abs(m[7]);
            if (flat > 1e-5F || m[0] < 0.2F * scale || m[0] > 5.0F * scale) {
                continue;
            }

            matches++;
            const auto first = offset >= 0x80 ? offset - 0x80 : 0;
            out << std::format("\nmatch {}: {} -> +{:#x}, m00 {} m11 {}\n", matches, node.path, offset, m[0],
                               scale);
            for (auto at = first; at + 16 <= limit && at < offset + 0x100; at += 16) {
                float row[4];
                std::memcpy(row, node.base + at, sizeof(row));
                out << std::format("  +{:#06x}: {:.6g} {:.6g} {:.6g} {:.6g}\n", at, row[0], row[1], row[2], row[3]);
            }
        }

        for (std::size_t offset = 0; node.depth < 3 && nodes.size() < 40000 && offset + 8 <= limit; offset += 8) {
            std::uintptr_t value = 0;
            std::memcpy(&value, node.base + offset, sizeof(value));
            if (value >= 0x10000 && value < 0x00007fffffffffffULL &&
                seen.insert(reinterpret_cast<const void *>(value)).second) {
                nodes.push_back({reinterpret_cast<const std::byte *>(value), node.depth + 1,
                                 std::format("{} -> +{:#x}", node.path, offset)});
            }
        }
    }
    out << std::format("\n{} match(es), {} objects visited\n", matches, std::min<std::size_t>(nodes.size(), 20000));
    return fov;
}

}  // namespace

void install_camera_hook()
{
    install_hook("FovSource::read", kCameraFov, detail::fp_cast(&FovSource::read), &g_read);
    install_hook("FovHost::getFov", kCameraFovHost, detail::fp_cast(&FovHost::getFov), &g_get_fov);
}

#else

void install_camera_hook() {}

#endif

}  // namespace spyglass
