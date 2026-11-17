#include "prx/libSceAgcDriver/Graphics/include/MemoryProbe.hpp"
#include "prx/libSceAgcDriver/Graphics/include/BufferPool.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestBufferMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libSceAgcDriver/Graphics/include/UnitShadow.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>
#ifndef _WIN32
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace AgcDriver::Graphics {
namespace {

constexpr double MiB = 1048576.0;

// One cache line per event kind: writers of different kinds never share a line.
struct alignas(64) Cell {
    std::atomic<std::uint64_t> count{0};
    std::atomic<std::uint64_t> bytes{0};
};

struct alignas(64) Live {
    std::atomic<std::uint64_t> images{0};
    std::atomic<std::uint64_t> imageBytes{0};
    std::atomic<std::uint64_t> peakImageBytes{0};
    std::atomic<std::uint64_t> peakImages{0};
};

struct Probe {
    std::array<Cell, MemoryProbe::KindCount> cells{};
    Live live;
    std::mutex mutex;
    std::optional<Context> context;
    std::thread thread;
    std::condition_variable wake;
    bool stop = false;
    bool started = false;
    std::uint64_t interval = 0;
    std::uint64_t lastPresents = 0;
    MemoryProbe::Events lastEvents{};
    std::mutex printMutex;
};

Probe& probe() {
    static Probe value;
    return value;
}

thread_local MemoryProbe::Kind pendingCause = MemoryProbe::Kind::StorageDirect;

std::uint64_t readInterval() {
    const std::uint64_t value = [] {
        const char* text = std::getenv("APS5_MEM_PROBE");
        if (text == nullptr || *text == '\0') return std::uint64_t{0};
        const auto parsed = std::strtoull(text, nullptr, 10);
        return parsed == 0 ? std::uint64_t{60} : parsed;
    }();
    return value;
}

std::uint64_t interval() {
    static const std::uint64_t value = readInterval();
    return value;
}

bool deep() {
    static const bool value = std::getenv("APS5_MEM_PROBE_DEEP") != nullptr;
    return value;
}

void raise(std::atomic<std::uint64_t>& peak, std::uint64_t value) {
    auto seen = peak.load(std::memory_order_relaxed);
    while (value > seen && !peak.compare_exchange_weak(seen, value, std::memory_order_relaxed)) {}
}

std::vector<MemoryProbe::Gauge> sampleGauges(const std::optional<Context>& context, StorageTexture::MemoryTotalsResult* storage) {
    std::vector<MemoryProbe::Gauge> gauges;
    auto& state = probe();
    const auto totals = StorageTexture::MemoryTotals(storage != nullptr);
    if (storage != nullptr) *storage = totals;
    gauges.push_back({"storage_images", totals.images, 0});
    gauges.push_back({"storage_gpu_bytes", totals.imageBytes, context ? StorageTextureCacheBudget(*context) : 0});
    gauges.push_back({"storage_guest_bytes", totals.guestBytes, 0});
    gauges.push_back({"original", totals.originalBytes, 0});
    gauges.push_back({"generation_baseline", totals.baselineBytes, 0});
    gauges.push_back({"storage_created_live", state.live.images.load(std::memory_order_relaxed), 0});
    gauges.push_back({"sampled_textures", SampledTextureMemory(), context ? SampledTextureCacheBudget(*context) : 0});
    const auto mirrors = MirrorCounters();
    gauges.push_back({"heap_mirrors", mirrors.heapBytes, 0});
    const auto imports = HostImportCounters();
    gauges.push_back({"host_imports", imports.liveBytes, 0});
    gauges.push_back({"unit_shadow", UnitShadowLiveBytes(), context ? UnitShadowBudgetBytes(*context) : 0});
    if (context && context->bufferPool) {
        const auto pool = context->bufferPool->Totals();
        gauges.push_back({"pool_small", pool.small, pool.smallBudget});
        gauges.push_back({"pool_large", pool.large, pool.largeBudget});
        gauges.push_back({"pool_device", pool.device, pool.deviceBudget});
        gauges.push_back({"pool_images", pool.images, pool.imageBudget});
    }
    return gauges;
}

// Resident bytes of [begin, begin + bytes) by mincore; -1 when the range is not mapped.
std::int64_t residentBytes(std::uint64_t begin, std::uint64_t bytes) {
#ifdef _WIN32
    static_cast<void>(begin);
    static_cast<void>(bytes);
    return -1;
#else
    static const std::uint64_t page = static_cast<std::uint64_t>(::sysconf(_SC_PAGESIZE));
    const auto first = begin & ~(page - 1u);
    const auto last = (begin + bytes + page - 1u) & ~(page - 1u);
    if (last <= first) return 0;
    std::vector<char> vec(static_cast<std::size_t>((last - first) / page));
    if (::mincore(reinterpret_cast<void*>(first), static_cast<std::size_t>(last - first), vec.data()) != 0) return -1;
    std::int64_t pages = 0;
    for (const char value : vec) pages += (value & 1) != 0 ? 1 : 0;
    return pages * static_cast<std::int64_t>(page);
#endif
}

void printDeep(const StorageTexture::MemoryTotalsResult& storage) {
    if (!deep()) return;
    auto ranges = storage.ranges;
    std::sort(ranges.begin(), ranges.end());
    // Merge overlapping surfaces so aliased images count their pages once.
    std::vector<std::pair<std::uint64_t, std::uint64_t>> merged;
    for (const auto& [begin, end] : ranges) {
        if (!merged.empty() && begin <= merged.back().second) merged.back().second = std::max(merged.back().second, end);
        else merged.emplace_back(begin, end);
    }
    std::uint64_t span = 0, resident = 0, unmapped = 0;
    for (const auto& [begin, end] : merged) {
        span += end - begin;
        const auto bytes = residentBytes(begin, end - begin);
        if (bytes < 0) unmapped += end - begin;
        else resident += static_cast<std::uint64_t>(bytes);
    }
    std::fprintf(stderr, "[mem-deep] storage surface ranges=%zu (merged %zu) span=%.1f MiB resident_in_surfaces=%.1f MiB unmapped_span=%.1f MiB\n", ranges.size(), merged.size(), span / MiB, resident / MiB, unmapped / MiB);
}

void summary(const char* tag, bool force) noexcept {
    try {
        auto& state = probe();
        std::lock_guard print(state.printMutex);
        const auto presents = Recorder::Presents();
        std::optional<Context> context;
        {
            std::lock_guard lock(state.mutex);
            context = state.context;
            if (!force && presents < state.lastPresents + state.interval) return;
        }
        StorageTexture::MemoryTotalsResult storage;
        const auto gauges = sampleGauges(context, &storage);
        const auto now = MemoryProbe::Counters();
        const auto window = presents - std::min(presents, state.lastPresents);
        const auto line = MemoryProbe::FormatLine(tag, presents, window, now, state.lastEvents, gauges);
        std::fprintf(stderr, "%s\n", line.c_str());
        printDeep(storage);
        static const bool importResidency = std::getenv("APS5_IMPORT_RESIDENCY") != nullptr;
        if (importResidency) LogHostImportResidency(tag);
        std::fflush(stderr);
        state.lastPresents = presents;
        state.lastEvents = now;
    } catch (...) {
    }
}

void run() {
    auto& state = probe();
    std::unique_lock lock(state.mutex);
    while (!state.stop) {
        state.wake.wait_for(lock, std::chrono::milliseconds(100));
        if (state.stop) break;
        lock.unlock();
        summary("mem", false);
        lock.lock();
    }
}

struct Shutdown {
    ~Shutdown() {
        auto& state = probe();
        if (!state.started) return;
        {
            std::lock_guard lock(state.mutex);
            state.stop = true;
        }
        state.wake.notify_all();
        if (state.thread.joinable()) state.thread.join();
        summary("final", true);
    }
};

}

const char* MemoryProbe::KindName(Kind kind) {
    static constexpr const char* names[KindCount] = {"new", "extent", "mips", "layers", "format", "dcc", "other", "direct", "destroyed", "upload", "writeback", "original", "baseline"};
    return names[static_cast<std::size_t>(kind)];
}

const bool MemoryProbeOn = readInterval() != 0;

void MemoryProbe::AddOn(Kind kind, std::uint64_t bytes) noexcept {
    auto& cell = probe().cells[static_cast<std::size_t>(kind)];
    cell.count.fetch_add(1, std::memory_order_relaxed);
    cell.bytes.fetch_add(bytes, std::memory_order_relaxed);
}

MemoryProbe::Events MemoryProbe::Counters() noexcept {
    Events events{};
    auto& state = probe();
    for (std::size_t i = 0; i < KindCount; ++i) {
        events[i].count = state.cells[i].count.load(std::memory_order_relaxed);
        events[i].bytes = state.cells[i].bytes.load(std::memory_order_relaxed);
    }
    return events;
}

MemoryProbe::Kind MemoryProbe::ClassifyCreation(const GuestTextureResource* existing, const GuestTextureResource& requested, bool metadataMoved) noexcept {
    if (metadataMoved) return Kind::StorageDcc;
    if (existing == nullptr) return Kind::StorageNew;
    if (existing->width != requested.width || existing->height != requested.height) return Kind::StorageExtent;
    if (existing->mipCount != requested.mipCount) return Kind::StorageMips;
    if (existing->depthOrLastArray != requested.depthOrLastArray || existing->baseArray != requested.baseArray) return Kind::StorageLayers;
    if (existing->format != requested.format) return Kind::StorageFormat;
    return Kind::StorageOther;
}

MemoryProbe::CauseScope::CauseScope(Kind kind) noexcept {
    pendingCause = kind;
}

MemoryProbe::CauseScope::~CauseScope() {
    pendingCause = Kind::StorageDirect;
}

MemoryProbe::Kind MemoryProbe::TakeCause() noexcept {
    const auto cause = pendingCause;
    pendingCause = Kind::StorageDirect;
    return cause;
}

void MemoryProbe::CreatedOn(Kind cause, std::uint64_t imageBytes) noexcept {
    AddOn(cause, imageBytes);
    auto& state = probe();
    const auto images = state.live.images.fetch_add(1, std::memory_order_relaxed) + 1;
    const auto live = state.live.imageBytes.fetch_add(imageBytes, std::memory_order_relaxed) + imageBytes;
    raise(state.live.peakImages, images);
    raise(state.live.peakImageBytes, live);
}

void MemoryProbe::DestroyedOn(std::uint64_t imageBytes) noexcept {
    AddOn(Kind::StorageDestroyed, imageBytes);
    auto& state = probe();
    state.live.images.fetch_sub(1, std::memory_order_relaxed);
    state.live.imageBytes.fetch_sub(imageBytes, std::memory_order_relaxed);
}

std::uint64_t MemoryProbe::LiveImages() noexcept { return probe().live.images.load(std::memory_order_relaxed); }
std::uint64_t MemoryProbe::LiveImageBytes() noexcept { return probe().live.imageBytes.load(std::memory_order_relaxed); }
std::uint64_t MemoryProbe::PeakImages() noexcept { return probe().live.peakImages.load(std::memory_order_relaxed); }
std::uint64_t MemoryProbe::PeakImageBytes() noexcept { return probe().live.peakImageBytes.load(std::memory_order_relaxed); }

void MemoryProbe::NoteContext(const Context& context) noexcept {
    if (!Enabled()) return;
    try {
        auto& state = probe();
        std::lock_guard lock(state.mutex);
        if (!state.context) state.context = context;
        if (state.started) return;
        state.started = true;
        state.interval = interval();
        static Shutdown shutdown;
        static_cast<void>(shutdown);
        state.thread = std::thread(run);
    } catch (...) {
    }
}

std::string MemoryProbe::FormatLine(const char* tag, std::uint64_t presents, std::uint64_t window, const Events& now, const Events& previous, const std::vector<Gauge>& gauges) {
    std::string line;
    char text[256];
    std::snprintf(text, sizeof(text), "[%s] presents=%llu window=%llu", tag, static_cast<unsigned long long>(presents), static_cast<unsigned long long>(window));
    line += text;
    const double divisor = window == 0 ? 1.0 : static_cast<double>(window);
    std::string created;
    std::uint64_t createdCount = 0, createdBytes = 0;
    for (std::size_t i = 0; i <= static_cast<std::size_t>(Kind::StorageDirect); ++i) {
        const auto count = now[i].count - previous[i].count;
        createdCount += count;
        createdBytes += now[i].bytes - previous[i].bytes;
        if (count == 0) continue;
        std::snprintf(text, sizeof(text), " %s=%llu", KindName(static_cast<Kind>(i)), static_cast<unsigned long long>(count));
        created += text;
    }
    const auto destroyed = static_cast<std::size_t>(Kind::StorageDestroyed);
    std::snprintf(text, sizeof(text), " | storage created +%llu (%.1f MiB; by cause:%s) destroyed +%llu (%.1f MiB) per window, %.2f created/present", static_cast<unsigned long long>(createdCount), createdBytes / MiB, created.empty() ? " none" : created.c_str(), static_cast<unsigned long long>(now[destroyed].count - previous[destroyed].count), (now[destroyed].bytes - previous[destroyed].bytes) / MiB, static_cast<double>(createdCount) / divisor);
    line += text;
    const auto rate = [&](Kind kind, const char* label) {
        const auto index = static_cast<std::size_t>(kind);
        std::snprintf(text, sizeof(text), " %s +%.1f MiB (%.2f MiB/present, %llu ops)", label, (now[index].bytes - previous[index].bytes) / MiB, (now[index].bytes - previous[index].bytes) / MiB / divisor, static_cast<unsigned long long>(now[index].count - previous[index].count));
        line += text;
    };
    line += " |";
    rate(Kind::Upload, "upload");
    rate(Kind::WriteBack, "writeback");
    rate(Kind::OriginalCopy, "original-grown");
    rate(Kind::BaselineCopy, "baseline-grown");
    line += " | live MiB/budget MiB:";
    for (const auto& gauge : gauges) {
        if (gauge.name == "storage_images" || gauge.name == "storage_created_live") std::snprintf(text, sizeof(text), " %s=%llu", gauge.name.c_str(), static_cast<unsigned long long>(gauge.live));
        else if (gauge.budget != 0) std::snprintf(text, sizeof(text), " %s=%.1f/%.0f", gauge.name.c_str(), gauge.live / MiB, gauge.budget / MiB);
        else std::snprintf(text, sizeof(text), " %s=%.1f", gauge.name.c_str(), gauge.live / MiB);
        line += text;
    }
    auto& state = probe();
    std::snprintf(text, sizeof(text), " | storage peak %llu images %.1f MiB", static_cast<unsigned long long>(state.live.peakImages.load(std::memory_order_relaxed)), state.live.peakImageBytes.load(std::memory_order_relaxed) / MiB);
    line += text;
    return line;
}

void MemoryProbe::Flush(const char* tag) noexcept {
    if (!Enabled()) return;
    summary(tag, true);
}

}
