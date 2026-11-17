#pragma once
// Machine-safety self protection (nps5-safety, after the 2026-11-16 kernel panic in IOGPUFamily).
//
//  * Timestamp query pool budget: KosmicKrisp makes one Metal counter heap per timestamp query pool; Metal refuses
//    new heaps once the process (or system) holds too many ("too many heaps already allocated"), and Apple's GPU kernel
//    driver then took the whole Mac down. TimestampBudget caps the live pools (NPS5_TS_POOL_MAX, default 48) and turns
//    ALL timestamp profiling off for the rest of the run on the first refusal or vkCreateQueryPool failure.
//  * Self memory limit: a 1 s sampler thread reads phys_footprint and exits the process cleanly (exit code 86, log line
//    "[RED] self memory limit") above NPS5_SELF_MEM_LIMIT_MB (default 18944 = 18.5 GB, 0 = off), before the machine
//    guard (20 GB) has to SIGKILL a process that still has GPU work in flight. The exit goes through the runtime's own
//    request-exit path (LibcRequestExit: shutdown token, guest threads unwind, GPU queues drain); a plain exit() from
//    the sampler thread crashed ("recursive crash") with guest threads still running. A hard _Exit(86) follows after 10 s.
//  * GPU profile block: APS5_PROFILE_GPU / APS5_PROFILE_PASSES are ignored for ASTRO BOT (NPS5_TITLE_ID=PPSA21564)
//    unless NPS5_ALLOW_GPU_PROFILE=1.
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

bool GpuProfileBlocked();                // ASTRO BOT without NPS5_ALLOW_GPU_PROFILE=1 (logged once)
std::uint64_t FootprintMiB();            // task phys_footprint; 0 when unavailable
void StartSelfMemoryLimit(void (*requestExit)(int code));   // idempotent; starts the sampler thread when the limit is on

}  // namespace AgcDriver::SelfGuard
