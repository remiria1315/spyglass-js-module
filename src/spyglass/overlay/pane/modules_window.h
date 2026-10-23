#pragma once

#include <cstdint>
#include <vector>

#include "spyglass/script/engine.h"

namespace spyglass {

struct ModulesWindow {
    std::uint64_t seen{0};
    double refreshed{-1.0};
    double touched{0.0};
    std::vector<script::ModuleInfo> modules;
    std::vector<script::LogLine> lines;
};

void draw_modules_window(ModulesWindow &window, bool &open, bool passive);

}  // namespace spyglass
