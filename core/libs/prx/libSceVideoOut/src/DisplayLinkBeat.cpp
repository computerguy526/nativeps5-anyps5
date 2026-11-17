#include "prx/libSceVideoOut/include/DisplayLinkBeat.hpp"

#ifdef __APPLE__
#include <CoreGraphics/CoreGraphics.h>
#include <CoreVideo/CVDisplayLink.h>

// CVDisplayLink is deprecated since macOS 15 in favour of NSView/NSScreen display links, which need an AppKit
// run loop on the main thread; the C link has its own thread and still ticks.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

namespace {

CVReturn onRefresh(CVDisplayLinkRef link, const CVTimeStamp*, const CVTimeStamp* output, CVOptionFlags, CVOptionFlags*, void* context) {
    // The nominal period: the measured one moves by 10 % from second to second on an adaptive-refresh panel.
    double period = 0.0;
    if (const CVTime nominal = CVDisplayLinkGetNominalOutputVideoRefreshPeriod(link); (nominal.flags & kCVTimeIsIndefinite) == 0 && nominal.timeScale != 0) period = static_cast<double>(nominal.timeValue) / static_cast<double>(nominal.timeScale);
    if (period <= 0.0 && output != nullptr && output->videoTimeScale != 0) period = static_cast<double>(output->videoRefreshPeriod) / static_cast<double>(output->videoTimeScale);
    static_cast<DisplayLinkBeat*>(context)->Tick(period);
    return kCVReturnSuccess;
}

}

bool DisplayLinkBeat::Follow(int x, int y) {
    CGDirectDisplayID found = 0;
    std::uint32_t count = 0;
    if (CGGetDisplaysWithPoint(CGPointMake(static_cast<CGFloat>(x), static_cast<CGFloat>(y)), 1, &found, &count) != kCGErrorSuccess || count == 0) found = CGMainDisplayID();
    std::lock_guard lock(mutex);
    if (link != nullptr && found == display) return true;
    if (link == nullptr) {
        CVDisplayLinkRef created = nullptr;
        if (CVDisplayLinkCreateWithCGDisplay(found, &created) != kCVReturnSuccess || created == nullptr) return false;
        if (CVDisplayLinkSetOutputCallback(created, &onRefresh, this) != kCVReturnSuccess || CVDisplayLinkStart(created) != kCVReturnSuccess) {
            CVDisplayLinkRelease(created);
            return false;
        }
        link = created;
    } else if (CVDisplayLinkSetCurrentCGDisplay(static_cast<CVDisplayLinkRef>(link), found) != kCVReturnSuccess) {
        return false;
    }
    display = found;
    period.store(0.0, std::memory_order_release);
    return true;
}

void DisplayLinkBeat::Stop() {
    void* stopping = nullptr;
    {
        std::lock_guard lock(mutex);
        stopping = link;
        link = nullptr;
    }
    // Outside the mutex: CVDisplayLinkStop waits for a running callback, and the callback takes the mutex.
    if (stopping != nullptr) {
        CVDisplayLinkStop(static_cast<CVDisplayLinkRef>(stopping));
        CVDisplayLinkRelease(static_cast<CVDisplayLinkRef>(stopping));
    }
    Wake();
}

#pragma clang diagnostic pop

#else

bool DisplayLinkBeat::Follow(int, int) { return false; }
void DisplayLinkBeat::Stop() { Wake(); }

#endif

DisplayLinkBeat::~DisplayLinkBeat() { Stop(); }

void DisplayLinkBeat::Tick(double refreshPeriodSeconds) {
    {
        std::lock_guard lock(mutex);
        if (refreshPeriodSeconds > 0.0) period.store(refreshPeriodSeconds, std::memory_order_release);
        ticks.fetch_add(1, std::memory_order_release);
    }
    changed.notify_all();
}

std::uint64_t DisplayLinkBeat::WaitPast(std::uint64_t seen, std::chrono::nanoseconds timeout) {
    std::unique_lock lock(mutex);
    changed.wait_for(lock, timeout, [&] { return woken || ticks.load(std::memory_order_acquire) != seen; });
    woken = false;
    return ticks.load(std::memory_order_acquire);
}

void DisplayLinkBeat::Wake() {
    {
        std::lock_guard lock(mutex);
        woken = true;
    }
    changed.notify_all();
}
