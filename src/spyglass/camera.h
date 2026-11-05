#pragma once

#include <optional>

namespace spyglass {

void install_camera_hook();

std::optional<float> camera_fov();

}  // namespace spyglass
