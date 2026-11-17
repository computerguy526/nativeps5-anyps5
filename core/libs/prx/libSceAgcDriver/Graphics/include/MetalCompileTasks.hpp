#pragma once
// nps5: Metal's own number of concurrent compile tasks for the system GPU (MTLDevice.maximumConcurrentCompilationTaskCount), the width
// the shader/pipeline compilers inside Metal (the ones KosmicKrisp calls synchronously) can really run at. Taken from nxnative's prebuild
// tool (72d00571): sizing the workers from hw.perflevel0 left 1-4 of them on chips with three core levels. Header-only so every
// target that sizes a pool can use it; Metal is reached through dlopen and the objective-c runtime, so nothing links to it.
// Returns 0 when it cannot be read (no Metal, no GPU, older macOS): the caller then falls back to the core count.
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#ifdef __APPLE__
#include <dlfcn.h>
#include <sys/sysctl.h>
#endif

namespace AgcDriver::Graphics {

[[nodiscard]] inline unsigned MetalConcurrentCompileTasks() {
    static const unsigned value = [] {
#ifndef __APPLE__
        return 0u;
#else
        void* metal = dlopen("/System/Library/Frameworks/Metal.framework/Metal", RTLD_LAZY | RTLD_LOCAL);
        if (metal == nullptr) return 0u;
        using Create = void* (*)();
        const auto create = reinterpret_cast<Create>(dlsym(metal, "MTLCreateSystemDefaultDevice"));
        if (create == nullptr) return 0u;
        void* device = create();  // +1 and kept: a process-lifetime probe
        if (device == nullptr) return 0u;
        // The objective-c runtime by dlsym: libobjc is loaded by Metal, and the pipeline libraries do not link it.
        using Sel = void* (*)(const char*);
        using ClassOf = void* (*)(void*);
        using Responds = bool (*)(void*, void*);
        using Count = std::uintptr_t (*)(void*, void*);
        const auto sel = reinterpret_cast<Sel>(dlsym(RTLD_DEFAULT, "sel_registerName"));
        const auto classOf = reinterpret_cast<ClassOf>(dlsym(RTLD_DEFAULT, "object_getClass"));
        const auto responds = reinterpret_cast<Responds>(dlsym(RTLD_DEFAULT, "class_respondsToSelector"));
        const auto send = reinterpret_cast<Count>(dlsym(RTLD_DEFAULT, "objc_msgSend"));
        if (sel == nullptr || classOf == nullptr || responds == nullptr || send == nullptr) return 0u;
        void* selector = sel("maximumConcurrentCompilationTaskCount");
        if (!responds(classOf(device), selector)) return 0u;
        const auto count = send(device, selector);
        return count > 0 && count < 1024 ? static_cast<unsigned>(count) : 0u;
#endif
    }();
    return value;
}

[[nodiscard]] inline std::uint64_t PhysicalMemoryBytes() {
#ifdef __APPLE__
    std::uint64_t bytes = 0;
    std::size_t size = sizeof(bytes);
    if (sysctlbyname("hw.memsize", &bytes, &size, nullptr, 0) == 0) return bytes;
#endif
    return 0;  // unknown: no memory bound
}

// Workers for a pipeline/shader build pool that has the machine to itself (startup prebuild, offline prewarm): Metal's compile task
// count, at most the core count, and no more than one per 2.5 GiB of RAM (a KosmicKrisp compile holds about 2 GB). APS5_PIPELINE_PREBUILD_THREADS overrides.
[[nodiscard]] inline unsigned PrebuildWorkerCount(unsigned cores, std::uint64_t memoryBytes) {
    if (const char* text = std::getenv("APS5_PIPELINE_PREBUILD_THREADS"); text != nullptr && std::strtoul(text, nullptr, 10) != 0) {
        // The override may exceed the 2.5 GiB/worker rule, but never one worker per GiB of RAM (a typo must not OOM the machine).
        const std::uint64_t ramCap = memoryBytes == 0 ? 1024u : std::max<std::uint64_t>(1u, memoryBytes >> 30);
        return static_cast<unsigned>(std::min<std::uint64_t>({1024u, ramCap, std::strtoul(text, nullptr, 10)}));
    }
    const unsigned metal = MetalConcurrentCompileTasks();
    unsigned workers = metal != 0 ? std::min(metal, std::max(1u, cores)) : std::max(1u, cores > 2u ? cores - 2u : 1u);
    const std::uint64_t byMemory = memoryBytes == 0 ? workers : std::max<std::uint64_t>(1u, memoryBytes / (5ull << 29));
    return static_cast<unsigned>(std::max<std::uint64_t>(1u, std::min<std::uint64_t>(workers, byMemory)));
}

}
