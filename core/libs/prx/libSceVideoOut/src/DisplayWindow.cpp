#include "prx/libSceVideoOut/include/DisplayWindow.hpp"
#include "prx/libSceVideoOut/include/Telemetry.hpp"
#include "prx/libSceAgcDriver/Execution/include/AspectFit.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceMouse/include/MouseState.hpp"
#include "prx/libkernel/AppMetadata/include/AppMetadata.hpp"
#include "Decoder/Png.hpp"
#include "SDL_vulkan.h"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <stdexcept>
#include <string>
#ifndef _WIN32
#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#ifdef __APPLE__
#include <CoreGraphics/CoreGraphics.h>
#endif

#ifdef _WIN32
#include "SDL_syswm.h"
#include <windows.h>
#include <commctrl.h>
#endif

// nps5: window slot registry. NPS5_WINDOW_STACK=0 turns it off. Each open window holds an flock() on slot-N in NPS5_WINDOW_SLOT_DIR
// (default <per-user temp dir>/nps5-windows, else $TMPDIR, else /tmp) for the life of the process, and writes its pid into it for humans/tools. The kernel drops
// the lock when the process dies for any reason (crash, SIGKILL), so there is no pid-reuse and no unlink race: a slot file is never unlinked,
// a new window just takes the lowest slot whose lock it can get. The window is placed below the windows of the lower slots, then in the next column when the screen is full.
namespace {
int windowSlot = -1;
int windowSlotFd = -1;

void releaseWindowSlot() noexcept {
#ifndef _WIN32
    if (windowSlotFd >= 0) {
        if (ftruncate(windowSlotFd, 0) != 0) {}  // we still hold the lock: only our own pid text is cleared
        close(windowSlotFd);                      // closing drops the flock
        windowSlotFd = -1;
    }
#endif
    windowSlot = -1;
}

int takeWindowSlot() {
#ifndef _WIN32
    const char* off = std::getenv("NPS5_WINDOW_STACK");
    if (off != nullptr && std::string(off) == "0") return -1;
    std::string dir;
    if (const char* custom = std::getenv("NPS5_WINDOW_SLOT_DIR"); custom != nullptr && *custom != '\0') {
        dir = custom;
    } else {
        // The per-user temp dir from the system (not $TMPDIR: test harnesses point that at a private dir per run, which would stop the windows stacking).
        char tmpBuffer[1024] = {};
        const char* tmp = std::getenv("TMPDIR");
#ifdef __APPLE__
        if (confstr(_CS_DARWIN_USER_TEMP_DIR, tmpBuffer, sizeof(tmpBuffer)) > 0) tmp = tmpBuffer;
#endif
        dir = std::string(tmp != nullptr && *tmp != '\0' ? tmp : "/tmp");
        while (dir.size() > 1 && dir.back() == '/') dir.pop_back();
        dir += "/nps5-windows";
    }
    mkdir(dir.c_str(), 0700);  // the parent must exist
    struct stat info{};
    if (stat(dir.c_str(), &info) != 0 || !S_ISDIR(info.st_mode)) return -1;
    for (int slot = 0; slot < 64; ++slot) {
        const std::string path = dir + "/slot-" + std::to_string(slot);
        const int fd = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (fd < 0) return -1;
        if (flock(fd, LOCK_EX | LOCK_NB) != 0) {  // held by a live process: next slot
            close(fd);
            continue;
        }
        const auto text = std::to_string(getpid());
        if (ftruncate(fd, 0) != 0) {}
        if (pwrite(fd, text.data(), text.size(), 0) < 0) {}
        static bool registered = false;
        if (!registered) {
            registered = true;
            std::atexit([] { releaseWindowSlot(); });
        }
        windowSlotFd = fd;
        return slot;
    }
#endif
    return -1;
}
}

namespace {

void require(bool condition, const char* reason) {
    if (!condition) throw std::runtime_error(std::string("DisplayWindow: ") + reason);
}

// SDL index of the built-in (laptop) panel, or -1. CoreGraphics and SDL2's Cocoa backend both use global
// top-left-origin point coordinates, so the display whose bounds start at the panel's origin is the panel.
int builtinDisplayIndex() {
#ifdef __APPLE__
    CGDirectDisplayID ids[16];
    uint32_t count = 0;
    if (CGGetActiveDisplayList(16, ids, &count) != kCGErrorSuccess) return -1;
    for (uint32_t i = 0; i < count; ++i) {
        if (!CGDisplayIsBuiltin(ids[i])) continue;
        const CGRect b = CGDisplayBounds(ids[i]);
        for (int d = 0; d < SDL_GetNumVideoDisplays(); ++d) {
            SDL_Rect r{};
            if (SDL_GetDisplayBounds(d, &r) == 0 && r.x == static_cast<int>(b.origin.x) && r.y == static_cast<int>(b.origin.y)) return d;
        }
    }
#endif
    return -1;
}

// NPS5_WINDOW_SCREEN=<builtin|main|index>[:tl|:tr|:bl|:br|:center] (as nxnative's DM_WINDOW_SCREEN). Default
// "builtin:tl": the laptop panel (falling back to the main display), window in its top-left corner, so test runs
// leave an external monitor free.
enum class WindowCorner { Center, TopLeft, TopRight, BottomLeft, BottomRight };

struct WindowPlacement {
    int display = -1;
    WindowCorner corner = WindowCorner::TopLeft;
};

WindowPlacement windowPlacement() {
    WindowPlacement placement;
    std::string spec = "builtin:tl";
    if (const char* value = std::getenv("NPS5_WINDOW_SCREEN"); value != nullptr && *value != '\0') spec = value;
    std::string screen = spec;
    if (const auto colon = spec.find(':'); colon != std::string::npos) {
        screen = spec.substr(0, colon);
        const auto where = spec.substr(colon + 1);
        if (where == "tl") placement.corner = WindowCorner::TopLeft;
        else if (where == "tr") placement.corner = WindowCorner::TopRight;
        else if (where == "bl") placement.corner = WindowCorner::BottomLeft;
        else if (where == "br" || where == "corner") placement.corner = WindowCorner::BottomRight;
        else placement.corner = WindowCorner::Center;
    } else if (std::getenv("NPS5_WINDOW_SCREEN") != nullptr) {
        placement.corner = WindowCorner::Center;
    }
    if (screen == "builtin") placement.display = builtinDisplayIndex();
    else if (screen == "main") placement.display = 0;
    else {
        char* end = nullptr;
        const auto index = std::strtol(screen.c_str(), &end, 10);
        if (end != screen.c_str() && *end == '\0' && index >= 0 && index < SDL_GetNumVideoDisplays()) placement.display = static_cast<int>(index);
        else std::fprintf(stderr, "DisplayWindow: ignoring NPS5_WINDOW_SCREEN=%s\n", spec.c_str());
    }
    return placement;
}

int initialDisplayIndex() {
    const auto value = std::getenv("ANYPS5_DISPLAY_INDEX");
    if (value == nullptr) {
        const auto placement = windowPlacement();
        return placement.display >= 0 ? placement.display : 0;
    }
    char* end = nullptr;
    const auto index = std::strtol(value, &end, 10);
    const auto count = SDL_GetNumVideoDisplays();
    require(end != value && *end == '\0' && index >= 0 && index < count, "ANYPS5_DISPLAY_INDEX is not a connected display index");
    return static_cast<int>(index);
}

// Initial window size as a percent of the usable display, applied to width and height.
// NPS5_WINDOW_PERCENT=1..100 overrides; default is DisplayWindowInitialSizePercent.
std::uint32_t initialSizePercent() {
    const char* value = std::getenv("NPS5_WINDOW_PERCENT");
    if (value == nullptr || *value == '\0') return DisplayWindowInitialSizePercent;
    char* end = nullptr;
    const auto parsed = std::strtol(value, &end, 10);
    if (end == value || *end != '\0' || parsed < 1 || parsed > 100) {
        std::fprintf(stderr, "DisplayWindow: ignoring NPS5_WINDOW_PERCENT=%s (want 1..100)\n", value);
        return DisplayWindowInitialSizePercent;
    }
    return static_cast<std::uint32_t>(parsed);
}

void applyAppIcon(SDL_Window* window) {
    if (!HasAppIcon_nid_postfix()) return;
    const auto data = GetAppIconData_nid_postfix();
    auto image = Decoder::Png::Decode(std::span<const std::uint8_t>(data.bytes, static_cast<std::size_t>(data.size)));
    require(image.has_value(), "failed to decode icon0.png");
    SDL_Surface* icon = SDL_CreateRGBSurfaceWithFormatFrom(image->pixels.data(), static_cast<int>(image->width), static_cast<int>(image->height), 32, static_cast<int>(image->width * 4), SDL_PIXELFORMAT_RGBA32);
    require(icon != nullptr, SDL_GetError());
    SDL_SetWindowIcon(window, icon);
    SDL_FreeSurface(icon);
}

class SdlHostWindow final : public IHostFullscreenWindow {
public:
    explicit SdlHostWindow(SDL_Window* window) : window(window) {}

    bool IsFullscreen() const override {
        return (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0;
    }

    void SetFullscreen(bool fullscreen) override {
        require(SDL_SetWindowFullscreen(window, fullscreen ? static_cast<Uint32>(SDL_WINDOW_FULLSCREEN_DESKTOP) : 0u) == 0, SDL_GetError());
    }

    HostWindowSize Size() const override {
        HostWindowSize size;
        SDL_GetWindowSize(window, &size.width, &size.height);
        return size;
    }

    void SetSize(HostWindowSize size) override {
        SDL_SetWindowSize(window, size.width, size.height);
    }

    void ShowCursor(bool show) override {
        require(SDL_ShowCursor(show ? SDL_ENABLE : SDL_DISABLE) >= 0, SDL_GetError());
    }

private:
    SDL_Window* window;
};

#ifdef _WIN32
constexpr UINT_PTR DisplayWindowSubclassId = 0x41505335u;
#endif

}

DisplayWindow::~DisplayWindow() {
    Destroy();
}

void DisplayWindow::Ensure(std::uint32_t sourceWidth, std::uint32_t sourceHeight) {
    require(sourceWidth != 0 && sourceHeight != 0, "source extent must be non-zero");
    if (window == nullptr) create(sourceWidth, sourceHeight);
    updateAspectRatio(sourceWidth, sourceHeight);
}

void DisplayWindow::create(std::uint32_t sourceWidth, std::uint32_t sourceHeight) {
    SDL_Rect usable{};
    const auto displayIndex = initialDisplayIndex();
    require(SDL_GetDisplayUsableBounds(displayIndex, &usable) == 0, SDL_GetError());
    const auto percent = initialSizePercent();
    require(percent > 0 && percent <= 100, "initial window size percent must be between 1 and 100");
    require(usable.w > 0 && usable.h > 0, "usable display extent must be positive");
    const auto boundsWidth = std::max<std::uint32_t>(DisplayWindowMinimumWidth, static_cast<std::uint32_t>(static_cast<std::uint64_t>(usable.w) * percent / 100));
    const auto boundsHeight = std::max<std::uint32_t>(DisplayWindowMinimumHeight, static_cast<std::uint32_t>(static_cast<std::uint64_t>(usable.h) * percent / 100));
    const auto initialSize = AgcDriver::ComputeContainSize_nid_postfix(sourceWidth, sourceHeight, boundsWidth, boundsHeight, true);
    require(initialSize.width >= DisplayWindowMinimumWidth && initialSize.height >= DisplayWindowMinimumHeight, "initial window extent is smaller than the minimum");
    const char* fullscreenSetting = std::getenv("ANYPS5_FULLSCREEN");
    bool unrecognizedFullscreen = false;
    const auto fullscreenMode = ParseHostFullscreenMode(fullscreenSetting, &unrecognizedFullscreen);
    if (unrecognizedFullscreen) std::fprintf(stderr, "%s\n", HostFullscreenWarning(fullscreenSetting).c_str());
    const auto title = GetAppTitle_nid_postfix();
    AgcDriverLockVulkanLoader_nid_postfix();
    const auto fullscreenFlag = fullscreenMode == HostFullscreenMode::Borderless ? static_cast<Uint32>(SDL_WINDOW_FULLSCREEN_DESKTOP) : 0u;
    int x = SDL_WINDOWPOS_CENTERED_DISPLAY(displayIndex);
    int y = SDL_WINDOWPOS_CENTERED_DISPLAY(displayIndex);
    if (const auto corner = windowPlacement().corner; fullscreenFlag == 0 && std::getenv("ANYPS5_DISPLAY_INDEX") == nullptr && corner != WindowCorner::Center) {
        // SDL positions the content area; leave room for the macOS title bar above a top-aligned window.
        constexpr int titleBar = 28;
        const bool left = corner == WindowCorner::TopLeft || corner == WindowCorner::BottomLeft;
        const bool top = corner == WindowCorner::TopLeft || corner == WindowCorner::TopRight;
        x = left ? usable.x : usable.x + usable.w - static_cast<int>(initialSize.width);
        y = top ? usable.y + titleBar : usable.y + usable.h - static_cast<int>(initialSize.height);
    }
    if (fullscreenFlag == 0 && std::getenv("ANYPS5_DISPLAY_INDEX") == nullptr && windowSlot < 0) {
        const int slot = takeWindowSlot();
        if (slot >= 0) {
            windowSlot = slot;
            constexpr int titleBar = 28;
            const int pitch = static_cast<int>(initialSize.height) + titleBar;
            const int rows = std::max(1, usable.h / pitch);
            const int column = slot / rows;
            const int columns = std::max(1, usable.w / static_cast<int>(initialSize.width));
            x = usable.x + (column % columns) * static_cast<int>(initialSize.width);
            y = usable.y + titleBar + (slot % rows) * pitch;
        }
    }
    window = SDL_CreateWindow((DisplayWindowRunTagPrefix() + DisplayWindowTitleName(title.value)).c_str(), x, y, static_cast<int>(initialSize.width), static_cast<int>(initialSize.height), SDL_WINDOW_SHOWN | SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | fullscreenFlag);
    AgcDriverUnlockVulkanLoader_nid_postfix();
    if (window == nullptr) releaseWindowSlot();
    require(window != nullptr, SDL_GetError());
    SDL_SetWindowMinimumSize(window, static_cast<int>(DisplayWindowMinimumWidth), static_cast<int>(DisplayWindowMinimumHeight));
    // NPS5_WINDOW_TOP=1 keeps the test window above other apps' windows (nxnative's DM_WINDOW_TOP).
    if (const char* top = std::getenv("NPS5_WINDOW_TOP"); top != nullptr && std::atoi(top) != 0) SDL_SetWindowAlwaysOnTop(window, SDL_TRUE);
    applyAppIcon(window);
    installSubclass();
    hostWindow = std::make_unique<SdlHostWindow>(window);
    fullscreen = std::make_unique<HostFullscreenState>(*hostWindow);
    fullscreen->Adopt(fullscreenMode, HostWindowSize{static_cast<int>(initialSize.width), static_cast<int>(initialSize.height)});
}

void DisplayWindow::updateAspectRatio(std::uint32_t sourceWidth, std::uint32_t sourceHeight) {
    aspectWidth = sourceWidth;
    aspectHeight = sourceHeight;
}

void DisplayWindow::Destroy() noexcept {
    if (window == nullptr) return;
    if (fullscreen) fullscreen->Release();
    fullscreen.reset();
    hostWindow.reset();
    removeSubclass();
    SDL_DestroyWindow(window);
    window = nullptr;
    releaseWindowSlot();
}

SDL_Window* DisplayWindow::Handle() const {
    return window;
}

void DisplayWindow::ToggleFullscreen() {
    require(window != nullptr && fullscreen != nullptr, "window must exist before toggling fullscreen");
    fullscreen->Toggle();
    fullscreen->Refresh(MouseIsOpen_nid_no_patch());
}

void DisplayWindow::DrawableSize(std::uint32_t& width, std::uint32_t& height) const {
    if (window == nullptr) {
        width = 0;
        height = 0;
        return;
    }
    int drawableWidth = 0;
    int drawableHeight = 0;
    SDL_Vulkan_GetDrawableSize(window, &drawableWidth, &drawableHeight);
    width = drawableWidth > 0 ? static_cast<std::uint32_t>(drawableWidth) : 0;
    height = drawableHeight > 0 ? static_cast<std::uint32_t>(drawableHeight) : 0;
}

void DisplayWindow::UpdateTitle() {
    require(window != nullptr, "window must exist before updating title");
    static const AppTitle title = GetAppTitle_nid_postfix();
    static auto fpsStart = std::chrono::steady_clock::now();
    static std::uint64_t fpsFrames = 0;
    static double currentFps = 0.0;
    const auto now = std::chrono::steady_clock::now();
    frameNum++;
    fpsFrames++;
    const double elapsed = std::chrono::duration<double>(now - fpsStart).count();
    if (elapsed >= 2.0) {
        currentFps = static_cast<double>(fpsFrames) / elapsed;
        fpsStart = now;
        fpsFrames = 0;
    }
    // The title is laid out by AppKit on every change (about half of the main thread's CPU time per frame when it
    // changed with every frame number), so it follows the counter twice a second.
    static auto titleSet = std::chrono::steady_clock::time_point{};
    if (titleSet == std::chrono::steady_clock::time_point{} || now - titleSet >= std::chrono::milliseconds(500)) {
        titleSet = now;
        char text[512];
        static const std::string name = DisplayWindowTitleName(title.value);
        std::snprintf(text, sizeof(text), "%s%.0f fps | %s (%llu)", DisplayWindowRunTagPrefix().c_str(), currentFps, name.c_str(), static_cast<unsigned long long>(frameNum));
        SDL_SetWindowTitle(window, text);
    }
    if (fullscreen) fullscreen->Refresh(MouseIsOpen_nid_no_patch());
    if (auto* telemetry = VideoOutTelemetry::Instance(); telemetry != nullptr && (frameNum == 1 || frameNum % 30 == 0)) telemetry->EmitPresent(frameNum, currentFps);
}

void DisplayWindow::installSubclass() {
#ifdef _WIN32
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    require(SDL_GetWindowWMInfo(window, &info) == SDL_TRUE, SDL_GetError());
    require(info.subsystem == SDL_SYSWM_WINDOWS, "unsupported window subsystem");
    const auto attached = SetWindowSubclass(info.info.win.window, reinterpret_cast<SUBCLASSPROC>(&DisplayWindow::windowProc), DisplayWindowSubclassId, reinterpret_cast<DWORD_PTR>(this));
    require(attached != FALSE, "SetWindowSubclass failed");
#endif
}

void DisplayWindow::removeSubclass() noexcept {
#ifdef _WIN32
    if (window == nullptr) return;
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (SDL_GetWindowWMInfo(window, &info) != SDL_TRUE) return;
    if (info.subsystem != SDL_SYSWM_WINDOWS) return;
    RemoveWindowSubclass(info.info.win.window, reinterpret_cast<SUBCLASSPROC>(&DisplayWindow::windowProc), DisplayWindowSubclassId);
#endif
}

void DisplayWindow::applyAspectRatio(void* hwnd, std::uintptr_t edge, void* rect) const {
#ifdef _WIN32
    require(aspectWidth != 0 && aspectHeight != 0, "source extent must be non-zero during resize");
    RECT windowBounds{};
    RECT clientBounds{};
    require(GetWindowRect(static_cast<HWND>(hwnd), &windowBounds) != FALSE, "GetWindowRect failed");
    require(GetClientRect(static_cast<HWND>(hwnd), &clientBounds) != FALSE, "GetClientRect failed");
    const auto frameWidth = (windowBounds.right - windowBounds.left) - (clientBounds.right - clientBounds.left);
    const auto frameHeight = (windowBounds.bottom - windowBounds.top) - (clientBounds.bottom - clientBounds.top);
    require(frameWidth >= 0 && frameHeight >= 0, "invalid window frame extent");
    auto* bounds = static_cast<RECT*>(rect);
    const auto clientWidth = bounds->right - bounds->left - frameWidth;
    const auto clientHeight = bounds->bottom - bounds->top - frameHeight;
    require(clientWidth > 0 && clientHeight > 0, "resized client extent must be positive");
    if (edge == WMSZ_TOP || edge == WMSZ_BOTTOM) {
        const auto width = AgcDriver::ComputeWidthForHeight_nid_postfix(aspectWidth, aspectHeight, static_cast<std::uint32_t>(clientHeight));
        bounds->right = bounds->left + static_cast<LONG>(width) + frameWidth;
        return;
    }
    const auto height = AgcDriver::ComputeHeightForWidth_nid_postfix(aspectWidth, aspectHeight, static_cast<std::uint32_t>(clientWidth));
    if (edge == WMSZ_TOPLEFT || edge == WMSZ_TOPRIGHT) bounds->top = bounds->bottom - static_cast<LONG>(height) - frameHeight;
    else bounds->bottom = bounds->top + static_cast<LONG>(height) + frameHeight;
#else
    static_cast<void>(hwnd);
    static_cast<void>(edge);
    static_cast<void>(rect);
#endif
}

std::intptr_t DisplayWindow::windowProc(void* hwnd, unsigned int message, std::uintptr_t wParam, std::intptr_t lParam, std::uintptr_t subclassId, std::uintptr_t referenceData) {
#ifdef _WIN32
    static_cast<void>(subclassId);
    if (message == WM_SIZING) {
        reinterpret_cast<const DisplayWindow*>(referenceData)->applyAspectRatio(hwnd, wParam, reinterpret_cast<void*>(lParam));
        return TRUE;
    }
    return DefSubclassProc(static_cast<HWND>(hwnd), message, static_cast<WPARAM>(wParam), static_cast<LPARAM>(lParam));
#else
    static_cast<void>(hwnd);
    static_cast<void>(message);
    static_cast<void>(wParam);
    static_cast<void>(lParam);
    static_cast<void>(subclassId);
    static_cast<void>(referenceData);
    return 0;
#endif
}
