// A host runtime that keeps its own guest clocks installs them with KernelSetGuestClock_nid_postfix; the process-time and TSC
// exports (which VideoOut's flip/vblank status stamps and AMPR timestamps call) then return the runtime's values in the runtime's
// units, and clearing the hook restores AnyPS5's own clock.
#include "prx/libkernel/Time/include/Time.hpp"
#include "prx/libkernel/include/Api.hpp"
#include <cstdint>
#include <cstdio>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <thread>
#include <vector>

namespace {
constexpr std::uint64_t RuntimeFrequency = 2130000000ULL;  // the runtime's TSC and process-counter frequency (docs/m3/clock.md)
std::uint64_t fakeTicks = 0;
std::uint64_t Counter() { return fakeTicks; }
std::uint64_t Micros() { return fakeTicks / (RuntimeFrequency / 1000000ULL); }
std::uint64_t Frequency() { return RuntimeFrequency; }
std::uint64_t Tsc() { return fakeTicks + 7; }

void Require(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        std::abort();
    }
}
}

int main() {
    const std::uint64_t ownFrequency = sceKernelGetProcessTimeCounterFrequency();
    KernelGuestClock clock{&Micros, &Counter, &Frequency, &Tsc, &Frequency};
    KernelSetGuestClock_nid_postfix(&clock);
    fakeTicks = 5 * RuntimeFrequency;  // five seconds of runtime time
    Require(sceKernelGetProcessTimeCounterFrequency() == RuntimeFrequency, "process counter frequency is the runtime's");
    Require(sceKernelGetTscFrequency() == RuntimeFrequency, "TSC frequency is the runtime's");
    Require(sceKernelGetProcessTimeCounter() == fakeTicks, "process time counter is the runtime's");
    Require(sceKernelReadTsc() == fakeTicks + 7, "TSC is the runtime's");
    Require(sceKernelGetProcessTime() == 5000000ULL, "process time is the runtime's");
    // What a title does with a stamp: counter / frequency is seconds, in the same units as its own clock reads.
    const double stampSeconds = static_cast<double>(sceKernelGetProcessTimeCounter()) / static_cast<double>(sceKernelGetProcessTimeCounterFrequency());
    Require(stampSeconds > 4.999 && stampSeconds < 5.001, "counter / frequency is seconds");
    KernelSetGuestClock_nid_postfix(nullptr);
    Require(sceKernelGetProcessTimeCounterFrequency() == ownFrequency, "clearing the hook restores AnyPS5's clock");
    Require(sceKernelGetProcessTimeCounter() != fakeTicks, "clearing the hook restores AnyPS5's counter");
    // Install and clear the hook while other threads read every clock: a reader must never call a null member (the old plain
    // struct copy let a reader pass the null test and then call through a just-cleared pointer) and never see a half-installed hook.
    {
        std::atomic<bool> stop{false};
        std::atomic<std::uint64_t> reads{0};
        std::vector<std::thread> readers;
        for (int index = 0; index < 4; ++index) {
            readers.emplace_back([&] {
                std::uint64_t sink = 0;
                while (!stop.load(std::memory_order_relaxed)) {
                    sink += sceKernelGetProcessTime() + sceKernelGetProcessTimeCounter() + sceKernelGetProcessTimeCounterFrequency() + sceKernelReadTsc() + sceKernelGetTscFrequency();
                    reads.fetch_add(1, std::memory_order_relaxed);
                }
                if (sink == 1) std::puts("");
            });
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        std::uint64_t flips = 0;
        while (std::chrono::steady_clock::now() < deadline) {
            KernelGuestClock hook{&Micros, &Counter, &Frequency, &Tsc, &Frequency};
            KernelSetGuestClock_nid_postfix(&hook);
            KernelSetGuestClock_nid_postfix(nullptr);
            ++flips;
        }
        stop = true;
        for (auto& reader : readers) reader.join();
        Require(reads.load() > 0 && flips > 0, "the stress readers ran");
    }
    std::puts("guest clock hook ok");
    return 0;
}
