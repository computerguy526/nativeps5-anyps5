#ifndef CORE_LIBS_PRX_LIBSCEVIDEOOUT_INCLUDE_DISPLAYWINDOW_HPP
#define CORE_LIBS_PRX_LIBSCEVIDEOOUT_INCLUDE_DISPLAYWINDOW_HPP

#include "prx/libSceVideoOut/include/HostFullscreen.hpp"
#include <cstdint>
#include <cstdlib>
#include <string>
#include <memory>
#include "SDL.h"

inline constexpr std::uint32_t DisplayWindowMinimumWidth = 320;
inline constexpr std::uint32_t DisplayWindowMinimumHeight = 180;
// nps5: "[tag] " for the start of the window title: NPS5_RUN_TAG, else the basename of NPS5_RUNS_DIR; empty when neither is set.
inline std::string DisplayWindowRunTagPrefix() {
    static const std::string prefix = [] {
        std::string tag;
        if (const char* value = std::getenv("NPS5_RUN_TAG"); value != nullptr && *value != 0) tag = value;
        else if (const char* dir = std::getenv("NPS5_RUNS_DIR"); dir != nullptr && *dir != 0) {
            tag = dir;
            while (tag.size() > 1 && tag.back() == '/') tag.pop_back();
            if (const auto slash = tag.find_last_of('/'); slash != std::string::npos) tag = tag.substr(slash + 1);
        }
        return tag.empty() ? std::string() : "[" + tag + "] ";
    }();
    return prefix;
}

// nps5: the title name shown in the window: NPS5_WINDOW_TITLE_NAME when set, else the app's own title.
inline std::string DisplayWindowTitleName(const char* appTitle) {
    if (const char* value = std::getenv("NPS5_WINDOW_TITLE_NAME"); value != nullptr && *value != 0) return value;
    // The full name of PPSA30552 pushes the FPS out of the title bar: shorten it (NPS5_TITLE_ID is set by the run harness).
    if (const char* id = std::getenv("NPS5_TITLE_ID"); id != nullptr && std::string(id) == "PPSA30552") return "Digging A Hole";
    return appTitle != nullptr ? appTitle : "";
}

inline constexpr std::uint32_t DisplayWindowInitialSizePercent = 30;  // half width/height of usable display (quarter area); NPS5_WINDOW_PERCENT overrides

class DisplayWindow {
public:
    DisplayWindow() = default;
    ~DisplayWindow();
    DisplayWindow(const DisplayWindow&) = delete;
    DisplayWindow& operator=(const DisplayWindow&) = delete;

    void Ensure(std::uint32_t sourceWidth, std::uint32_t sourceHeight);
    void Destroy() noexcept;
    SDL_Window* Handle() const;
    void DrawableSize(std::uint32_t& width, std::uint32_t& height) const;
    void UpdateTitle();
    std::uint64_t FrameNumber() const { return frameNum; }
    void ToggleFullscreen();

private:
    void create(std::uint32_t sourceWidth, std::uint32_t sourceHeight);
    void updateAspectRatio(std::uint32_t sourceWidth, std::uint32_t sourceHeight);
    void installSubclass();
    void removeSubclass() noexcept;
    void applyAspectRatio(void* hwnd, std::uintptr_t edge, void* rect) const;

    static std::intptr_t windowProc(void* hwnd, unsigned int message, std::uintptr_t wParam, std::intptr_t lParam, std::uintptr_t subclassId, std::uintptr_t referenceData);

    SDL_Window* window = nullptr;
    std::unique_ptr<IHostFullscreenWindow> hostWindow;
    std::unique_ptr<HostFullscreenState> fullscreen;
    std::uint32_t aspectWidth = 0;
    std::uint32_t aspectHeight = 0;
    std::uint64_t frameNum = 0;
};

#endif
