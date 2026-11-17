#pragma once
// Machine-safety self protection (nps5-safety, after a kernel panic in IOGPUFamily).
//
//  * Timestamp query pool budget: KosmicKrisp makes one Metal counter heap per timestamp query pool; Metal refuses
//    new heaps once the process (or system) holds too many ("too many heaps already allocated"), and Apple's GPU kernel
//    driver then took the whole machine down. TimestampBudget caps the live pools (NPS5_TS_POOL_MAX, default 48) and turns
//    ALL timestamp profiling off for the rest of the run on the first refusal or vkCreateQueryPool failure.
//  * Self memory limit: a 1 s sampler thread reads phys_footprint and exits the process cleanly (exit code 86, log line
//    "[RED] self memory limit") above NPS5_SELF_MEM_LIMIT_MB (0 = off; default: physical memory less the larger of
//    5.5 GiB and a fifth, see DefaultMemoryLimitMiB), before the system runs out of memory with GPU work in flight. The exit goes through the runtime's own
//    request-exit path (LibcRequestExit: shutdown token, guest threads unwind, GPU queues drain); a plain exit() from
//    the sampler thread crashed ("recursive crash") with guest threads still running. A hard _Exit(86) follows after 10 s.
//  * GPU profile block: APS5_PROFILE_GPU / APS5_PROFILE_PASSES are ignored on macOS unless
//    NPS5_ALLOW_GPU_PROFILE=1 (every title: the counter heaps are a property of Apple's driver, not of a game).
#include <cstdint>

namespace AgcDriver::SelfGuard {

// Pure helpers (unit tested).
std::uint32_t ParseLimit(const char* text, std::uint32_t fallback);   // unsigned decimal; null/empty/garbage -> fallback
bool OverLimit(std::uint64_t footprintMiB, std::uint32_t limitMiB);   // limitMiB 0 = never

class TimestampBudget {
public:
    // True and counted when a pool may be created. False when timestamps are off or the budget is spent
    // (the latter switches them off for good, with one log line).
    static bool TryAcquire(const char* where);
    static void Release() noexcept;                                    // a counted pool was destroyed
    static void NoteCreateFailure(const char* where, int vkResult);    // vkCreateQueryPool failed: undo the count, switch off
    static bool Disabled() noexcept;
    static std::uint32_t Live() noexcept;
    static void ResetForTest(std::uint32_t max);
};

bool GpuProfileAllowed(const char* allow, bool applePlatform);   // on Apple platforms only NPS5_ALLOW_GPU_PROFILE "1" allows
bool GpuProfileBlocked();                // macOS: GPU timestamp profiling without NPS5_ALLOW_GPU_PROFILE=1 (logged once)
// Default of NPS5_SELF_MEM_LIMIT_MB for a machine: physical memory less the larger of 5.5 GiB and a fifth; 0 (no
// limit) when the size is unknown or too small for that.
std::uint32_t DefaultMemoryLimitMiB(std::uint64_t physicalBytes);
std::uint64_t PhysicalMemoryBytes();     // 0 when unavailable
std::uint64_t FootprintMiB();            // task phys_footprint; 0 when unavailable
void StartSelfMemoryLimit(void (*requestExit)(int code));   // idempotent; starts the sampler thread when the limit is on

}  // namespace AgcDriver::SelfGuard
