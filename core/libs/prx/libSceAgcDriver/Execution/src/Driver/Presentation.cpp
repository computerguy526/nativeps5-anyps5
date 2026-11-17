#include "prx/libSceAgcDriver/Execution/include/ProfileOutput.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Diagnostics.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Queues/WorkerAffinity.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/PipelineCreateStats.hpp"
#include "prx/libSceAgcDriver/Execution/include/PerformanceTimer.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <optional>
#include <thread>
#include "prx/libSceAgcDriver/Graphics/include/PipelineJournal.hpp"
#include "prx/libSceAgcDriver/Execution/include/TracyZone.hpp"
#include <cstdlib>
#include "prx/libSceAgcDriver/Execution/include/ProgressScreen.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"

namespace AgcDriver::DriverDetail {

std::uint32_t Driver::ReserveEopFlip(const FlipInfo& info) {
    std::lock_guard lock(mutex);
    require(nextEopToken <= EopFlipTokenLimit, "EOP flip token space exhausted");
    return reserveEopFlip(info, nextEopToken);
}

std::uint32_t Driver::ReserveEopFlipAt(const FlipInfo& info, std::uint32_t token) {
    std::lock_guard lock(mutex);
    return reserveEopFlip(info, token);
}

std::uint32_t Driver::reserveEopFlip(const FlipInfo& info, std::uint32_t token) {
    rethrowFailure();
    checkStopping();
    require(token >= EopFlipTokenBase && token <= EopFlipTokenLimit, "EOP flip token is out of range");
    const auto output = outputs.find(info.handle);
    require(output != outputs.end(), "EOP flip references an unregistered video output");
    if (const auto found = eopFlips.find(token); found != eopFlips.end()) {
        const auto& reserved = found->second.info;
        require(reserved.handle == info.handle && reserved.index == info.index && reserved.mode == info.mode && reserved.argument == info.argument, "EOP flip token is reserved for a different flip");
        throw EopFlipAlreadyReserved{};
    }
    auto request = output->second->TryReserve(info);
    if (!request) return 0;
    eopFlips.emplace(token, EopFlip{info, std::move(request)});
    nextEopToken = std::max(nextEopToken, token + 1);
    return token;
}

std::vector<std::uint32_t> Driver::PendingEopFlipTokens() {
    std::lock_guard lock(mutex);
    std::vector<std::uint32_t> tokens;
    for (const auto& [token, flip] : eopFlips) tokens.push_back(token);
    return tokens;
}

void Driver::RegisterVideoOutput(std::uint32_t handle, const std::shared_ptr<IVideoOutput>& output) {
    require(output != nullptr, "null video output");
    std::lock_guard lock(mutex);
    rethrowFailure();
    checkStopping();
    require(outputs.emplace(handle, output).second, "video output already registered");
}

void Driver::UnregisterVideoOutput(std::uint32_t handle, const std::shared_ptr<IVideoOutput>& output) {
    std::lock_guard lock(mutex);
    const auto it = outputs.find(handle);
    require(it != outputs.end() && it->second == output, "video output registration mismatch");
    std::erase_if(eopFlips, [handle](const auto& entry) { return entry.second.info.handle == handle; });
    outputs.erase(it);
}

// nps5: before the guest's first frame, build every pipeline of the journal that the cache does not hold yet, on the whole pool
// (the replay runs with Now priority while the prebuild mode is on). Progress goes to the log and to the window title as
// "Compiling shaders  N / M  [#####-----]". NPS5_PREBUILD=0 skips it; NPS5_PREBUILD_SECONDS (default 120) bounds it: the game then starts
// and the replay goes on in the background. A warm cache finishes in well under a second and shows nothing.
// nps5: one frame of the progress screen through the presenter's own path (resize, acquire, PresentClear with the progress layout, queue present). Runs on the thread
// that attaches the window (the guest has not drawn a frame yet, so nothing else presents). NPS5_PREBUILD_SCREEN=0 leaves it to the window title. A minimized window or a dropped frame just skips this frame; only repeated hard failures (kMaxProgressFailures in a row) turn the screen off.
static bool ShowProgressFrame(const PresentationWindow& window, VulkanDevice& dev, const ProgressScreenInfo& info) {
    static constexpr int kMaxProgressFailures = 5;
    static thread_local int failures = 0;
    try {
        std::uint32_t width = 0, height = 0;
        window.getDrawableSize(window.context, &width, &height);
        if (width == 0 || height == 0) return true;  // minimized: skip this frame
        bool presentable = false;
        {
            std::lock_guard lock(GuestMemory::GpuMutex());
            dev.Resize(width, height);
            presentable = dev.Presentable();
            if (presentable) dev.SetProgressScreen(info);
        }
        if (!presentable) return true;
        dev.RetirePresents(0);
        if (!dev.AcquireImage()) return true;
        bool submitted = false;
        {
            std::lock_guard lock(GuestMemory::GpuMutex());
            submitted = dev.PresentClear(window.width, window.height, true, nullptr);
        }
        if (!submitted) return true;
        if (dev.PresentsOnOwnQueue()) {
            dev.QueuePresent();
        } else {
            std::lock_guard lock(GuestMemory::GpuMutex());
            dev.QueuePresent();
        }
        dev.FinishPresent();
        failures = 0;
        return true;
    } catch (const std::exception& error) {
        if (++failures < kMaxProgressFailures) {
            std::fprintf(stderr, "[prebuild] progress frame skipped: %s\n", error.what());
            return true;
        }
        std::fprintf(stderr, "[prebuild] progress screen off after %d failures: %s\n", failures, error.what());
        return false;
    }
}

static void PrebuildPipelines(const PresentationWindow& window, const std::shared_ptr<VulkanDevice>& dev) {
    static const bool screenOn = [] { const char* value = std::getenv("NPS5_PREBUILD_SCREEN"); return value == nullptr || std::strcmp(value, "0") != 0; }();
    bool screen = screenOn && dev != nullptr && window.getDrawableSize != nullptr;
    double rateBase = -1.0;
    std::uint64_t rateDone = 0;
    double lastFrame = -1.0;
    std::uint64_t lastFrameDone = ~0ull;
    const char* limitText = std::getenv("NPS5_PREBUILD_SECONDS");
    const double limit = limitText != nullptr ? std::strtod(limitText, nullptr) : 120.0;
    const auto begin = std::chrono::steady_clock::now();
    const auto elapsed = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count(); };
    double lastLog = 0.0;
    bool shown = false;
    for (;;) {
        const auto progress = Graphics::PipelineJournalProgress();
        const double now = elapsed();
        if (progress.finished) break;
        if (window.stop.stop_requested()) break;
        if (window.pollQuit != nullptr && window.pollQuit(window.context)) {
            std::fprintf(stderr, "[prebuild] window closed during the prebuild: stopping it, the process exits\n");
            break;
        }
        if (!Graphics::PipelineJournalReplayActive() && now > 2.0) break;  // no replay (journal off, or another device)
        if (now >= limit) {
            std::fprintf(stderr, "[prebuild] stopped after %.0f s at %llu / %llu (NPS5_PREBUILD_SECONDS); the game starts, the rest builds in the background\n", now, static_cast<unsigned long long>(progress.done), static_cast<unsigned long long>(progress.total));
            break;
        }
        if (progress.loaded && progress.total != 0 && now > 0.5) {
            if (screen && (progress.done != lastFrameDone || now - lastFrame >= 1.0)) {
                if (rateBase < 0.0 && progress.done != 0) {
                    rateBase = now;
                    rateDone = progress.done;
                }
                double eta = -1.0;
                if (rateBase >= 0.0 && now - rateBase >= 1.0 && progress.done > rateDone) eta = static_cast<double>(progress.total - std::min(progress.total, progress.done)) * (now - rateBase) / static_cast<double>(progress.done - rateDone);
                ProgressScreenInfo info;
                info.done = progress.done;
                info.total = progress.total;
                info.etaSeconds = eta;
                lastFrame = now;
                lastFrameDone = progress.done;
                screen = ShowProgressFrame(window, *dev, info);
            }
            if (window.setStatus != nullptr) {
                const std::size_t cells = 20;
                const std::size_t filled = static_cast<std::size_t>(cells * progress.done / progress.total);
                char text[96];
                std::snprintf(text, sizeof(text), "Shaders %llu/%llu [%s%s]", static_cast<unsigned long long>(progress.done), static_cast<unsigned long long>(progress.total), std::string(filled, '#').c_str(), std::string(cells - filled, '-').c_str());
                window.setStatus(window.context, text);
            }
            if (now - lastLog >= 2.0) {
                lastLog = now;
                shown = true;
                std::fprintf(stderr, "[prebuild] Compiling shaders  %llu / %llu (%.0f s)\n", static_cast<unsigned long long>(progress.done), static_cast<unsigned long long>(progress.total), now);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    const auto progress = Graphics::PipelineJournalProgress();
    std::fprintf(stderr, "[prebuild] %llu / %llu pipelines in %.1f s before the first frame%s\n", static_cast<unsigned long long>(progress.done), static_cast<unsigned long long>(progress.total), elapsed(), shown ? "" : " (nothing to compile)");
    if (shown && window.setStatus != nullptr) window.setStatus(window.context, "Starting");
}

namespace {
// nps5: leaves the prebuild mode and clears the progress screen on every exit path of AttachWindow (including exceptions).
struct PrebuildGuard {
    VulkanDevice* dev = nullptr;
    bool screenSafe = false;  // the GPU mutex is not held by the owner
    ~PrebuildGuard() {
        try {
            if (dev != nullptr && screenSafe) {
                std::lock_guard lock(GuestMemory::GpuMutex());
                dev->ClearProgressScreen();
            }
        } catch (...) {
        }
        Graphics::SetPipelinePrebuildMode(false);
    }
};
}

void Driver::AttachWindow(const PresentationWindow& window) {
    CheckFailure();
    std::unique_lock replacing(deviceReplacement);
    GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Present);
    PerformanceTimer timing("Driver.AttachWindow");
    std::unique_lock lock(GuestMemory::GpuMutex());
    timing.Mark("gpu_mutex_wait");
    static const bool prebuildOn = [] { const char* value = std::getenv("NPS5_PREBUILD"); return value == nullptr || std::strcmp(value, "0") != 0; }();
    const bool prebuild = prebuildOn && Graphics::PipelineJournalEnabled();
    std::optional<PrebuildGuard> prebuildGuard;
    if (prebuild) {
        Graphics::SetPipelinePrebuildMode(true);
        prebuildGuard.emplace();
    }
    if (device) {
        require(device->Window() == nullptr, "a window is already attached to the device");
        device->PrepareForReplacement();
        replacedDevices.push_back(device);
        timing.Mark("device_replacement");
    }
    {
        Graphics::GpuStallScope stall;  // vkCreateDevice builds Metal pipelines: up to ~1 s on a cold OS cache
        device = std::make_shared<VulkanDevice>(&window);
    }
    timing.Mark("vulkan_device_create");
    if (!prebuild) return;
    lock.unlock();  // the replay's builds never take the GPU mutex; the guest has not drawn a frame yet
    const auto prebuildDevice = device.Load();
    prebuildGuard->dev = prebuildDevice.get();
    prebuildGuard->screenSafe = true;
    PrebuildPipelines(window, prebuildDevice);
    prebuildGuard.reset();
    timing.Mark("prebuild");
}

void Driver::TracePresent(std::uint32_t width, std::uint32_t height) {
    if (PerformanceTrace::Configured()) PerformanceTrace::Present(width, height, device.Load().get());
}

void Driver::Present(const PresentationWindow& window, const DisplayBuffer* buffer, const DisplayBuffer* plane, bool opaque, void (*gpuReady)(void*), void* context) {
    if (PerformanceTrace::Configured()) PerformanceTrace::Present(window.width, window.height, device.Load().get());
    PerformanceContext timingContext(window.timing.get());
    PerformanceTimer timing("Driver.Present");
    static thread_local bool pinned = false;
    if (!pinned) {
        pinned = true;
        PinWorkerThread("presenter");
        NameDriverThread("presenter");
        GuestMemory::MarkPresenterThread();
    }
    CheckFailure();
    require(gpuReady != nullptr && context != nullptr, "missing GPU completion callback");
    require(window.getDrawableSize != nullptr, "missing window drawable size query");

    static const bool syncFlip = std::getenv("APS5_SYNC_FLIP") != nullptr;
    static const std::size_t inFlight = VulkanDevice::FlipInFlight();
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    std::shared_ptr<VulkanDevice> presenting;
    timing.Mark("validate");
    try {
        bool submitted = false;
        bool presentable = false;
        bool trailing = false;
        bool queueUnlocked = false;
        double waitedMs = 0;
        {
            GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Present);
            std::lock_guard lock(GuestMemory::GpuMutex());
            timing.Mark("gpu_mutex_wait");
            require(device != nullptr && device->Window() == window.context, "presentation window is not attached to the device");
            presenting = device;
            timing.Mark("device_setup");
            std::uint32_t drawableWidth = 0;
            std::uint32_t drawableHeight = 0;
            window.getDrawableSize(window.context, &drawableWidth, &drawableHeight);
            presenting->Resize(drawableWidth, drawableHeight);
            presenting->SelectDynamicRange(buffer);
            timing.Mark("resize");
            presentable = presenting->Presentable();
            if (buffer != nullptr) require(buffer->width == window.width && buffer->height == window.height, "display buffer extent differs from output");
        }

        if (presentable && !syncFlip) {
            if (inFlight != 0) {

                waitedMs = presenting->RetirePresents(presenting->PresentWaitsForSlots(buffer) ? 0 : inFlight);
                timing.Mark("inflight_wait");
            }
            presentable = presenting->AcquireImage();
            timing.Mark("acquire_image");
        }
        if (presentable) {
            GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Present);
            std::lock_guard lock(GuestMemory::GpuMutex());
            timing.Mark("gpu_mutex_wait");
            if (buffer != nullptr) {
                if (syncFlip) {
                    presenting->WaitIdle();
                    timing.Mark("device_idle_wait");
                }
                submitted = presenting->PresentDisplayBuffer(*buffer, plane);
                timing.Mark("present_display_buffer");
            } else {
                submitted = presenting->PresentClear(window.width, window.height, opaque, plane);
                timing.Mark("present_clear");
            }
            if (submitted && syncFlip) {
                waitedMs = presenting->FinishPresent();
                timing.Mark("render_fence_wait");
            }
            if (submitted && (syncFlip || inFlight != 0)) {

                if (presenting->PresentsOnOwnQueue()) {
                    queueUnlocked = true;
                } else {
                    presenting->QueuePresent();
                    timing.Mark("queue_present");
                }
                trailing = !syncFlip;
                submitted = false;
            }
        }
        if (queueUnlocked) {
            presenting->QueuePresent();
            timing.Mark("queue_present");
        }
        if (trailing) {
            waitedMs += presenting->FinishPresent();
            timing.Mark("render_fence_wait");
        }
        if (submitted) {
            waitedMs = presenting->FinishPresent();
            timing.Mark("render_fence_wait");

            if (presenting->PresentsOnOwnQueue()) {
                presenting->QueuePresent();
            } else {
                GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Present);
                std::lock_guard lock(GuestMemory::GpuMutex());
                presenting->QueuePresent();
            }
            timing.Mark("queue_present");
        }
        gpuReady(context);
        timing.Mark("release_and_callback");
        TracyZone::MarkFrame();
        if (profile) reportPresents(waitedMs, inFlight);
        CheckFailure();
    } catch (const ProcessShutdown&) {
        throw;
    } catch (...) {
        ReportFailure(std::current_exception());
        throw;
    }
}

void Driver::reportPresents(double waitedMs, std::size_t inFlight) {
    static std::vector<double> waits;
    static VulkanDevice::PresentStatistics previous{};
    static std::uint64_t previousFlips = 0, windowSerial = 0, previousUnsignaled = 0;
    static auto lastReport = std::chrono::steady_clock::now();
    waits.push_back(waitedMs);
    if (waits.size() == 1) windowSerial = flipSerial.load();
    const auto now = std::chrono::steady_clock::now();
    if (now - lastReport < std::chrono::seconds(10)) return;
    lastReport = now;
    std::sort(waits.begin(), waits.end());
    const auto percentile = [&](double fraction) { return waits.empty() ? 0.0 : waits[std::min(waits.size() - 1, static_cast<std::size_t>(fraction * static_cast<double>(waits.size())))]; };
    const auto counts = VulkanDevice::PresentCounts();
    const auto presents = counts.presents - previous.presents;
    const double perPresent = presents != 0 ? 1.0 / static_cast<double>(presents) : 0.0;
    const auto dropped = counts.acquireDrops - previous.acquireDrops;
    const double perFrame = presents + dropped != 0 ? 1.0 / static_cast<double>(presents + dropped) : 0.0;
    const auto flips = flipsCounted.load() - previousFlips;
    const double perFlip = flips != 0 ? 1.0 / static_cast<double>(flips) : 0.0;
    const auto serial = flipSerial.load();
    AgcDriver::ProfilePrint_nid_no_patch( "[present] %llu presents over 10 s (%llu dropped: no free swapchain image; %zu may trail on the GPU); %s p50 %.2f ms, p90 %.2f ms, max %.2f ms over %zu; per frame, presented or dropped: GPU busy %.1f ms, idle gaps %.1f ms (%llu batches without a completion record); per flip: %.1f batches recorded (%.1f unsignaled at the flip); per present: %.1f batches ahead of the blit, %.1f after it; after the flip packet: last submit %.1f ms, blit submit %.1f ms; in-place reads overwritten before execution: %llu of %llu checked; waits for the display keys' writer outside the GPU mutex: %llu, %.1f ms\n", static_cast<unsigned long long>(presents), static_cast<unsigned long long>(dropped), inFlight, inFlight != 0 ? "inflight_wait" : "render_fence_wait", percentile(0.5), percentile(0.9), waits.empty() ? 0.0 : waits.back(), waits.size(), (counts.gpuBusyMs - previous.gpuBusyMs) * perFrame, (counts.gpuGapMs - previous.gpuGapMs) * perFrame, static_cast<unsigned long long>(counts.gpuUnread - previous.gpuUnread), static_cast<double>(serial - windowSerial) * perFlip, static_cast<double>(flipBatchesUnsignaled.load() - previousUnsignaled) * perFlip, static_cast<double>(counts.batchesAheadOfBlit - previous.batchesAheadOfBlit) * perPresent, static_cast<double>(counts.batchesAfterFlip - previous.batchesAfterFlip) * perPresent, (counts.lastSubmitAfterFlipMs - previous.lastSubmitAfterFlipMs) * perPresent, (counts.blitSubmitAfterFlipMs - previous.blitSubmitAfterFlipMs) * perPresent, static_cast<unsigned long long>(counts.readsOverwritten - previous.readsOverwritten), static_cast<unsigned long long>(counts.readsChecked - previous.readsChecked), static_cast<unsigned long long>(counts.keyWriterWaits - previous.keyWriterWaits), counts.keyWriterWaitMs - previous.keyWriterWaitMs);
    previous = counts;
    previousFlips = flipsCounted.load();
    previousUnsignaled = flipBatchesUnsignaled.load();
    waits.clear();
}

void Driver::ReleaseWindow(void* window) {
    std::lock_guard lock(GuestMemory::GpuMutex());
    if (device && device->Window() == window) device.Reset();
}

}
