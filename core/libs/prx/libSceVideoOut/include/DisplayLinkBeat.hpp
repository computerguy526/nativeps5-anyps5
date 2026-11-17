#ifndef CORE_LIBS_PRX_LIBSCEVIDEOOUT_DISPLAYLINKBEAT_HPP
#define CORE_LIBS_PRX_LIBSCEVIDEOOUT_DISPLAYLINKBEAT_HPP

// nps5: the host display's refresh ticks (CVDisplayLink), for APS5_PRESENT_LINK=1: the guest's vblank beat then
// follows the screen the window is on instead of a free-running host timer (see docs/nps5-present.md).
// Everything is a no-op that reports "no link" off macOS.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

class DisplayLinkBeat {
public:
    DisplayLinkBeat() = default;
    ~DisplayLinkBeat();
    DisplayLinkBeat(const DisplayLinkBeat&) = delete;
    DisplayLinkBeat& operator=(const DisplayLinkBeat&) = delete;

    // Starts (or moves) the link on the display that contains this point of the global desktop (SDL's display
    // bounds use the same coordinates). False when the host has no link for it.
    bool Follow(int x, int y);
    void Stop();

    std::uint64_t Ticks() const { return ticks.load(std::memory_order_acquire); }
    // The display's nominal refresh period in seconds; 0 until the link reported one.
    double PeriodSeconds() const { return period.load(std::memory_order_acquire); }
    // Returns Ticks() once it differs from `seen`, or `seen` after the timeout (a silent link) or a Wake().
    std::uint64_t WaitPast(std::uint64_t seen, std::chrono::nanoseconds timeout);
    void Wake();

    // Called by the link's own thread, once per refresh.
    void Tick(double refreshPeriodSeconds);

private:
    std::mutex mutex;
    std::condition_variable changed;
    std::atomic<std::uint64_t> ticks{0};
    std::atomic<double> period{0.0};
    bool woken = false;
    void* link = nullptr;          // CVDisplayLinkRef
    std::uint32_t display = 0;     // CGDirectDisplayID the link follows
};

// How many 119.88 Hz guest ticks one refresh of a display with this period is: 1 (120 Hz), 2 (60 Hz), 4 (30 Hz);
// 0 when the display's rate is no such multiple (75 Hz, 144 Hz, 50 Hz), so the guest beat cannot follow it.
inline int DisplayLinkTicksPerRefresh(double periodSeconds) {
    if (periodSeconds <= 0.0) return 0;
    const double ratio = periodSeconds * 120000.0 / 1001.0;
    for (const int k : {1, 2, 4}) {
        if (ratio >= k * 0.98 && ratio <= k * 1.02) return k;
    }
    return 0;
}

#endif
