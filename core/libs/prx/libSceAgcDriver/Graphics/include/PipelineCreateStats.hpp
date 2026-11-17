#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_PIPELINECREATESTATS_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_PIPELINECREATESTATS_HPP

#include <atomic>
#include <chrono>
#include <cstdint>

namespace AgcDriver::Graphics {

// nps5: times every vkCreate{Graphics,Compute}Pipelines the driver makes. Cheap (two clock reads and
// a few relaxed atomics) so it is always on. Calls longer than StallMs print "[pipeline-stall]";
// APS5_LOG_PIPELINE_CREATE=1 prints every call; a totals line is printed at process exit
// (and by LogPipelineCreateTotals) whenever anything was created.
struct PipelineCreateTotals {
    std::uint64_t graphics, compute, stalls;
    double graphicsMs, computeMs, maxMs, stallMs;
};

// nps5: wall time during which the host was busy building GPU state that a console does not have to build: draw-time
// pipeline creation (not prewarm work on the journal pool) and Vulkan device creation. Overlapping scopes count once.
// A host runtime subtracts it from the clock the title sees (AgcDriverStallMicros), so a title's own "the first frame
// did not complete within N seconds" check does not fire because shaders were compiling on a cold cache.
class GpuStallScope {
public:
    GpuStallScope();
    ~GpuStallScope();
    GpuStallScope(const GpuStallScope&) = delete;
    GpuStallScope& operator=(const GpuStallScope&) = delete;
};
// Marks the calling thread as a background pipeline builder (the async pool): its creations are not draw-time stalls.
void SetBackgroundPipelineBuildThread();
[[nodiscard]] bool BackgroundPipelineBuildThread();
[[nodiscard]] std::uint64_t GpuStallMicros();  // completed + currently running, microseconds

class PipelineCreateTimer {
public:
    enum class Kind : std::uint8_t { Graphics, Compute };
    static constexpr double StallMs = 50.0;
    explicit PipelineCreateTimer(Kind kind, const char* what = nullptr);
    ~PipelineCreateTimer();
    PipelineCreateTimer(const PipelineCreateTimer&) = delete;
    PipelineCreateTimer& operator=(const PipelineCreateTimer&) = delete;

private:
    Kind kind;
    const char* what;
    std::chrono::steady_clock::time_point start;
    bool stallScope;
};

[[nodiscard]] PipelineCreateTotals PipelineCreateStatistics();
void LogPipelineCreateTotals(const char* when);

// nps5: wall time of one RDNA -> SPIR-V compile the recompiler did itself (a shader disk cache miss), reported through the
// recompiler's diagnostic sink. Prints "[shader-compile] N ms" over StallMs and a totals line at exit.
void NoteShaderCompile(long long nanoseconds);

}

#endif
