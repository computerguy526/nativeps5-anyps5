#include "prx/libSceAgcDriver/Execution/include/SelfGuard.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#if defined(__APPLE__)
#include <mach/mach.h>
#include <unistd.h>
#endif

namespace AgcDriver::SelfGuard {
namespace {
std::atomic<std::uint32_t> g_live{0};
std::atomic<std::uint32_t> g_max{0};   // 0 = read the environment on first use
std::atomic<bool> g_disabled{false};

std::uint32_t Max() {
    auto m = g_max.load(std::memory_order_relaxed);
    if (m == 0) {
        m = ParseLimit(std::getenv("NPS5_TS_POOL_MAX"), 48);
        if (m == 0) m = 0xffffffffu;   // NPS5_TS_POOL_MAX=0 : no budget
        g_max.store(m, std::memory_order_relaxed);
    }
    return m;
}

void DisableOnce(const char* why, const char* where, std::uint32_t live) {
    if (!g_disabled.exchange(true))
        std::fprintf(stderr, "[RED] gpu timestamp profiling OFF for the rest of the run: %s at %s (%u timestamp query pools live). "
                     "More counter heaps can panic the Mac (IOGPUFamily, 2026-11-16).\n", why, where, live);
}
}  // namespace

std::uint32_t ParseLimit(const char* text, std::uint32_t fallback) {
    if (text == nullptr || *text == 0) return fallback;
    char* end = nullptr;
    const unsigned long v = std::strtoul(text, &end, 10);
    if (end == text || *end != 0 || v > 0xfffffffful) return fallback;
    return static_cast<std::uint32_t>(v);
}

bool OverLimit(std::uint64_t footprintMiB, std::uint32_t limitMiB) {
    return limitMiB != 0 && footprintMiB > limitMiB;
}

bool TimestampBudget::TryAcquire(const char* where) {
    if (g_disabled.load(std::memory_order_relaxed)) return false;
    const auto max = Max();
    auto live = g_live.load(std::memory_order_relaxed);
    for (;;) {
        if (live >= max) { DisableOnce("pool budget (NPS5_TS_POOL_MAX) reached", where, live); return false; }
        if (g_live.compare_exchange_weak(live, live + 1, std::memory_order_relaxed)) return true;
    }
}

void TimestampBudget::Release() noexcept {
    auto live = g_live.load(std::memory_order_relaxed);
    while (live > 0 && !g_live.compare_exchange_weak(live, live - 1, std::memory_order_relaxed)) {}
}

void TimestampBudget::NoteCreateFailure(const char* where, int vkResult) {
    Release();
    char why[48];
    std::snprintf(why, sizeof(why), "vkCreateQueryPool failed (%d)", vkResult);
    DisableOnce(why, where, g_live.load(std::memory_order_relaxed));
}

bool TimestampBudget::Disabled() noexcept { return g_disabled.load(std::memory_order_relaxed); }
std::uint32_t TimestampBudget::Live() noexcept { return g_live.load(std::memory_order_relaxed); }

void TimestampBudget::ResetForTest(std::uint32_t max) {
    g_live.store(0);
    g_disabled.store(false);
    g_max.store(max);
}

bool GpuProfileBlocked() {
    static const bool blocked = [] {
        const char* title = std::getenv("NPS5_TITLE_ID");
        const char* allow = std::getenv("NPS5_ALLOW_GPU_PROFILE");
        const bool astro = title != nullptr && std::strcmp(title, "PPSA21564") == 0;
        const bool wants = std::getenv("APS5_PROFILE_GPU") != nullptr || std::getenv("APS5_PROFILE_PASSES") != nullptr;
        const bool block = astro && !(allow != nullptr && std::strcmp(allow, "1") == 0);
        if (block && wants)
            std::fprintf(stderr, "[RED] APS5_PROFILE_GPU/APS5_PROFILE_PASSES ignored for ASTRO BOT (timestamp counter heaps panicked the Mac on 2026-11-16); NPS5_ALLOW_GPU_PROFILE=1 overrides\n");
        return block;
    }();
    return blocked;
}

std::uint64_t FootprintMiB() {
#if defined(__APPLE__)
    task_vm_info_data_t info;
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS) return 0;
    return static_cast<std::uint64_t>(info.phys_footprint) >> 20;
#else
    return 0;
#endif
}

void StartSelfMemoryLimit(void (*requestExit)(int code)) {
    static std::once_flag once;
    std::call_once(once, [requestExit] {
        const auto limit = ParseLimit(std::getenv("NPS5_SELF_MEM_LIMIT_MB"), 18944);
        if (limit == 0) return;
        std::fprintf(stderr, "[selfguard] self memory limit %u MiB (NPS5_SELF_MEM_LIMIT_MB, 0 = off); GPU timestamp pool budget %u\n", limit, Max() == 0xffffffffu ? 0u : Max());
        std::thread([limit, requestExit] {
            for (;;) {
                const auto mib = FootprintMiB();
                if (OverLimit(mib, limit)) {
                    std::fprintf(stderr, "[RED] self memory limit: phys_footprint %llu MiB over NPS5_SELF_MEM_LIMIT_MB=%u, exiting cleanly (exit 86) before the machine guard has to SIGKILL\n",
                                 static_cast<unsigned long long>(mib), limit);
                    std::fflush(stderr);
                    // The runtime's own exit lets guest threads unwind and Metal release GPU objects in order;
                    // a hung teardown is cut short by the hard exit.
                    if (requestExit != nullptr) requestExit(86);
                    std::this_thread::sleep_for(std::chrono::seconds(requestExit != nullptr ? 10 : 0));
                    std::fflush(stderr);
                    std::_Exit(86);
                }
                // Poll faster as the limit nears: a loading spike can add GB in seconds.
                const bool near = mib * 100 > static_cast<std::uint64_t>(limit) * 90;
                std::this_thread::sleep_for(std::chrono::milliseconds(near ? 250 : 1000));
            }
        }).detach();
    });
}

}  // namespace AgcDriver::SelfGuard
