#ifndef CORE_LIBS_PRX_LIBSCEPAD_INCLUDE_LATTRACE_HPP
#define CORE_LIBS_PRX_LIBSCEPAD_INCLUDE_LATTRACE_HPP

// nps5: APS5_TRACE_LAT=1 timestamps the input-to-photon chain (host steady clock, ms):
//   inject  the scripted pad changed a button (NPS5_VPAD_SCRIPT)
//   event   PadInput saw the SDL button event
//   publish the state reached the Pad HLE (PadPublishInput)
//   read    the guest's scePadRead/ReadState returned the first state with the changed buttons
//   submit  sceVideoOutSubmitFlip (the next kLatFlips flips after that read)
//   done    the flip completed (GPU ready, guest notified)
//   present vkQueuePresentKHR returned
// Each line: "[lat] <stage> t=<ms> +<ms since inject>".

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace LatTrace {

// nps5: guest pad reads (scePadRead/ReadState calls), counted always: scripted taps hold until the title has sampled them.
inline std::atomic<std::uint64_t> gGuestReads{0};

inline bool Enabled() {
    static const bool on = std::getenv("APS5_TRACE_LAT") != nullptr;
    return on;
}
// APS5_TRACE_LAT_AXIS=1: a scripted axis leaving zero is an injection too (a camera stick changes the whole frame).
inline bool AxisInject() {
    static const bool on = std::getenv("APS5_TRACE_LAT_AXIS") != nullptr;
    return on;
}
inline double Now() {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
inline std::atomic<double> gInject{0.0};
inline std::atomic<int> gFlipsLeft{0};     // flips still to log after the read
inline std::atomic<int> gPresentsLeft{0};
inline std::atomic<int> gDoneLeft{0};
inline std::atomic<bool> gWaitPublish{false}, gWaitRead{false};
constexpr int kLatFlips = 4;

inline void Log(const char* stage, double t = 0.0, long long n = -1) {
    if (t == 0.0) t = Now();
    const double inj = gInject.load();
    std::fprintf(stderr, "[lat] %s t=%.2f +%.2f n=%lld\n", stage, t, inj > 0 ? t - inj : -1.0, n);
}
inline void Inject() {
    if (!Enabled()) return;
    gInject.store(Now());
    gWaitPublish = true;
    gWaitRead = false;
    gFlipsLeft = 0; gPresentsLeft = 0; gDoneLeft = 0;
    Log("inject", gInject.load());
}
inline void Event() { if (Enabled() && gInject.load() > 0) Log("event"); }
inline void Publish() {
    if (!Enabled() || !gWaitPublish.exchange(false)) return;
    Log("publish");
    gWaitRead = true;
}
// Called by the guest read path when the returned state carries a changed button (caller decides).
inline void Read(std::uint64_t sampleAgeUs) {
    if (!Enabled() || !gWaitRead.exchange(false)) return;
    Log("read", 0.0, static_cast<long long>(sampleAgeUs));
    gFlipsLeft = kLatFlips; gPresentsLeft = kLatFlips; gDoneLeft = kLatFlips;
}
inline void Submit(long long n) { if (Enabled() && gFlipsLeft.load() > 0 && gFlipsLeft.fetch_sub(1) > 0) Log("submit", 0.0, n); }
inline void Done(long long n) { if (Enabled() && gFlipsLeft.load() < kLatFlips && gDoneLeft.load() > 0 && gDoneLeft.fetch_sub(1) > 0) Log("done", 0.0, n); }
inline void Present() { if (Enabled() && gFlipsLeft.load() < kLatFlips && gPresentsLeft.load() > 0 && gPresentsLeft.fetch_sub(1) > 0) Log("present"); }

}

#endif
