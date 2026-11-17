#include "prx/libSceAgcDriver/Graphics/include/AbSwitch.hpp"
#include "prx/libSceAgcDriver/Graphics/include/RuntimeSwitches.hpp"
#include "prx/libSceAgcDriver/Execution/include/ProfileOutput.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/thread_info.h>
#endif

namespace AgcDriver::Graphics::AbSwitch {

namespace {

std::vector<std::string>& ListedNames() {
    static std::vector<std::string>* names = [] {
        auto* list = new std::vector<std::string>();
        const char* text = std::getenv("APS5_AB_FLAGS");
        std::string_view rest = text != nullptr ? text : "";
        while (!rest.empty()) {
            const auto comma = rest.find(',');
            auto item = rest.substr(0, comma);
            while (!item.empty() && (item.front() == ' ' || item.front() == '\t')) item.remove_prefix(1);
            while (!item.empty() && (item.back() == ' ' || item.back() == '\t')) item.remove_suffix(1);
            if (!item.empty()) list->emplace_back(item);
            if (comma == std::string_view::npos) break;
            rest.remove_prefix(comma + 1);
        }
        return list;
    }();
    return *names;
}

struct TrackedThread {
    std::string kind;
#ifdef __APPLE__
    mach_port_t port = MACH_PORT_NULL;
#endif
    double lastCpuMs = 0;
};

struct State {
    std::mutex mutex;
    std::uint64_t period = 0;
    std::uint64_t framesInPeriod = 0;
    std::uint64_t periodIndex = 0;
    std::uint64_t periodsPerMode = 2;   // APS5_AB_MODE_PERIODS
    std::uint64_t sinceSwitch = 0;      // periods completed in the current mode
    std::vector<double> frameMs;
    std::chrono::steady_clock::time_point lastFlip{};
    bool haveLast = false;
    std::vector<TrackedThread> threads;
    std::vector<RuntimeSwitch> runtimeSwitches;       // listed names that are existing RuntimeSwitches
    std::vector<bool> runtimeEnvironment;
    bool initialised = false;
};

State& S() {
    static State* state = new State();
    return *state;
}

double ThreadCpuMs(const TrackedThread& thread, bool& ok) {
    ok = false;
#ifdef __APPLE__
    if (thread.port == MACH_PORT_NULL) return 0;
    thread_basic_info_data_t info;
    mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
    if (thread_info(thread.port, THREAD_BASIC_INFO, reinterpret_cast<thread_info_t>(&info), &count) != KERN_SUCCESS) return 0;
    ok = true;
    return static_cast<double>(info.user_time.seconds + info.system_time.seconds) * 1000.0 + static_cast<double>(info.user_time.microseconds + info.system_time.microseconds) / 1000.0;
#else
    return 0;
#endif
}

void Init(State& state) {
    state.initialised = true;
    state.period = ParsePeriod(std::getenv("APS5_AB_PERIOD"));
    if (const auto per = ParsePeriod(std::getenv("APS5_AB_MODE_PERIODS")); per != 0) state.periodsPerMode = per;
    for (const auto& name : ListedNames()) {
        if (const auto found = RuntimeSwitches::Find(name)) {
            state.runtimeSwitches.push_back(*found);
            state.runtimeEnvironment.push_back(RuntimeSwitches::Set(*found));
        }
    }
    state.frameMs.reserve(static_cast<std::size_t>(std::min<std::uint64_t>(state.period, 100000)));
    std::string flags;
    for (const auto& name : ListedNames()) flags += (flags.empty() ? "" : ",") + name;
    AgcDriver::ProfilePrint_nid_no_patch("[ab] AbSwitch: period %llu frames, mode switches every %llu periods, toggled in mode B: %s\n", static_cast<unsigned long long>(state.period), static_cast<unsigned long long>(state.periodsPerMode), flags.empty() ? "(none)" : flags.c_str());
}

}

std::uint64_t ParsePeriod(const char* text) {
    if (text == nullptr || *text == '\0') return 0;
    char* end = nullptr;
    const auto value = std::strtoull(text, &end, 10);
    return (end != nullptr && *end == '\0') ? value : 0;
}

double Percentile(double* values, std::size_t count, double fraction) {
    if (count == 0) return 0;
    std::sort(values, values + count);
    const auto index = static_cast<std::size_t>(fraction * static_cast<double>(count - 1) + 0.5);
    return values[std::min(index, count - 1)];
}

bool Configured() {
    static const bool configured = ParsePeriod(std::getenv("APS5_AB_PERIOD")) != 0;
    return configured;
}

void RegisterThread(const char* kind) {
    if (!Configured()) return;
    auto& state = S();
    std::lock_guard lock(state.mutex);
    TrackedThread thread;
    thread.kind = kind;
#ifdef __APPLE__
    thread.port = mach_thread_self();
#endif
    bool ok = false;
    thread.lastCpuMs = ThreadCpuMs(thread, ok);
    state.threads.push_back(std::move(thread));
}

void OnFlip() {
    if (!Configured()) return;
    auto& state = S();
    std::lock_guard lock(state.mutex);
    if (!state.initialised) Init(state);
    const auto now = std::chrono::steady_clock::now();
    if (state.haveLast) state.frameMs.push_back(std::chrono::duration<double, std::milli>(now - state.lastFlip).count());
    state.lastFlip = now;
    state.haveLast = true;
    if (++state.framesInPeriod < state.period) return;

    const bool b = modeB.load(std::memory_order_relaxed);
    double commitMs = 0, workerMs = 0;
    for (auto& thread : state.threads) {
        bool ok = false;
        const double cpu = ThreadCpuMs(thread, ok);
        if (!ok) continue;
        const double delta = cpu - thread.lastCpuMs;
        thread.lastCpuMs = cpu;
        (thread.kind == "commit" ? commitMs : workerMs) += delta;
    }
    const double frames = static_cast<double>(state.framesInPeriod);
    const std::size_t n = state.frameMs.size();
    double sum = 0;
    for (const double v : state.frameMs) sum += v;
    const double p50 = Percentile(state.frameMs.data(), n, 0.5);
    const double p90 = Percentile(state.frameMs.data(), n, 0.9);
    AgcDriver::ProfilePrint_nid_no_patch("[ab] mode=%c period=%llu since_switch=%llu frames=%llu ft_p50=%.2f ft_p90=%.2f ft_mean=%.2f commit_ms=%.2f qworker_ms=%.2f\n", b ? 'B' : 'A', static_cast<unsigned long long>(state.periodIndex), static_cast<unsigned long long>(state.sinceSwitch), static_cast<unsigned long long>(state.framesInPeriod), p50, p90, n != 0 ? sum / static_cast<double>(n) : 0.0, commitMs / frames, workerMs / frames);

    state.frameMs.clear();
    state.framesInPeriod = 0;
    ++state.periodIndex;
    if (++state.sinceSwitch < state.periodsPerMode) return;
    state.sinceSwitch = 0;
    const bool next = !b;
    for (std::size_t i = 0; i < state.runtimeSwitches.size(); ++i) RuntimeSwitches::Assign(state.runtimeSwitches[i], state.runtimeEnvironment[i] != next);
    modeB.store(next, std::memory_order_relaxed);
}

}
