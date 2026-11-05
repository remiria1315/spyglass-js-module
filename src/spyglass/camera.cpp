#include "spyglass/camera.h"

#include <atomic>
#include <numbers>

#ifdef _WIN32
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

}  // namespace

void install_camera_hook()
{
    install_hook("FovSource::read", kCameraFov, detail::fp_cast(&FovSource::read), &g_read);
}

#else

void install_camera_hook() {}

#endif

}  // namespace spyglass
