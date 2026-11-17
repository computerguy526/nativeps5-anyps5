#include "prx/libSceAgcDriver/Graphics/include/PipelineCreateStats.hpp"
#include "prx/libSceAgcDriver/Graphics/include/PipelineJournal.hpp"
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <pthread.h>

namespace AgcDriver::Graphics {

namespace {

struct State {
    std::atomic<std::uint64_t> graphics{0}, compute{0}, stalls{0};
    std::atomic<double> graphicsMs{0}, computeMs{0}, maxMs{0}, stallMs{0};
    bool verbose = std::getenv("APS5_LOG_PIPELINE_CREATE") != nullptr;
    ~State() {
        if (graphics.load() + compute.load() != 0) LogPipelineCreateTotals("exit");
    }
};

State& state() {
    static State instance;
    return instance;
}

void add(std::atomic<double>& target, double value) {
    auto old = target.load(std::memory_order_relaxed);
    while (!target.compare_exchange_weak(old, old + value, std::memory_order_relaxed)) {}
}

}

namespace {
struct StallClock {
    std::mutex mutex;
    int active = 0;
    std::chrono::steady_clock::time_point since;
    std::uint64_t doneMicros = 0;
};
StallClock& stallClock() {
    static auto* clock = new StallClock;  // leaked: scopes can end during process teardown
    return *clock;
}
}

namespace {
thread_local bool backgroundBuildThread = false;
}
void SetBackgroundPipelineBuildThread() { backgroundBuildThread = true; }
bool BackgroundPipelineBuildThread() { return backgroundBuildThread; }

GpuStallScope::GpuStallScope() {
    auto& c = stallClock();
    std::lock_guard lock(c.mutex);
    if (c.active++ == 0) c.since = std::chrono::steady_clock::now();
}

GpuStallScope::~GpuStallScope() {
    auto& c = stallClock();
    std::lock_guard lock(c.mutex);
    if (--c.active == 0) c.doneMicros += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - c.since).count());
}

std::uint64_t GpuStallMicros() {
    auto& c = stallClock();
    std::lock_guard lock(c.mutex);
    return c.doneMicros + (c.active > 0 ? static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - c.since).count()) : 0);
}

PipelineCreateTimer::PipelineCreateTimer(Kind kind, const char* what) : kind(kind), what(what), start(std::chrono::steady_clock::now()), stallScope(!PipelineJournalReplaying() && !BackgroundPipelineBuildThread()) {
    if (stallScope) {
        auto& c = stallClock();
        std::lock_guard lock(c.mutex);
        if (c.active++ == 0) c.since = start;
    }
    // Test aid: APS5_PIPELINE_CREATE_DELAY_MS=<n> makes every draw-time creation take n ms longer (a slow cold cache).
    static const long delayMs = [] { const char* v = std::getenv("APS5_PIPELINE_CREATE_DELAY_MS"); return v != nullptr ? std::strtol(v, nullptr, 10) : 0L; }();
    if (stallScope && delayMs > 0) std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
}

PipelineCreateTimer::~PipelineCreateTimer() {
    if (stallScope) {
        auto& c = stallClock();
        std::lock_guard lock(c.mutex);
        if (--c.active == 0) c.doneMicros += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - c.since).count());
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    auto& s = state();
    const bool graphicsKind = kind == Kind::Graphics;
    (graphicsKind ? s.graphics : s.compute).fetch_add(1, std::memory_order_relaxed);
    add(graphicsKind ? s.graphicsMs : s.computeMs, ms);
    auto peak = s.maxMs.load(std::memory_order_relaxed);
    while (ms > peak && !s.maxMs.compare_exchange_weak(peak, ms, std::memory_order_relaxed)) {}
    // A creation while the journal replay runs is prewarm work on a pool thread; one after it is a draw-time stall.
    const bool prewarm = PipelineJournalReplaying() || BackgroundPipelineBuildThread();
    const bool stall = ms > PipelineCreateTimer::StallMs;
    if (stall && !prewarm) {
        s.stalls.fetch_add(1, std::memory_order_relaxed);
        add(s.stallMs, ms);
    }
    if (s.verbose || (stall && !prewarm)) std::fprintf(stderr, "[pipeline-%s] %s %s%s %.2f ms\n", stall && !prewarm ? "stall" : "create", graphicsKind ? "graphics" : "compute", what ? what : "", prewarm ? " (prewarm)" : "", ms);
}

namespace {
struct CompileStats {
    std::atomic<std::uint64_t> count{0}, ns{0}, maxNs{0}, slow{0}, slowNs{0};
    ~CompileStats() {
        if (count.load() == 0) return;
        std::fprintf(stderr, "[shader-compile] exit: %llu compiles %.1f ms (avg %.2f), max %.1f ms, %llu over %.0f ms totalling %.1f ms\n", static_cast<unsigned long long>(count.load()), static_cast<double>(ns.load()) / 1e6, static_cast<double>(ns.load()) / 1e6 / static_cast<double>(count.load()), static_cast<double>(maxNs.load()) / 1e6, static_cast<unsigned long long>(slow.load()), PipelineCreateTimer::StallMs, static_cast<double>(slowNs.load()) / 1e6);
    }
};
}

static const auto origin = std::chrono::steady_clock::now();  // library load, ~process start
void NoteShaderCompile(long long nanoseconds) {
    static CompileStats stats;
    const auto value = static_cast<std::uint64_t>(nanoseconds);
    stats.count.fetch_add(1, std::memory_order_relaxed);
    stats.ns.fetch_add(value, std::memory_order_relaxed);
    auto peak = stats.maxNs.load(std::memory_order_relaxed);
    while (value > peak && !stats.maxNs.compare_exchange_weak(peak, value, std::memory_order_relaxed)) {}
    if (static_cast<double>(value) / 1e6 > PipelineCreateTimer::StallMs) {
        stats.slow.fetch_add(1, std::memory_order_relaxed);
        stats.slowNs.fetch_add(value, std::memory_order_relaxed);
        char threadName[64] = "?";
        pthread_getname_np(pthread_self(), threadName, sizeof threadName);
        std::uint64_t tid = 0;
        pthread_threadid_np(nullptr, &tid);
        std::fprintf(stderr, "[shader-compile] %.1f ms (t=%.1f s, thread '%s' tid %llu%s)\n", static_cast<double>(value) / 1e6, std::chrono::duration<double>(std::chrono::steady_clock::now() - origin).count(), threadName, static_cast<unsigned long long>(tid), BackgroundPipelineBuildThread() ? " bg" : "");
    }
}

PipelineCreateTotals PipelineCreateStatistics() {
    auto& s = state();
    return {s.graphics.load(), s.compute.load(), s.stalls.load(), s.graphicsMs.load(), s.computeMs.load(), s.maxMs.load(), s.stallMs.load()};
}

void LogPipelineCreateTotals(const char* when) {
    const auto t = PipelineCreateStatistics();
    std::fprintf(stderr, "[pipeline-create] %s: %llu graphics %.1f ms (avg %.2f), %llu compute %.1f ms (avg %.2f), max %.1f ms, %llu draw-time stalls over %.0f ms totalling %.1f ms\n", when, static_cast<unsigned long long>(t.graphics), t.graphicsMs, t.graphics ? t.graphicsMs / static_cast<double>(t.graphics) : 0.0, static_cast<unsigned long long>(t.compute), t.computeMs, t.compute ? t.computeMs / static_cast<double>(t.compute) : 0.0, t.maxMs, static_cast<unsigned long long>(t.stalls), PipelineCreateTimer::StallMs, t.stallMs);
}

}

// Host runtime entry point (docs/nps5-pipeline-cache.md): microseconds the host spent building GPU state a console does not
// have to build (see GpuStallScope). Monotonic, safe from any thread.
extern "C" std::uint64_t AgcDriverStallMicros(void) { return AgcDriver::Graphics::GpuStallMicros(); }
