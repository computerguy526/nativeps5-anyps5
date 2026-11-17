#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_HANDOFFSPIN_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_HANDOFFSPIN_HPP

#include "prx/libc/include/CpuTopology.hpp"
#include "prx/libSceAgcDriver/Graphics/include/AbSwitch.hpp"
#include <algorithm>
#include <bitset>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <thread>
#include "prx/libc/include/PreciseWait.hpp"

namespace AgcDriver {

inline void SpinPause() {
    PreciseSpinPause();
}

inline unsigned UsableProcessors() {
    const auto count = static_cast<unsigned>(std::bitset<64>(CpuTopology::Get().process).count());
    return count != 0 ? count : std::thread::hardware_concurrency();
}

inline std::chrono::nanoseconds HandoffSpinWindowFor(const char* configured, unsigned usableProcessors) {
    constexpr std::chrono::microseconds automaticWindow{150};
    constexpr unsigned automaticProcessorsAbove = 8;
    constexpr long long longestWindowUs = 10000;
    if (configured != nullptr) return std::chrono::microseconds(std::clamp<long long>(std::strtoll(configured, nullptr, 10), 0, longestWindowUs));
    return usableProcessors > automaticProcessorsAbove ? std::chrono::nanoseconds(automaticWindow) : std::chrono::nanoseconds::zero();
}

inline std::chrono::nanoseconds HandoffSpinWindow() {
    static const std::chrono::nanoseconds window = HandoffSpinWindowFor(std::getenv("APS5_HANDOFF_SPIN_US"), UsableProcessors());
    // AbSwitch name handoff_spin: an in-run A/B measures what the spin costs in CPU and buys in frame time (off = no spin).
    static const ::AgcDriver::Graphics::AbSwitch::Flag spin("handoff_spin", true);
    return spin.on() ? window : std::chrono::nanoseconds::zero();
}

// steady_clock::now() is mach_continuous_time() on macOS (a commpage read plus a
// call each time); the spin loops poll it every iteration. On arm64 read the
// virtual counter directly (cntvct_el0, fixed frequency) instead.
#if defined(__aarch64__)
inline std::uint64_t SpinTicks() {
    std::uint64_t v;
    asm volatile("mrs %0, cntvct_el0" : "=r"(v));
    return v;
}
inline std::uint64_t SpinTickFrequency() {
    static const std::uint64_t f = [] { std::uint64_t v; asm volatile("mrs %0, cntfrq_el0" : "=r"(v)); return v ? v : 24000000ULL; }();
    return f;
}
#endif

template<typename TReady>
bool SpinUntil(std::chrono::nanoseconds window, TReady&& ready) {
    if (ready()) return true;
    if (window.count() <= 0) return false;
#if defined(__aarch64__)
    static const ::AgcDriver::Graphics::AbSwitch::Flag cheapClock("spin_cheap_clock", std::getenv("APS5_SPIN_STEADY_CLOCK") == nullptr);
    if (cheapClock.on()) {
        const std::uint64_t freq = SpinTickFrequency();
        const std::uint64_t span = static_cast<std::uint64_t>(window.count()) * freq / 1000000000ULL;
        const std::uint64_t start = SpinTicks();
        do {
            SpinPause();
            if (ready()) return true;
        } while (SpinTicks() - start < span);
        return false;
    }
#endif
    const auto until = std::chrono::steady_clock::now() + window;
    do {
        SpinPause();
        if (ready()) return true;
    } while (std::chrono::steady_clock::now() < until);
    return false;
}

template<typename TMutex>
void LockSpinning(TMutex& mutex, std::chrono::nanoseconds window) {
    if (window.count() > 0 && SpinUntil(window, [&] { return mutex.try_lock(); })) return;
    mutex.lock();
}

}

#endif
