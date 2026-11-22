#pragma once

#include <ctime>

namespace spyglass {

inline std::tm local_time(const std::time_t seconds)
{
    std::tm parts{};
    localtime_s(&parts, &seconds);
    return parts;
}

}  // namespace spyglass
