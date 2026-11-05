#pragma once

#include <optional>

namespace spyglass {

struct CameraPose {
    float x;
    float y;
    float z;
    float yaw;
    float pitch;
    float fov;
};

void install_camera_hook();

std::optional<float> camera_fov();

std::optional<CameraPose> camera_pose();

}  // namespace spyglass
