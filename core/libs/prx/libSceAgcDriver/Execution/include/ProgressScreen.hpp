#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_PROGRESSSCREEN_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_PROGRESSSCREEN_HPP

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include "prx/libSceAgcDriver/Execution/include/PerformanceOverlay.hpp"
#include <cstdint>
#include <string>

namespace AgcDriver {

// nps5: the startup shader-prebuild screen drawn into the game window (dark background, centered rounded bar, title, "N / M", percent,
// ETA) with the overlay's rect pipeline and 5x7 font. The look follows the prepare-step progress screen of nxnative (MIT, seedds): a dark
// panel, a title, a bar and a count, centered; only the look, no code.
struct ProgressScreenInfo {
    std::uint64_t done = 0;
    std::uint64_t total = 0;
    double etaSeconds = -1.0;  // < 0: unknown
    std::string title = "Compiling shaders";
};

std::string FormatProgressEta(double seconds);
OverlayLayout LayOutProgress(const ProgressScreenInfo& info, VkExtent2D extent);

}

#endif
