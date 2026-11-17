// nps5_prewarm: offline pre-compile of a title's GPU pipelines, no guest code.
//
// Opens the per-title GPU cache (ANYPS5_SHADER_CACHE_DIR), creates the Vulkan device (KosmicKrisp) and replays the
// pipeline journal on the pool (every graphics/compute pipeline the title ever created, with its state), so the
// VkPipelineCache, the Metal pipeline archive and the collected MSL are filled before the first launch. The cache store
// writes back at exit like any run. See docs/nps5-pipeline-cache.md ("Prewarm") and tools/nps5-prewarm.sh.
#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Graphics/include/PipelineCreateStats.hpp"
#include "prx/libSceAgcDriver/Graphics/include/MetalCompileTasks.hpp"
#include "prx/libSceAgcDriver/Graphics/include/PipelineJournal.hpp"
#include "ShaderDiskCache.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <exception>
#include <string>
#include <sys/resource.h>
#include <thread>

int main(int argc, char** argv) {
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--cache" && index + 1 < argc) setenv("ANYPS5_SHADER_CACHE_DIR", argv[++index], 1);
        else if (argument == "--range" && index + 1 < argc) setenv("APS5_PIPELINE_JOURNAL_RANGE", argv[++index], 1);
        else if (argument == "--threads" && index + 1 < argc) setenv("APS5_PIPELINE_JOURNAL_THREADS", argv[++index], 1);
        // For tools/nps5-prewarm.sh: the recompiler version names the cache's <version>/ directory (and its pipelines.journal), and the
        // worker count is Metal's concurrent compile task count (bounded by cores and RAM).
        else if (argument == "--print-version") {
            std::printf("%016llx\n", static_cast<unsigned long long>(ShaderRecompiler::ShaderDiskCache::SourceVersion()));
            return 0;
        } else if (argument == "--print-workers") {
            std::printf("%u\n", AgcDriver::Graphics::PrebuildWorkerCount(std::max(1u, std::thread::hardware_concurrency()), AgcDriver::Graphics::PhysicalMemoryBytes()));
            return 0;
        }
        else {
            std::fprintf(stderr, "usage: nps5_prewarm --cache <gpu-cache dir> [--threads N (default: Metal compile tasks)] [--range first:count] | --print-version | --print-workers\n  env: VK_DRIVER_FILES (KosmicKrisp icd), APS5_PIPELINE_JOURNAL_QOS (default user-initiated here)\n");
            return 2;
        }
    }
    if (std::getenv("ANYPS5_SHADER_CACHE_DIR") == nullptr) {
        std::fprintf(stderr, "nps5_prewarm: --cache or ANYPS5_SHADER_CACHE_DIR required\n");
        return 2;
    }
    {
        char workers[16];
        std::snprintf(workers, sizeof(workers), "%u", AgcDriver::Graphics::PrebuildWorkerCount(std::max(1u, std::thread::hardware_concurrency()), AgcDriver::Graphics::PhysicalMemoryBytes()));
        setenv("APS5_PIPELINE_JOURNAL_THREADS", workers, 0);
    }
    setenv("APS5_PIPELINE_JOURNAL_QOS", "user-initiated", 0);
    setenv("APS5_HEADLESS_PIPELINE_CACHE", "1", 0);
    setpriority(PRIO_PROCESS, 0, 10);  // the Mac stays usable: WindowServer and the user's apps win
    int code = 0;
    const auto started = std::chrono::steady_clock::now();
    try {
        AgcDriverWaitForPipelineJournal_nid_postfix();
        AgcDriverShutdown_nid_postfix();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "nps5_prewarm: %s\n", error.what());
        code = 1;
    }
    const auto totals = AgcDriver::Graphics::PipelineJournalTotals();
    std::fprintf(stderr, "[prewarm] %.1f s: journal %llu recipes loaded, %llu graphics + %llu compute built, %llu already present, %llu rejected, %llu failed\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count(), static_cast<unsigned long long>(totals.loadedRecipes), static_cast<unsigned long long>(totals.replayedGraphics), static_cast<unsigned long long>(totals.replayedCompute), static_cast<unsigned long long>(totals.replayPresent), static_cast<unsigned long long>(totals.replayRejected), static_cast<unsigned long long>(totals.replayFailed));
    std::fflush(stderr);
    // --range 0:N covers the first N entries: entries of earlier batches are archive hits (about 1 ms each) that keep the Metal
    // archive complete (an archive is rewritten from what its process looked up), only the new ones compile. 11 = the range
    // reached the end of the journal, the caller stops after this batch.
    if (const char* range = std::getenv("APS5_PIPELINE_JOURNAL_RANGE"); range != nullptr) {
        const char* colon = std::strchr(range, ':');
        if (colon != nullptr && std::strtoull(colon + 1, nullptr, 10) >= totals.loadedRecipes && code == 0) return 11;
    }
    if (totals.replayFailed != 0 || totals.replayRejected != 0) code = code != 0 ? code : 3;
    return code;
}
