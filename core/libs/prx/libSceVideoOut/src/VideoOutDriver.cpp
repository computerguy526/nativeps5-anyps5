#include "prx/libSceVideoOut/include/DisplayWindow.hpp"
#include <bit>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <exception>
#include <thread>
#include "prx/libScePad/include/LatTrace.hpp"
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>

#include "SDL.h"
#include "SDL_vulkan.h"
#include "prx/libc/include/HostThread.hpp"
#include "prx/libSceVideoOut/include/MainThread.hpp"
#include "prx/libSceVideoOut/include/PadInput.hpp"
#include "prx/libSceVideoOut/include/MouseInput.hpp"
#include "prx/libSceAgcDriver/Execution/include/AspectFit.hpp"
#include "prx/libSceAgcDriver/Execution/include/FrameLimit.hpp"
#include "prx/libSceVideoOut/include/KeyboardInput.hpp"
#include "prx/libSceVideoOut/include/ImeTextInput.hpp"
#include "prx/libSceVideoOut/include/DialogInput.hpp"
#include "prx/libScePad/include/PadState.hpp"
#include "prx/libkernel/Equeue/Equeue.hpp"
#include "prx/libkernel/include/Api.hpp"
#include "prx/libkernel/Time/include/Time.hpp"
#include "prx/libkernel/Time/include/SleepWake.hpp"
#include "prx/libSceVideoOut/include/VideoOutDriver.hpp"
#if APS5_ENABLE_TIMING_LOG
#include "prx/libSceVideoOut/include/FrameTimingLog.hpp"
#endif
#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/HostHdr.hpp"
#include "prx/libSceAgcDriver/Execution/include/Presentation.hpp"
#include "prx/libSceIme/include/ImeKeyboardView.hpp"
#include "prx/libSceIme/include/ModalDialog.hpp"
#include "prx/libSceAgcDriver/Execution/include/PerformanceTimer.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libc/include/PreciseWait.hpp"
#include "prx/libc/include/Shutdown.hpp"
#include "prx/libc/include/RunReport.hpp"
#include "prx/libc/include/RunReportComponent.hpp"

APS5_RUN_REPORT_COMPONENT("libSceVideoOut")

namespace {

constexpr std::uint64_t FLIP_ROOM_WAIT_MICROS = 60000000;
constexpr std::uint64_t PRESENT_POLL_MICROS = 10000;
constexpr std::int64_t FRAME_LIMIT_SLICE_NANOS = 20000000;
constexpr std::uint64_t PRESENT_DEPTH_WAIT_MICROS = 250000;

// nps5 (docs/nps5-present.md): APS5_PRESENT_DEPTH=<n> (1..15, 0/unset = off) holds the guest's submission of a
// frame's flip while n flips are already pending (reserved, not yet on screen), so the title runs at most n frames
// ahead of the picture instead of VIDEO_OUT_FLIP_QUEUE_CAPACITY. The hold is bounded (PRESENT_DEPTH_WAIT_MICROS):
// a flip that cannot complete without the title's next submission only costs that wait.
std::size_t presentDepth() {
    static const std::size_t depth = [] {
        const char* value = std::getenv("APS5_PRESENT_DEPTH");
        if (value == nullptr) return std::size_t{0};
        const long parsed = std::strtol(value, nullptr, 10);
        if (parsed >= 0 && parsed < static_cast<long>(VIDEO_OUT_FLIP_QUEUE_CAPACITY)) return static_cast<std::size_t>(parsed);
        std::fprintf(stderr, "[videoout] APS5_PRESENT_DEPTH=%s refused (0..%zu)\n", value, VIDEO_OUT_FLIP_QUEUE_CAPACITY - 1);
        return std::size_t{0};
    }();
    // Measurement aid: APS5_PRESENT_DEPTH_AB=<seconds> applies the depth only in every second period of that
    // length, so one run holds both arms under the same machine load ([pflip] lines carry depth=<active>).
    static const double abSeconds = [] {
        const char* value = std::getenv("APS5_PRESENT_DEPTH_AB");
        return value != nullptr ? std::atof(value) : 0.0;
    }();
    if (abSeconds > 0.0 && depth != 0) {
        static const auto start = std::chrono::steady_clock::now();
        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        if (static_cast<std::uint64_t>(elapsed / abSeconds) % 2 == 0) return 0;
    }
    return depth;
}
std::atomic<std::uint64_t> presentDepthHolds{0}, presentDepthTimeouts{0};

// APS5_PRESENT_DRAIN=1: a flip taken while another GPU-ready flip already stands behind it waits one vblank, not
// its whole flip interval, so frames queued up by a hitch at a 30 fps cap leave the queue instead of each staying
// a frame late for as long as the title holds the cap. Nothing is dropped or shown twice.
bool presentDrain() {
    static const bool on = [] {
        const char* value = std::getenv("APS5_PRESENT_DRAIN");
        return value != nullptr && value[0] != '\0' && value[0] != '0';
    }();
    return on;
}

void require(bool condition, const char* reason) {
    if (!condition) throw std::runtime_error(std::string("VideoOut: ") + reason);
}

std::optional<AgcDriver::FramePacer>& framePacer() {
    static std::optional<AgcDriver::FramePacer> pacer = []() -> std::optional<AgcDriver::FramePacer> {
        const auto& limit = AgcDriver::ConfiguredFrameLimit();
        if (!limit) return std::nullopt;
        return AgcDriver::FramePacer(limit->milliFramesPerSecond);
    }();
    return pacer;
}

bool paceFlip(FlipRequest& req) {
    auto& pacer = framePacer();
    if (!pacer) return true;
    const auto clock = [] { return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); };
    const auto sleep = [&](std::int64_t nanos) {
        PreciseSleepUs(static_cast<unsigned long long>((nanos + 999) / 1000));
        std::lock_guard lock(req.cfg->mutex);
        return !req.cfg->Closed() && !req.cfg->failure;
    };
    return pacer->Pace(clock, sleep, FRAME_LIMIT_SLICE_NANOS);
}

std::uint32_t resolutionIndex(std::uint32_t width, std::uint32_t height) {
    return (width >= 3840 || height >= 2160) ? VIDEO_OUT_RESOLUTION_2160P : VIDEO_OUT_RESOLUTION_1080P;
}

std::optional<std::uint32_t> configuredResolution() {
    const char* value = std::getenv("APS5_OUTPUT_RESOLUTION");
    if (value == nullptr || *value == '\0') return std::nullopt;
    const std::string text(value);
    const char* end = text.data() + text.size();
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    const auto widthResult = std::from_chars(text.data(), end, width);
    const bool separated = widthResult.ec == std::errc{} && widthResult.ptr != end && (*widthResult.ptr == 'x' || *widthResult.ptr == 'X');
    const auto heightResult = separated ? std::from_chars(widthResult.ptr + 1, end, height) : std::from_chars_result{end, std::errc::invalid_argument};
    if (!separated || heightResult.ec != std::errc{} || heightResult.ptr != end || width == 0 || height == 0) {
        throw std::runtime_error("VideoOut: APS5_OUTPUT_RESOLUTION must be <width>x<height> with positive integers, got \"" + text + "\"");
    }
    return resolutionIndex(width, height);
}

bool readHostClipboard(std::string& text) {
    if (SDL_HasClipboardText() != SDL_TRUE) return false;
    char* raw = SDL_GetClipboardText();
    if (raw == nullptr) return false;
    text.assign(raw);
    SDL_free(raw);
    return true;
}

std::vector<const char*> windowExtensions(SDL_Window* window) {
    return OnMainThread([&] {
        unsigned extensionCount = 0;
        if (!SDL_Vulkan_GetInstanceExtensions(window, &extensionCount, nullptr)) throw std::runtime_error(std::string("SDL_Vulkan_GetInstanceExtensions failed: ") + SDL_GetError());
        std::vector<const char*> extensions(extensionCount);
        if (!SDL_Vulkan_GetInstanceExtensions(window, &extensionCount, extensions.data())) throw std::runtime_error(std::string("SDL_Vulkan_GetInstanceExtensions failed: ") + SDL_GetError());
        extensions.resize(extensionCount);
        return extensions;
    });
}

VkSurfaceKHR createWindowSurface(void* context, VkInstance instance) {
    return OnMainThread([&] {
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        if (!SDL_Vulkan_CreateSurface(static_cast<SDL_Window*>(context), instance, &surface)) throw std::runtime_error(std::string("SDL_Vulkan_CreateSurface failed: ") + SDL_GetError());
        return surface;
    });
}

void windowSetStatus(void* context, const char* text) {
    const std::string line = DisplayWindowRunTagPrefix() + text;
    OnMainThread([&] { SDL_SetWindowTitle(static_cast<SDL_Window*>(context), line.c_str()); });
}

// nps5: set when SDL_QUIT was consumed while the window attach (the startup shader prebuild) ran; presentLoop then shuts down at once.
std::atomic<bool> gQuitDuringAttach{false};

// nps5: called every 100 ms by the prebuild: pumps SDL and takes only a pending SDL_QUIT (other events stay queued for the presentation loop).
bool windowPollQuit(void*) {
    const bool quit = OnMainThread([&] {
        SDL_PumpEvents();
        SDL_Event event;
        return SDL_PeepEvents(&event, 1, SDL_GETEVENT, SDL_QUIT, SDL_QUIT) > 0;
    });
    if (quit) gQuitDuringAttach = true;
    return quit;
}

void windowDrawableSize(void* context, std::uint32_t* width, std::uint32_t* height) {
    OnMainThread([&] {
        if ((SDL_GetWindowFlags(static_cast<SDL_Window*>(context)) & SDL_WINDOW_MINIMIZED) != 0) {
            *width = 0;
            *height = 0;
            return;
        }
        int drawableWidth = 0;
        int drawableHeight = 0;
        SDL_Vulkan_GetDrawableSize(static_cast<SDL_Window*>(context), &drawableWidth, &drawableHeight);
        *width = drawableWidth > 0 ? static_cast<std::uint32_t>(drawableWidth) : 0;
        *height = drawableHeight > 0 ? static_cast<std::uint32_t>(drawableHeight) : 0;
    });
}

void checkConfig(const VideoOutConfig& cfg) {
    cfg.Check();
}

void resolveFlipBuffer(FlipRequest& request, const VideoOutConfig& cfg) {
    if (request.index < 0) {
        request.width = cfg.width;
        request.height = cfg.height;
        return;
    }
    request.buffer = cfg.buffers[request.index];
    if (!request.buffer.Occupied()) {
        static std::array<std::atomic<bool>, VIDEO_OUT_BUFFER_NUM_MAX> reported{};
        if (!reported[request.index].exchange(true)) std::fprintf(stderr, "[videoout] GPU flip of unregistered buffer %d on handle %u presents black\n", request.index, request.outputHandle);
        request.unregistered = true;
        request.width = cfg.width;
        request.height = cfg.height;
        return;
    }
    require(request.buffer.groupIndex < VIDEO_OUT_BUFFER_ATTRIBUTE_NUM_MAX, "invalid buffer group");
    if (!request.groupCaptured) request.group = cfg.groups[request.buffer.groupIndex];
    require(request.group.occupied, "buffer group is not registered");
    require(request.buffer.dataAddress != 0, "null registered buffer address");
    static_cast<void>(DescribeVideoOutBuffer(request.buffer, request.group));
    request.width = request.group.attribute.width;
    request.height = request.group.attribute.height;
}

class RenderingWait final : public AgcDriver::IRenderingWait {
    std::shared_ptr<VideoOutConfig> _config;
    std::uint32_t _index;
    std::uint64_t _ticket;
public:
    RenderingWait(std::shared_ptr<VideoOutConfig> config, std::uint32_t index, std::uint64_t ticket)
        : _config(std::move(config)), _index(index), _ticket(ticket) {}
    void Wait() override {
        std::unique_lock lock(_config->mutex);
        _config->vblankCond.wait(lock, _config->shutdownToken, [&] {
            return _config->failure || _config->Closed() ||
                _config->bufferReuse[_index].IsComplete(_ticket);
        });
        _config->CheckAlive();
    }
};

class VideoOutput final : public AgcDriver::IVideoOutput {
public:
    VideoOutput(std::shared_ptr<VideoOutConfig> config, std::shared_ptr<FlipQueue> requests) : cfg(std::move(config)), queue(std::move(requests)) {}

    std::shared_ptr<AgcDriver::IRenderingWait> CaptureRenderingWait(std::uint32_t index) override {
        std::lock_guard lock(cfg->mutex);
        checkConfig(*cfg);
        require(index < VIDEO_OUT_BUFFER_NUM_MAX && cfg->buffers[index].Occupied(), "wait buffer is not registered");
        return std::make_shared<RenderingWait>(cfg, index, cfg->bufferReuse[index].Capture());
    }

    void WaitForFlipRoom() override {
        std::unique_lock queueLock(queue->mutex);
        const bool room = queue->changed.WaitUntil(queueLock, TimedWait::DeadlineNanos(FLIP_ROOM_WAIT_MICROS), [&] {
            return queue->failure || queue->stopping || cfg->shutdownToken.stop_requested() || queue->reservations.load() < VIDEO_OUT_FLIP_QUEUE_CAPACITY;
        });
        if (queue->failure) std::rethrow_exception(queue->failure);
        if (queue->stopping || cfg->shutdownToken.stop_requested()) throw ProcessShutdown{};
        require(room, "flip queue stayed full for 60 s: the presenter is not completing flips");
        const auto depth = presentDepth();
        if (depth == 0 || queue->reservations.load() < depth) return;
        ++presentDepthHolds;
        const bool below = queue->changed.WaitUntil(queueLock, TimedWait::DeadlineNanos(PRESENT_DEPTH_WAIT_MICROS), [&] {
            return queue->failure || queue->stopping || cfg->shutdownToken.stop_requested() || queue->reservations.load() < depth;
        });
        if (!below) ++presentDepthTimeouts;
        if (queue->failure) std::rethrow_exception(queue->failure);
        if (queue->stopping || cfg->shutdownToken.stop_requested()) throw ProcessShutdown{};
    }

    std::shared_ptr<AgcDriver::IFlipRequest> Reserve(const AgcDriver::FlipInfo& info) override { return Reserve(info, false); }
    std::shared_ptr<AgcDriver::IFlipRequest> TryReserve(const AgcDriver::FlipInfo& info) override { return Reserve(info, true); }

    std::shared_ptr<AgcDriver::IFlipRequest> Reserve(const AgcDriver::FlipInfo& info, bool limited) {
        // All flip modes are presented at the next vsync.
        require(info.mode >= VIDEO_OUT_FLIP_MODE_VSYNC && info.mode <= 6, "unsupported flip mode");
        require(info.index >= VIDEO_OUT_BUFFER_INDEX_BLACK && info.index < VIDEO_OUT_BUFFER_NUM_MAX, "invalid flip index");
        auto request = std::make_shared<FlipRequest>();
        request->cfg = cfg;
        request->queue = queue;
        request->index = info.index;
        request->outputHandle = info.handle;
        request->flipMode = static_cast<int>(info.mode);
        request->flipArg = info.argument;
        std::lock_guard queueLock(queue->mutex);
        if (queue->failure) std::rethrow_exception(queue->failure);
        if (queue->stopping || cfg->shutdownToken.stop_requested()) throw ProcessShutdown{};
        if (limited && queue->reservations.load() >= VIDEO_OUT_FLIP_QUEUE_CAPACITY) return {};
        std::lock_guard lock(cfg->mutex);
        checkConfig(*cfg);
        request->generation = cfg->generation;
        request->flipRate = cfg->flipRate;
        if (info.index >= 0) {
            if (limited) require(cfg->buffers[info.index].Occupied(), "EOP flip buffer is not registered");
            request->reuseTicket = cfg->bufferReuse[info.index].Reserve();
        }
        ++queue->reservations;
        static const bool traceReserve = std::getenv("APS5_TRACE_PRESENT") != nullptr;  // only the [pflip] trace reads these stamps
        if (traceReserve) {
            request->reservedAt = std::chrono::steady_clock::now();
            request->pendingAtReserve = queue->reservations.load();
        }
        ++cfg->flipStatus.flipPendingNum;
        if (info.index >= 0) ++cfg->bufferPending[info.index];
        if (info.index >= 0 && cfg->buffers[info.index].Occupied() && cfg->buffers[info.index].groupIndex < VIDEO_OUT_BUFFER_ATTRIBUTE_NUM_MAX && cfg->groups[cfg->buffers[info.index].groupIndex].occupied) {
            request->group = cfg->groups[cfg->buffers[info.index].groupIndex];
            request->groupCaptured = true;
        }
        request->reserved = true;
        return request;
    }

    void Fail(std::exception_ptr error) noexcept override {
        if (!error) std::terminate();
        {
            std::lock_guard lock(queue->mutex);
            if (!queue->failure) queue->failure = error;
        }
        {
            std::lock_guard lock(cfg->mutex);
            if (!cfg->failure) cfg->failure = error;
            cfg->vblankCond.notify_all();
        }
        queue->changed.NotifyAll();
    }

private:
    std::shared_ptr<VideoOutConfig> cfg;
    std::shared_ptr<FlipQueue> queue;
};

}

FlipRequest::~FlipRequest() {
    if (!reserved) return;
    std::lock_guard lock(cfg->mutex);
    ReleaseLocked();
}

void FlipRequest::ReleaseLocked() noexcept {
    if (!reserved || terminal) return;
    --cfg->flipStatus.flipPendingNum;
    --queue->reservations;
    if (index >= 0) {
        --cfg->bufferPending[index];
        cfg->bufferReuse[index].Complete(reuseTicket);
    }
    terminal = true;
    cfg->vblankCond.notify_all();
}

void FlipRequest::Cancel() noexcept {
    {
        std::lock_guard lock(cfg->mutex);
        ReleaseLocked();
    }
    queue->changed.NotifyAll();
}

void FlipRequest::GpuReady(const std::shared_ptr<AgcDriver::FrameTiming>& frameTiming) {
    require(frameTiming != nullptr, "missing frame timing");
    timing = frameTiming;
    {
        AgcDriver::PerformanceContext timingContext(timing.get());
        AgcDriver::PerformanceTimer readiness("VideoOut.Readiness");
        std::lock_guard queueLock(queue->mutex);
        if (queue->failure) std::rethrow_exception(queue->failure);
        if (queue->stopping || cfg->shutdownToken.stop_requested()) throw ProcessShutdown{};
        std::lock_guard lock(cfg->mutex);
        cfg->CheckAlive();
        if (cfg->Closed()) {
            ReleaseLocked();
            return;
        }
        require(reserved && !ready && !terminal && cfg->generation == generation, "invalid flip readiness transition");
        resolveFlipBuffer(*this, *cfg);
        readiness.Mark("locks_validate");
        queuedAt = AgcDriver::FrameTiming::Clock::now();
        queue->requests.push_back(shared_from_this());
        ready = true;
    }
    queue->changed.NotifyAll();
    // The worker is done once the request is queued: hardware does not stall the command processor
    // on a flip, and the title observes completion through flipPendingNum, which processFlip drops
    // once the frame's GPU work and its presentation have completed (see Driver::Present). A
    // presentation failure reaches the worker through ReportFailure at its next packet. Debug aid:
    // APS5_SYNC_FLIP=1 parks the worker until the presenter is done, as before.
    static const bool syncFlip = std::getenv("APS5_SYNC_FLIP") != nullptr;
    if (!syncFlip) return;
    std::unique_lock lock(cfg->mutex);
    cfg->vblankCond.wait(lock, cfg->shutdownToken, [&] { return gpuComplete || cfg->failure || cfg->closing; });
    checkConfig(*cfg);
    require(gpuComplete, "flip preparation did not complete on GPU");
}

void FlipRequest::Fail(std::exception_ptr error) noexcept {
    if (!error) std::terminate();
    std::lock_guard lock(cfg->mutex);
    if (!cfg->failure) cfg->failure = error;
    ReleaseLocked();
    queue->changed.NotifyAll();
}

VideoOutDriver& VideoOutDriver::Get() {
    static VideoOutDriver instance;
    return instance;
}

VideoOutDriver::VideoOutDriver() {
    OnMainThread([] {
        SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
#ifdef _WIN32
        SDL_SetHintWithPriority(SDL_HINT_DIRECTINPUT_ENABLED, "0", SDL_HINT_DEFAULT);
        if (std::getenv("APS5_NO_WGI") != nullptr) SDL_SetHintWithPriority(SDL_HINT_JOYSTICK_WGI, "0", SDL_HINT_DEFAULT);
#endif
#ifdef __linux__
        SDL_SetHint(SDL_HINT_VIDEODRIVER, "wayland,x11");
#endif
        PadInput::ConfigureSdl();
        if (SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) < 0) {
            throw std::runtime_error(std::string("SDL_InitSubSystem(VIDEO | GAMECONTROLLER) failed: ") + SDL_GetError());
        }
    });
    try {
        AgcDriverWaitIdle_nid_postfix();
        std::promise<void> started;
        auto attached = started.get_future();
        presentThread = Libc::StartHostJThread([this, &started](std::stop_token token) {
            try {
                AgcDriverRaiseWorkerThreadPriority_nid_postfix("presenter");
            } catch (...) {
                started.set_exception(std::current_exception());
                return;
            }
            presentLoop(token, started);
        });
        attached.get();
        vblankThread = Libc::StartHostJThread([this](std::stop_token token) { vblankLoop(token); });
        LibcRegisterShutdown_nid_postfix([] { VideoOutDriver::Get().Shutdown(); });
    } catch (...) {
        if (vblankThread.joinable()) {
            vblankThread.request_stop();
            linkBeat.Wake();
            flipQueue->changed.NotifyAll();
            vblankThread.join();
        }
        if (presentThread.joinable()) {
            presentThread.request_stop();
            flipQueue->changed.NotifyAll();
            presentThread.join();
        }
        OnMainThread([] { SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER); });
        throw;
    }
}

VideoOutDriver::~VideoOutDriver() {
    if (!stopped) Shutdown();
}

void VideoOutDriver::Shutdown() {
    require(std::this_thread::get_id() != presentThread.get_id(), "presentation thread cannot stop itself");
    std::lock_guard shutdownLock(shutdownMutex);
    if (stopped) return;
    LibcRequestShutdown_nid_postfix();
    {
        std::lock_guard queueLock(flipQueue->mutex);
        flipQueue->stopping = true;
    }
    presentThread.request_stop();
    vblankThread.request_stop();
    linkBeat.Wake();
    flipQueue->changed.NotifyAll();
    presentThread.join();
    vblankThread.join();
    {
        std::lock_guard lock(mutex);
        for (int handle = 1; handle < VIDEO_OUT_NUM_MAX; ++handle) {
            if (outputs[handle]) close(handle);
        }
    }
    OnMainThread([] { SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER); });
    stopped = true;
    std::lock_guard lock(flipQueue->mutex);
    if (flipQueue->failure) std::rethrow_exception(flipQueue->failure);
}

int VideoOutDriver::Open(int busType) {
    std::unique_lock lock(mutex);
    const int handle = busType + 1;
    std::shared_ptr<VideoOutConfig> previous;
    for (std::shared_ptr<VideoOutConfig> settled;; settled = previous) {
        {
            std::lock_guard queueLock(flipQueue->mutex);
            if (flipQueue->failure) std::rethrow_exception(flipQueue->failure);
            if (flipQueue->stopping || LibcShutdownToken_nid_postfix().stop_requested()) throw ProcessShutdown{};
        }
        require(handle > 0 && handle < VIDEO_OUT_NUM_MAX, "invalid output bus");
        previous = contexts[handle];
        if (!previous) break;
        {
            std::lock_guard cfgLock(previous->mutex);
            if (previous->opened) return -1;
        }
        if (previous == settled) break;
        lock.unlock();
        AgcDriverWaitIdle_nid_postfix();
        lock.lock();
    }
    uint64_t generation = 1;
    if (previous) {
        std::lock_guard cfgLock(previous->mutex);
        require(previous->generation != std::numeric_limits<uint64_t>::max(), "port generation overflow");
        generation = previous->generation + 1;
    }
    auto cfg = std::make_shared<VideoOutConfig>();
    cfg->generation = generation;
    cfg->busType = busType;
    cfg->opened = true;
    cfg->flipStatus.flipArg = -1;
    cfg->flipStatus.currentBuffer = -1;
    auto output = std::make_shared<VideoOutput>(cfg, flipQueue);
    AgcDriverRegisterVideoOutput_nid_postfix(static_cast<uint32_t>(handle), output);
    contexts[handle] = std::move(cfg);
    outputs[handle] = std::move(output);
    return handle;
}

bool VideoOutDriver::Close(int handle) {
    std::lock_guard lock(mutex);
    if (LibcShutdownToken_nid_postfix().stop_requested()) throw ProcessShutdown{};
    return close(handle);
}

bool VideoOutDriver::close(int handle) {
    require(handle > 0 && handle < VIDEO_OUT_NUM_MAX && outputs[handle] != nullptr, "invalid close handle");
    AgcDriverUnregisterVideoOutput_nid_postfix(static_cast<uint32_t>(handle), outputs[handle]);
    outputs[handle].reset();
    auto cfg = contexts[handle];
    std::lock_guard cfgLock(cfg->mutex);
    cfg->opened = false;
    cfg->closing = true;
    const auto removeEvents = [](const auto& events, int kind) {
        for (const auto& event : events) {
            const auto result = EqueueDeleteEvent_nid_postfix(event.eq, static_cast<uintptr_t>(kind), EVFILT_VIDEO_OUT);
            require(result == EQUEUE_OK || result == SCE_KERNEL_ERROR_EBADF || result == SCE_KERNEL_ERROR_ENOENT, "event removal during close failed");
        }
    };
    removeEvents(cfg->flipEvents, VIDEO_OUT_EVENT_FLIP);
    removeEvents(cfg->vblankEvents, VIDEO_OUT_EVENT_VBLANK);
    removeEvents(cfg->preVblankEvents, VIDEO_OUT_EVENT_PRE_VBLANK_START);
    removeEvents(cfg->outputModeEvents, VIDEO_OUT_EVENT_SET_MODE);
    removeEvents(cfg->vrrStatusEvents, VIDEO_OUT_EVENT_VRR_STATUS);
    cfg->flipEvents.clear();
    cfg->vblankEvents.clear();
    cfg->preVblankEvents.clear();
    cfg->outputModeEvents.clear();
    cfg->vrrStatusEvents.clear();
    cfg->vblankCond.notify_all();
    flipQueue->changed.NotifyAll();
    return true;
}

std::shared_ptr<VideoOutConfig> VideoOutDriver::GetConfig(int handle) {
    {
        std::lock_guard lock(flipQueue->mutex);
        if (flipQueue->failure) std::rethrow_exception(flipQueue->failure);
        if (flipQueue->stopping || LibcShutdownToken_nid_postfix().stop_requested()) throw ProcessShutdown{};
    }
    std::lock_guard lock(mutex);
    require(handle > 0 && handle < VIDEO_OUT_NUM_MAX && contexts[handle] != nullptr, "invalid output handle");
    auto cfg = contexts[handle];
    std::lock_guard cfgLock(cfg->mutex);
    checkConfig(*cfg);
    return cfg;
}

bool VideoOutDriver::IsOpen(int handle) {
    return GetConfig(handle) != nullptr;
}

bool VideoOutDriver::HasConfig(int handle) {
    std::shared_ptr<VideoOutConfig> cfg;
    {
        std::lock_guard lock(mutex);
        if (handle <= 0 || handle >= VIDEO_OUT_NUM_MAX || contexts[handle] == nullptr) return false;
        cfg = contexts[handle];
    }
    std::lock_guard cfgLock(cfg->mutex);
    return cfg->opened && !cfg->closing;
}

int VideoOutDriver::GetBusType(int handle) {
    std::shared_ptr<VideoOutConfig> cfg;
    {
        std::lock_guard lock(mutex);
        if (handle <= 0 || handle >= VIDEO_OUT_NUM_MAX || contexts[handle] == nullptr) return -1;
        cfg = contexts[handle];
    }
    std::lock_guard cfgLock(cfg->mutex);
    return cfg->opened && !cfg->closing ? cfg->busType : -1;
}

int VideoOutDriver::SubmitFlip(int handle, int index, int flipMode, int64_t flipArg) {
    if (LibcShutdownToken_nid_postfix().stop_requested()) throw ProcessShutdown{};
    // A title that does not pace on flipPendingNum can run ahead of the presenter now that the queue
    // worker no longer waits per flip; a full queue is the documented error, not a failure.
    if (flipQueue->reservations.load() >= VIDEO_OUT_FLIP_QUEUE_CAPACITY) return VIDEO_OUT_ERROR_FLIP_QUEUE_FULL;
    LatTrace::Submit(static_cast<long long>(flipArg));
    AgcDriverSubmitFlip_nid_postfix(AgcDriver::FlipInfo{static_cast<std::uint32_t>(handle), index, static_cast<std::uint32_t>(flipMode), flipArg});
    return 0;
}

int VideoOutDriver::SubmitEopFlip(int handle, int index, int flipMode, int64_t flipArg, std::uint32_t* token) {
    std::lock_guard lock(mutex);
    if (handle <= 0 || handle >= VIDEO_OUT_NUM_MAX || !outputs[handle]) return VIDEO_OUT_ERROR_INVALID_HANDLE;
    if (flipMode < VIDEO_OUT_FLIP_MODE_VSYNC || flipMode > 6) return VIDEO_OUT_ERROR_INVALID_VALUE;
    if (index < VIDEO_OUT_BUFFER_INDEX_BLACK || index >= VIDEO_OUT_BUFFER_NUM_MAX) return VIDEO_OUT_ERROR_INVALID_INDEX;
    const auto cfg = contexts[handle];
    {
        std::lock_guard cfgLock(cfg->mutex);
        checkConfig(*cfg);
        if (index >= 0 && !cfg->buffers[index].Occupied()) return VIDEO_OUT_ERROR_INVALID_INDEX;
    }
    const AgcDriver::FlipInfo info{static_cast<std::uint32_t>(handle), index, static_cast<std::uint32_t>(flipMode), flipArg};
    LatTrace::Submit(static_cast<long long>(flipArg));
    const auto reserved = AgcDriverReserveEopFlip_nid_postfix(info);
    if (reserved == 0) return VIDEO_OUT_ERROR_FLIP_QUEUE_FULL;
    std::memcpy(token, &reserved, sizeof(reserved));
    return 0;
}

void VideoOutDriver::triggerEvents(VideoOutConfig& cfg, int eventKind, void* triggerData) {
    std::vector<EventRegistration>* events = nullptr;
    if (eventKind == VIDEO_OUT_EVENT_FLIP) events = &cfg.flipEvents;
    else if (eventKind == VIDEO_OUT_EVENT_VBLANK) events = &cfg.vblankEvents;
    else if (eventKind == VIDEO_OUT_EVENT_PRE_VBLANK_START) events = &cfg.preVblankEvents;
    else if (eventKind == VIDEO_OUT_EVENT_SET_MODE) events = &cfg.outputModeEvents;
    else throw std::runtime_error("VideoOut: unknown event kind");
    for (auto it = events->begin(); it != events->end();) {
        require(it->generation == cfg.generation, "stale event registration");
        const auto result = EqueueTriggerEvent_nid_postfix(it->eq, static_cast<uintptr_t>(eventKind), EVFILT_VIDEO_OUT, triggerData);
        if (result == SCE_KERNEL_ERROR_EBADF || result == SCE_KERNEL_ERROR_ENOENT) it = events->erase(it);
        else {
            require(result == EQUEUE_OK, "event delivery failed");
            ++it;
        }
    }
}

bool VideoOutDriver::SupportsHighFrameRate() {
    return GetDisplayRefreshRate() >= 120;
}

int VideoOutDriver::GetDisplayRefreshRate() {
    return displayRefreshRate.load();
}

std::uint32_t VideoOutDriver::GetDisplayResolution() {
    if (const auto configured = configuredResolution()) return *configured;
    return displayResolution.load();
}

void VideoOutDriver::updateDisplayRefreshRate() {
    const int display = SDL_GetWindowDisplayIndex(window.Handle());
    if (display < 0) throw std::runtime_error(std::string("SDL_GetWindowDisplayIndex failed: ") + SDL_GetError());
    SDL_DisplayMode mode{};
    if (SDL_GetCurrentDisplayMode(display, &mode) != 0) throw std::runtime_error(std::string("SDL_GetCurrentDisplayMode failed: ") + SDL_GetError());
    displayRefreshRate.store(mode.refresh_rate);
    displayResolution.store(resolutionIndex(mode.w, mode.h));
    followWindowDisplay();
}

void VideoOutDriver::pollHostHdr() {
    hdrProbe.Poll(window.Handle());
    const auto generation = AgcDriver::CurrentHostHdr().generation;
    if (generation == hdrGeneration) return;
    hdrGeneration = generation;
    std::lock_guard lock(mutex);
    for (const auto& cfg : contexts) {
        if (!cfg) continue;
        std::lock_guard cfgLock(cfg->mutex);
        if (!cfg->opened || cfg->closing || cfg->failure) continue;
        triggerEvents(*cfg, VIDEO_OUT_EVENT_SET_MODE, reinterpret_cast<void*>(cfg->outputMode));
    }
}

void VideoOutDriver::vblankEnd(std::uint64_t tick, std::uint64_t advance) {
    std::lock_guard lock(mutex);
    for (const auto& cfg : contexts) {
        if (!cfg) continue;
        std::lock_guard cfgLock(cfg->mutex);
        if (!cfg->opened || cfg->failure) continue;
        // A 59.94 Hz mode counts the even ticks of (tick - advance, tick].
        const std::uint64_t vblanks = cfg->outputMode == VIDEO_OUT_OUTPUT_MODE_119_88HZ ? advance : tick / 2 - (tick - advance) / 2;
        if (vblanks == 0) continue;
        require(cfg->vblankStatus.count < std::numeric_limits<uint64_t>::max() - vblanks, "vblank counter overflow");
        cfg->preVblankStatus.count += vblanks - 1;
        cfg->vblankStatus.count += vblanks - 1;
        ++cfg->preVblankStatus.count;
        cfg->preVblankStatus.processTime = sceKernelGetProcessTime();
        cfg->preVblankStatus.processTimeCounter = sceKernelGetProcessTimeCounter();
        triggerEvents(*cfg, VIDEO_OUT_EVENT_PRE_VBLANK_START, reinterpret_cast<void*>(cfg->preVblankStatus.count));
        ++cfg->vblankStatus.count;
        cfg->vblankStatus.processTime = sceKernelGetProcessTime();
        cfg->vblankStatus.processTimeCounter = sceKernelGetProcessTimeCounter();
        cfg->vblankStatus.reserved = sceKernelReadTsc();
        static const bool traceClock = std::getenv("APS5_TRACE_FLIP") != nullptr;
        if (traceClock && cfg->vblankStatus.count % 120 == 0) {
            std::fprintf(stderr, "[vblank-clock] #%llu processTime=%llu us processTimeCounter=%llu tsc=%llu => counter/us=%.1f tsc/us=%.1f; freq(process counter)=%llu tscfreq=%llu\n",
                         static_cast<unsigned long long>(cfg->vblankStatus.count), static_cast<unsigned long long>(cfg->vblankStatus.processTime),
                         static_cast<unsigned long long>(cfg->vblankStatus.processTimeCounter), static_cast<unsigned long long>(cfg->vblankStatus.reserved),
                         static_cast<double>(cfg->vblankStatus.processTimeCounter) / static_cast<double>(cfg->vblankStatus.processTime + 1),
                         static_cast<double>(cfg->vblankStatus.reserved) / static_cast<double>(cfg->vblankStatus.processTime + 1),
                         static_cast<unsigned long long>(sceKernelGetProcessTimeCounterFrequency()), static_cast<unsigned long long>(sceKernelGetTscFrequency()));
        }
        triggerEvents(*cfg, VIDEO_OUT_EVENT_VBLANK, reinterpret_cast<void*>(cfg->vblankStatus.count));
        cfg->vblankCond.notify_all();
    }
}

void VideoOutDriver::processFlip(FlipRequest& req) {
    // APS5_TRACE_FLIP=1: one stderr line per completed flip with the wall time spent in each stage.
    static const bool traceFlip = std::getenv("APS5_TRACE_FLIP") != nullptr;
    const auto traceNow = [] { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); };
    const double traceT0 = traceFlip ? traceNow() : 0.0;
    double traceVblank = 0.0, tracePace = 0.0, tracePresent = 0.0;
    AgcDriver::PerformanceContext timingContext(req.timing.get());
    AgcDriver::PerformanceTimer timing("VideoOut.Flip");
    {
        std::unique_lock lock(req.cfg->mutex);
        timing.Mark("config_mutex_wait");
        req.cfg->CheckAlive();
        if (req.cfg->Closed()) {
            req.ReleaseLocked();
            return;
        }
        if (req.cfg->outputMode == VIDEO_OUT_OUTPUT_MODE_119_88HZ && !SupportsHighFrameRate()) {
            req.cfg->outputMode = VIDEO_OUT_OUTPUT_MODE_DEFAULT;
            triggerEvents(*req.cfg, VIDEO_OUT_EVENT_SET_MODE, reinterpret_cast<void*>(req.cfg->outputMode));
            std::fprintf(stderr, "[videoout] display no longer supports 119.88 Hz; fell back to 59.94 Hz\n");
        }
        require(req.ready && !req.terminal && req.generation == req.cfg->generation, "stale or incomplete flip request");
        const auto interval = presentDrain() && req.readyBehind != 0 ? uint64_t{1} : static_cast<uint64_t>(req.flipRate + 1);
        require(req.cfg->lastFlipVblank <= std::numeric_limits<uint64_t>::max() - interval, "flip interval overflow");
        const auto target = req.cfg->lastFlipVblank + interval;
        timing.Mark("validate");
        req.cfg->vblankCond.wait(lock, req.cfg->shutdownToken, [&] { return req.cfg->vblankStatus.count >= target || req.cfg->failure || req.cfg->closing; });
        timing.Mark("vblank_wait");
        if (traceFlip) traceVblank = traceNow();
        req.cfg->CheckAlive();
        if (req.cfg->Closed()) {
            req.ReleaseLocked();
            return;
        }
        req.cfg->lastFlipVblank = req.cfg->vblankStatus.count;
    }
    require(req.width != 0 && req.height != 0 && req.width <= static_cast<uint32_t>(std::numeric_limits<int>::max()) && req.height <= static_cast<uint32_t>(std::numeric_limits<int>::max()), "invalid window dimensions");
    if (req.cfg->busType == VIDEO_OUT_BUS_TYPE_OVERLAY) {
        std::lock_guard lock(req.cfg->mutex);
        req.cfg->CheckAlive();
        if (!req.cfg->Closed()) {
            require(!req.terminal && !req.gpuComplete, "invalid GPU completion transition");
            req.gpuComplete = true;
        }
    } else {
        if (!paceFlip(req)) {
            std::lock_guard lock(req.cfg->mutex);
            req.cfg->CheckAlive();
            if (req.cfg->Closed()) {
                req.ReleaseLocked();
                return;
            }
        }
        timing.Mark("frame_limit");
        if (traceFlip) tracePace = traceNow();
        // The window exists and has this source extent after the first flip: no hop to the main thread then.
        if (window.Handle() == nullptr || req.width != ensuredWidth || req.height != ensuredHeight) {
            OnMainThread([&] { window.Ensure(req.width, req.height); });
            ensuredWidth = req.width;
            ensuredHeight = req.height;
        }
        const auto extensions = windowExtensions(window.Handle());
        const AgcDriver::PresentationWindow target{window.Handle(), extensions, &createWindowSurface, &windowDrawableSize, req.width, req.height, req.timing};
        timing.Mark("window_prepare");
        const auto gpuReady = [](void* context) {
            auto& request = *static_cast<FlipRequest*>(context);
            std::lock_guard lock(request.cfg->mutex);
            request.cfg->CheckAlive();
            if (request.cfg->Closed()) return;
            require(!request.terminal && !request.gpuComplete, "invalid GPU completion transition");
            request.gpuComplete = true;
            request.cfg->vblankCond.notify_all();
        };
        const auto plane = overlayPlane();
        if (req.index >= 0 && !req.unregistered) {
            const auto display = DescribeVideoOutBuffer(req.buffer, req.group);
            AgcDriverPresentBuffer_nid_postfix(target, display, plane ? &*plane : nullptr, gpuReady, &req);
        } else {
            AgcDriverPresentClear_nid_postfix(target, req.unregistered || req.index == VIDEO_OUT_BUFFER_INDEX_BLACK, plane ? &*plane : nullptr, gpuReady, &req);
        }
        timing.Mark("present");
        if (traceFlip) tracePresent = traceNow();
    }
    std::lock_guard lock(req.cfg->mutex);
    timing.Mark("completion_mutex_wait");
    req.cfg->CheckAlive();
    if (req.cfg->Closed()) {
        req.ReleaseLocked();
        return;
    }
    require(!req.terminal && req.cfg->generation == req.generation, "flip cancelled during presentation");
    require(req.gpuComplete, "flip submitted before GPU completion");
    require(req.cfg->flipStatus.count != std::numeric_limits<uint64_t>::max(), "flip counter overflow");
    triggerEvents(*req.cfg, VIDEO_OUT_EVENT_FLIP, reinterpret_cast<void*>(req.flipArg));
    ++req.cfg->flipStatus.count;
    req.cfg->flipStatus.processTime = sceKernelGetProcessTime();
    req.cfg->flipStatus.processTimeCounter = sceKernelGetProcessTimeCounter();
    req.cfg->flipStatus.flipArg = req.flipArg;
    const auto previous = req.cfg->flipStatus.currentBuffer;
    if (previous >= 0) {
        AgcDriverStoreGuestLabel_nid_postfix(reinterpret_cast<std::uint64_t*>(req.cfg->BufferLabelGuest(previous)), 0);
    }
    req.cfg->flipStatus.currentBuffer = req.index;
    req.cfg->width = req.width;
    req.cfg->height = req.height;
    --req.cfg->flipStatus.flipPendingNum;
    --req.queue->reservations;
    if (req.index >= 0) {
        --req.cfg->bufferPending[req.index];
        req.cfg->bufferReuse[req.index].Complete(req.reuseTicket);
    }
    req.terminal = true;
    req.cfg->vblankCond.notify_all();
    KernelWakeSleepers_nid_no_patch();
    timing.Mark("notify_game");
    LatTrace::Done(static_cast<long long>(req.cfg->flipStatus.count));
    // nps5: APS5_TRACE_PRESENT=1: one line per completed flip (host steady clock, ms): when the guest reserved it,
    // then the delays to GPU-ready (the driver replayed the frame), to the presenter taking it and to completion;
    // pend = flips pending at the reserve (frames in flight, this one included), behind = ready flips queued behind.
    static const bool tracePflip = std::getenv("APS5_TRACE_PRESENT") != nullptr;
    if (tracePflip) {
        const auto ms = [](std::chrono::steady_clock::time_point t) { return std::chrono::duration<double, std::milli>(t.time_since_epoch()).count(); };
        const double reserved = ms(req.reservedAt);
        std::fprintf(stderr, "[pflip] n=%llu arg=%lld t=%.2f ready=+%.2f deq=+%.2f done=+%.2f pend=%zu behind=%zu rate=%d vbl=%llu holds=%llu/%llu depth=%zu\n",
                     static_cast<unsigned long long>(req.cfg->flipStatus.count), static_cast<long long>(req.flipArg), reserved, ms(req.queuedAt) - reserved, ms(req.dequeuedAt) - reserved,
                     ms(std::chrono::steady_clock::now()) - reserved, req.pendingAtReserve, req.readyBehind, req.flipRate, static_cast<unsigned long long>(req.cfg->vblankStatus.count),
                     static_cast<unsigned long long>(presentDepthHolds.load()), static_cast<unsigned long long>(presentDepthTimeouts.load()), presentDepth());
    }
    if (traceFlip) {
        const double now = traceNow();
        static double last = now;
        std::fprintf(stderr, "[flip] #%llu t=%.0f dt=%.0f vblank_wait=%.0f frame_limit=%.0f present(gpu_ready)=%.0f\n",
                     static_cast<unsigned long long>(req.cfg->flipStatus.count), now, now - last, traceVblank - traceT0, tracePace > 0 ? tracePace - traceVblank : 0.0, tracePresent > 0 ? tracePresent - tracePace : 0.0);
        last = now;
    }
}

std::optional<AgcDriver::DisplayBuffer> VideoOutDriver::overlayPlane() {
    std::shared_ptr<VideoOutConfig> overlay;
    {
        std::lock_guard lock(mutex);
        overlay = contexts[VIDEO_OUT_BUS_TYPE_OVERLAY + 1];
    }
    if (!overlay) return std::nullopt;
    std::lock_guard lock(overlay->mutex);
    const auto index = overlay->flipStatus.currentBuffer;
    if (overlay->Closed() || index < 0 || index >= VIDEO_OUT_BUFFER_NUM_MAX || !overlay->buffers[index].Occupied()) return std::nullopt;
    const auto& buffer = overlay->buffers[index];
    return DescribeVideoOutBuffer(buffer, overlay->groups[buffer.groupIndex]);
}

void VideoOutDriver::presentLoop(std::stop_token token, std::promise<void>& started) {
    try {
        OnMainThread([&] {
            window.Ensure(VIDEO_OUT_DEFAULT_WIDTH, VIDEO_OUT_DEFAULT_HEIGHT);
            updateDisplayRefreshRate();
        });
        hdrProbe.Poll(window.Handle());
        const auto extensions = windowExtensions(window.Handle());
        AgcDriverAttachWindow_nid_postfix({window.Handle(), extensions, &createWindowSurface, &windowDrawableSize, VIDEO_OUT_DEFAULT_WIDTH, VIDEO_OUT_DEFAULT_HEIGHT, nullptr, &windowSetStatus, &windowPollQuit, token});
        hdrGeneration = AgcDriver::CurrentHostHdr().generation;
    } catch (...) {
        hdrProbe.Release();
        OnMainThread([&] { window.Destroy(); });
        started.set_exception(std::current_exception());
        return;
    }
    started.set_value();
    struct DialogPresenter {
        ~DialogPresenter() { ModalDialogSetPresenter_nid_postfix(false); }
    } dialogPresenter;
    std::shared_ptr<FlipRequest> current;
    std::optional<PadInput> padInput;
    std::optional<MouseInput> mouseInput;
    std::optional<KeyboardInput> keyboardInput;
    std::optional<ImeTextInput> imeTextInput;
    std::optional<DialogInput> dialogInput;
#if APS5_ENABLE_TIMING_LOG
    std::unique_ptr<FrameTimingLog> timingLog;
#endif
    try {
#if APS5_ENABLE_TIMING_LOG
        timingLog = std::make_unique<FrameTimingLog>();
#endif
        OnMainThread([&] {
            padInput.emplace();
            mouseInput.emplace();
            keyboardInput.emplace();
            imeTextInput.emplace();
            dialogInput.emplace();
            imeTextInput->SetClipboardProvider(&readHostClipboard);
        });
        int hopWindowWidth = 0;
        int hopWindowHeight = 0;
        // nps5: APS5_INPUT_HZ=<n> (0 = off): a thread that pumps SDL events and samples the pads n times a second
        // (on the main thread, like the hop below, so it never runs concurrently with it). Without it the pads
        // are sampled once per presentation-loop iteration, and the loop is inside a flip (vblank wait, frame
        // limit, GPU wait) for much of a frame.
        std::atomic<bool> inputQuit{gQuitDuringAttach.exchange(false)};
        std::jthread inputPump;
        {
            const char* hzText = std::getenv("APS5_INPUT_HZ");
            const long hz = hzText != nullptr ? std::strtol(hzText, nullptr, 10) : 0;
            if (hz > 0 && hz <= 2000) {
                std::fprintf(stderr, "[input] APS5_INPUT_HZ=%ld: input pump thread\n", hz);
                inputPump = Libc::StartHostJThread([&, hz](std::stop_token stop) {
                    const auto period = std::chrono::microseconds(1000000 / hz);
                    auto next = std::chrono::steady_clock::now();
                    while (!stop.stop_requested()) {
                        next += period;
                        const auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(next - std::chrono::steady_clock::now()).count();
                        if (remaining > 0) PreciseSleepUs(static_cast<unsigned long long>(remaining));
                        else next = std::chrono::steady_clock::now();
                        if (stop.stop_requested()) break;
                        const bool quit = OnMainThread([&] {
                            SDL_Event event;
                            while (SDL_PollEvent(&event)) {
                                if (event.type == SDL_QUIT) return true;
                                padInput->HandleEvent(event, window);
                                if (window.Handle() != nullptr) {
                                    mouseInput->HandleEvent(event, SDL_GetWindowID(window.Handle()));
                                    keyboardInput->HandleEvent(event, SDL_GetWindowID(window.Handle()));
                                    imeTextInput->HandleEvent(event, SDL_GetWindowID(window.Handle()));
                                    dialogInput->HandleEvent(event, SDL_GetWindowID(window.Handle()));
                                }
                            }
                            padInput->Update(window);
                            return false;
                        });
                        if (quit) { inputQuit = true; break; }
                    }
                });
            }
        }
        while (!token.stop_requested()) {
            {
                std::unique_lock lock(flipQueue->mutex);
                flipQueue->changed.WaitUntil(lock, TimedWait::DeadlineNanos(PRESENT_POLL_MICROS), [&] { return token.stop_requested() || flipQueue->failure || !flipQueue->requests.empty(); });
                if (flipQueue->failure) std::rethrow_exception(flipQueue->failure);
                if (token.stop_requested()) break;
                if (!flipQueue->requests.empty()) {
                    current = std::move(flipQueue->requests.front());
                    flipQueue->requests.pop_front();
                    current->readyBehind = flipQueue->requests.size();
                    current->dequeuedAt = std::chrono::steady_clock::now();
                }
            }

            ModalDialogSetPresenter_nid_postfix(window.Handle() != nullptr);
            pollHostHdr();
            if (current) {
                require(current->timing != nullptr, "missing presentation timing");
                const auto dequeued = AgcDriver::FrameTiming::Clock::now();
                current->timing->Add(current->timing->Get("VideoOut", "queue"), dequeued - current->queuedAt);
                ImeKeyboardView keyboardView;
                const bool keyboardShown = ImeReadKeyboardView_nid_postfix(keyboardView);
                AgcDriverSetSystemOverlay_nid_postfix(keyboardShown ? &keyboardView : nullptr);
                ModalDialogView dialogView;
                const bool dialogShown = !keyboardShown && ModalDialogReadView_nid_postfix(dialogView);
                AgcDriverSetSystemDialog_nid_postfix(dialogShown ? &dialogView : nullptr);
                processFlip(*current);
                // The size read at the top of this iteration; a window created or resized by this very flip is seen
                // one frame later (it only scales mouse coordinates).
                const int windowWidth = hopWindowWidth;
                const int windowHeight = hopWindowHeight;
                if (windowWidth > 0 && windowHeight > 0) {
                    const auto presented = AgcDriver::ComputeContainSize_nid_postfix(current->width, current->height, static_cast<std::uint32_t>(windowWidth), static_cast<std::uint32_t>(windowHeight), true);
                    mouseInput->SetPresentation(current->width, current->height, presented.width, presented.height);
                }
                const auto finished = AgcDriver::FrameTiming::Clock::now();
                AgcDriver::FrameTiming::Clock::duration interval{};
                {
                    std::lock_guard lock(current->cfg->mutex);
                    const auto previous = current->cfg->lastTimingFlip;
                    if (previous != AgcDriver::FrameTiming::Clock::time_point{}) interval = finished - previous;
                    current->cfg->lastTimingFlip = finished;
                }
#if APS5_ENABLE_TIMING_LOG
                timingLog->Enqueue(current->timing->Capture(current->outputHandle, current->index, current->flipArg, finished, interval));
#endif
            }
            const bool flipped = current != nullptr;
            if (current) {
                current.reset();
                flipQueue->changed.NotifyAll();
            }
            const bool quit = OnMainThread([&] {
                SDL_Event event;
                while (SDL_PollEvent(&event)) {
                    if (event.type == SDL_QUIT) return true;
                    padInput->HandleEvent(event, window);
                    if (window.Handle() != nullptr) {
                        mouseInput->HandleEvent(event, SDL_GetWindowID(window.Handle()));
                        keyboardInput->HandleEvent(event, SDL_GetWindowID(window.Handle()));
                        imeTextInput->HandleEvent(event, SDL_GetWindowID(window.Handle()));
                        dialogInput->HandleEvent(event, SDL_GetWindowID(window.Handle()));
                    }
                }
                padInput->Update(window);
                updateDisplayRefreshRate();
                // One hop to the main thread per loop iteration, after the flip was handed to the GPU and the game
                // was notified (it was three before the flip and one inside it, each a wake-up of the main thread
                // the game's flip waited for: about 1.8 ms of a 3.3 ms flip): events, the mouse capture, the window
                // size (used by the next iteration) and the title.
                mouseInput->UpdateCapture(!padInput->MouseModeEnabled());
                if (window.Handle() != nullptr) SDL_GetWindowSize(window.Handle(), &hopWindowWidth, &hopWindowHeight);
                if (flipped && window.Handle() != nullptr) window.UpdateTitle();
                return false;
            });
            if (quit || inputQuit.load()) {
                LibcRunReportQuitRequested_nid_no_patch();
                LibcRequestExit_nid_postfix(0);
                throw ProcessShutdown{};
            }
        }
    } catch (const ProcessShutdown&) {
    } catch (const std::exception& error) {
        std::fprintf(stderr, "[videoout] presentation failed: %s\n", error.what());
        std::fflush(stderr);
        std::terminate();
    } catch (...) {
        std::fprintf(stderr, "[videoout] presentation failed with a non-standard exception\n");
        std::fflush(stderr);
        std::terminate();
    }
#if APS5_ENABLE_TIMING_LOG
    try {
        timingLog->Finish();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "[videoout] frame timing log failed: %s\n", error.what());
        std::fflush(stderr);
        std::terminate();
    }
#endif
    current.reset();
    {
        std::list<std::shared_ptr<FlipRequest>> cancelled;
        {
            std::lock_guard lock(flipQueue->mutex);
            cancelled.swap(flipQueue->requests);
        }
    }
    try {
        AgcDriverShutdown_nid_postfix();
    } catch (...) {
        std::lock_guard lock(flipQueue->mutex);
        if (!flipQueue->failure) flipQueue->failure = std::current_exception();
    }
    hdrProbe.Release();
    OnMainThread([&] {
        padInput.reset();
        mouseInput.reset();
        keyboardInput.reset();
        imeTextInput.reset();
        window.Destroy();
    });
}

namespace {

// nps5: APS5_PRESENT_LINK=1: the vblank beat follows the display the window is on (docs/nps5-present.md).
bool presentLinkRequested() {
    static const bool on = [] {
        const char* value = std::getenv("APS5_PRESENT_LINK");
        return value != nullptr && value[0] != '\0' && value[0] != '0';
    }();
    return on;
}

}

void VideoOutDriver::followWindowDisplay() {
    if (!presentLinkRequested() || window.Handle() == nullptr) return;
    const int index = SDL_GetWindowDisplayIndex(window.Handle());
    if (index < 0 || index == linkDisplayIndex) return;
    SDL_Rect bounds{};
    if (SDL_GetDisplayBounds(index, &bounds) != 0) return;
    const bool following = linkBeat.Follow(bounds.x + bounds.w / 2, bounds.y + bounds.h / 2);
    std::fprintf(stderr, "[videoout] APS5_PRESENT_LINK: display %d %s\n", index, following ? "has a display link" : "has no display link; the vblank beat stays on the host clock");
    linkDisplayIndex = index;
}

// The beat with APS5_PRESENT_LINK=1. While the window's display nominally refreshes at a multiple the guest's
// 59.94/119.88 Hz divides (60 or 120 Hz; 30 Hz), the guest's vblank counter advances at the refreshes the link
// reports, by the host time passed since the last one, so a flip that waits for a vblank is released at the
// screen's own refresh. A display at another rate (75, 144, 50 Hz), a
// link that went silent (window hidden, display asleep) and a missing link fall back to a beat on the host clock.
// Either way a hitch never produces a burst of late vblanks: the counter takes the missed ticks with one event,
// and the host-clock beat then continues on its grid.
void VideoOutDriver::vblankLoopLinked(std::stop_token token) {
    using Clock = std::chrono::steady_clock;
    constexpr std::chrono::nanoseconds tickPeriod{1001ll * 1000000000ll / 120000ll};
    enum class Beat { None, Link, HostClock, Silent };
    Beat reported = Beat::None;
    std::uint64_t tick = 0;
    std::uint64_t seen = linkBeat.Ticks();
    auto anchor = Clock::now();   // host-clock beat: the time of tick `anchorTick`
    std::uint64_t anchorTick = 0;
    bool silent = false;
    bool linked = false;          // the anchor is a display refresh
    try {
        while (!token.stop_requested()) {
            const double period = linkBeat.PeriodSeconds();
            const int perRefresh = DisplayLinkTicksPerRefresh(period);
            std::uint64_t advance = 0;
            Beat beat = Beat::HostClock;
            if (perRefresh != 0 && !silent) {
                beat = Beat::Link;
                const auto timeout = std::max(std::chrono::nanoseconds(static_cast<std::int64_t>(period * 3e9)), std::chrono::nanoseconds(50000000));
                const auto now = linkBeat.WaitPast(seen, timeout);
                if (now == seen) {
                    if (token.stop_requested()) break;
                    silent = true;
                } else {
                    // The refresh gives the instant, the host clock the count: an adaptive-refresh panel skips
                    // refreshes and a 120.00 Hz one is 0.1 % faster than 119.88 Hz, so ticks are not counted.
                    seen = now;
                    if (!linked) {
                        linked = true;
                        anchor = Clock::now() - tickPeriod;
                        anchorTick = tick;
                    }
                    const auto elapsed = (Clock::now() - anchor + tickPeriod / 2) / tickPeriod;
                    const auto target = anchorTick + static_cast<std::uint64_t>(std::max<std::int64_t>(elapsed, 0));
                    if (target <= tick) continue;  // this refresh came before the next guest tick was due
                    advance = target - tick;
                }
                if (silent) {
                    // The host-clock beat takes over from here.
                    linked = false;
                    anchor = Clock::now();
                    anchorTick = tick;
                    continue;
                }
            } else {
                if (silent) beat = Beat::Silent;
                if (linked) {
                    linked = false;
                    anchor = Clock::now();
                    anchorTick = tick;
                }
                const auto next = anchor + tickPeriod * static_cast<std::int64_t>(tick + 1 - anchorTick);
                const auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(next - Clock::now()).count();
                if (remaining > 0) PreciseSleepUs(static_cast<unsigned long long>(remaining));
                const auto elapsed = static_cast<std::uint64_t>(std::max<std::int64_t>((Clock::now() - anchor) / tickPeriod, 0));
                advance = std::max<std::uint64_t>(elapsed + anchorTick > tick ? elapsed + anchorTick - tick : 0, 1);
                if (const auto now = linkBeat.Ticks(); now != seen) {
                    seen = now;
                    silent = false;
                }
            }
            if (beat != reported) {
                reported = beat;
                const double hz = period > 0.0 ? 1.0 / period : 0.0;
                if (beat == Beat::Link) std::fprintf(stderr, "[videoout] vblank beat: display link, %.2f Hz, %d guest tick(s) of 119.88 Hz per refresh\n", hz, perRefresh);
                else if (beat == Beat::Silent) std::fprintf(stderr, "[videoout] vblank beat: host clock (the display link went silent)\n");
                else if (period > 0.0) std::fprintf(stderr, "[videoout] vblank beat: host clock (a %.2f Hz display is no multiple of 59.94 Hz)\n", hz);
                else std::fprintf(stderr, "[videoout] vblank beat: host clock (no display link yet)\n");
            }
            {
                std::lock_guard lock(flipQueue->mutex);
                if (token.stop_requested() || flipQueue->failure) return;
            }
            tick += advance;
            vblankEnd(tick, advance);
        }
    } catch (const std::exception& error) {
        std::fprintf(stderr, "[videoout] vblank failed: %s\n", error.what());
        std::fflush(stderr);
        std::terminate();
    } catch (...) {
        std::fprintf(stderr, "[videoout] vblank failed with a non-standard exception\n");
        std::fflush(stderr);
        std::terminate();
    }
}

void VideoOutDriver::vblankLoop(std::stop_token token) {
    if (presentLinkRequested()) {
        vblankLoopLinked(token);
        return;
    }
    using Frame = std::chrono::duration<int64_t, std::ratio<1001, 120000>>;
    const auto start = std::chrono::steady_clock::now();
    try {
        for (int64_t frame = 1; !token.stop_requested(); ++frame) {
            const auto next = start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(Frame(frame));
            const auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(next - std::chrono::steady_clock::now()).count();
            if (remaining > 0) PreciseSleepUs(static_cast<unsigned long long>(remaining));
            {
                std::lock_guard lock(flipQueue->mutex);
                if (token.stop_requested() || flipQueue->failure) return;
            }
            vblankEnd(static_cast<std::uint64_t>(frame));
        }
    } catch (const std::exception& error) {
        std::fprintf(stderr, "[videoout] vblank failed: %s\n", error.what());
        std::fflush(stderr);
        std::terminate();
    } catch (...) {
        std::fprintf(stderr, "[videoout] vblank failed with a non-standard exception\n");
        std::fflush(stderr);
        std::terminate();
    }
}
