#ifndef CORE_LIBS_PRX_LIBKERNEL_TIME_INCLUDE_TIME_HPP
#define CORE_LIBS_PRX_LIBKERNEL_TIME_INCLUDE_TIME_HPP

#include <cstdint>
#include <cstdlib>
#include "SceTypes.hpp"
#include "prx/libc/include/general/VabiMacros.hpp"

extern "C" {

    std::uint64_t APS5_VABI sceKernelGetProcessTime();
    std::uint64_t APS5_VABI sceKernelGetProcessTimeCounter();
    std::uint64_t APS5_VABI sceKernelGetProcessTimeCounterFrequency();
    std::uint64_t APS5_VABI sceKernelGetTscFrequency();
    std::uint64_t APS5_VABI HostTscFrequency_nid_no_patch();

    // A host runtime that keeps its own guest clocks (nativeps5) installs them here so that everything AnyPS5 stamps for the
    // title (VideoOut flip and vblank status processTimeCounter/processTime/tsc, AMPR timestamps) is in the units and on the
    // timeline the title's sceKernelGetProcessTimeCounter / sceKernelReadTsc return. Without it a title that derives a deadline
    // from a vblank stamp and compares it with its own clock reads (UE's frame pacer) sleeps for the difference of two
    // clocks that run at different rates. Any member may be null (that clock stays AnyPS5's own).
    struct KernelGuestClock {
        std::uint64_t (*processTime)();          // microseconds since process start
        std::uint64_t (*processTimeCounter)();   // ticks at counterFrequency
        std::uint64_t (*counterFrequency)();     // sceKernelGetProcessTimeCounterFrequency
        std::uint64_t (*readTsc)();              // sceKernelReadTsc
        std::uint64_t (*tscFrequency)();         // sceKernelGetTscFrequency
    };
    void KernelSetGuestClock_nid_postfix(const KernelGuestClock* clock);
    int APS5_VABI sceKernelUsleep_nid_postfix(KernelUseconds microseconds);
    int APS5_VABI sceKernelNanosleep(const KernelTimespec* rqtp, KernelTimespec* rmtp);
    int APS5_VABI nanosleep_nid_postfix(const KernelTimespec* rqtp, KernelTimespec* rmtp);
    int APS5_VABI clock_gettime_nid_postfix(int clockId, KernelTimespec* tp);
    int APS5_VABI clock_getres_nid_postfix(int clockId, KernelTimespec* res);
#ifdef _WIN32
    int APS5_VABI ClockTimeWin32ErrorToGuestErrno_nid_no_patch(std::uint32_t error);
#endif
    int APS5_VABI gettimeofday_nid_postfix(KernelTimeval* tv, KernelTimezone* tz);

    // Debug aid: APS5_TRACE_WAITS=1 aggregates blocking waits (event flags, semaphores, condition
    // variables, event queues) per guest call site and prints the sites that wait longest every 3 s,
    // with their timeout rate; a site that times out once per frame is a lost wake-up.
    void KernelTraceWait_nid_postfix(const char* kind, const void* caller, std::uint64_t waitedNanos, bool timedOut);

}

inline bool KernelTraceWaitEnabled() {
    static const bool enabled = std::getenv("APS5_TRACE_WAITS") != nullptr;
    return enabled;
}

#endif
