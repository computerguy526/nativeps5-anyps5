#include "prx/libSceAgcDriver/Graphics/include/PipelineCache.hpp"
#include "prx/libc/include/HostThread.hpp"
#include "prx/libSceAgcDriver/Graphics/include/PipelineJobs.hpp"
#include "ShaderCacheDirectory.hpp"
#include "ShaderDiskCache.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <new>
#include <span>
#include <string>
#include <vector>
#ifdef __APPLE__
#include <sys/sysctl.h>
#include <sys/types.h>
#endif
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#ifdef __APPLE__
extern char** environ;
#endif

namespace AgcDriver::Graphics {

namespace {

constexpr std::uint32_t FileMagic = 0x50565041u;
constexpr std::uint32_t FileFormat = 1;

// nps5: the pipeline cache file is capped (APS5_VK_PIPELINE_CACHE_MAX_MIB, default 512; 0 turns the cap off). A file above the cap is not read
// (it is replaced by the next save), and a cache whose blob grows past it is not written: a driver that keeps every variant it ever
// saw would otherwise grow without bound and make every launch read and checksum a huge file (the persisted cache is also size-capped in ARMSX3).
std::uint64_t cacheCapBytes() {
    const char* text = std::getenv("APS5_VK_PIPELINE_CACHE_MAX_MIB");
    if (text == nullptr || *text < '0' || *text > '9') return 512ull << 20u;
    return std::strtoull(text, nullptr, 10) << 20u;
}

// Temporary files of a save a killed process left behind (name.tmp.<id>): the atomic write names them so, so an interrupted save is
// never mistaken for a cache. Older than ten minutes means no live save owns them.
void removeStaleTemporaries(const std::filesystem::path& file) {
    std::error_code error;
    const auto directory = file.parent_path();
    const auto prefix = file.filename().string() + ".tmp.";
    for (std::filesystem::directory_iterator it(directory, error), end; !error && it != end; it.increment(error)) {
        const auto name = it->path().filename().string();
        if (name.rfind(prefix, 0) != 0) continue;
        const auto modified = std::filesystem::last_write_time(it->path(), error);
        if (error) { error.clear(); continue; }
        if (std::filesystem::file_time_type::clock::now() - modified > std::chrono::minutes(10)) std::filesystem::remove(it->path(), error);
    }
}

bool profiling() {
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    return profile;
}

std::mutex& pathMutex(const std::filesystem::path& path) {
    struct Registry {
        std::mutex mutex;
        std::map<std::filesystem::path, std::mutex> paths;
    };
    static auto& registry = *new Registry;
    std::lock_guard lock(registry.mutex);
    return registry.paths[path];
}

}

#ifdef __APPLE__

// The environment switches that change what the driver generates (KosmicKrisp's MESA_KK_* and KK_*, and lower-case kk_*
// workaround toggles). The paths this code itself exports are not part of the identity.
static std::string driverFlagsHash() {
    std::vector<std::string> flags;
    for (char** entry = environ; entry != nullptr && *entry != nullptr; ++entry) {
        const std::string text = *entry;
        if (text.rfind("MESA_KK_", 0) != 0 && text.rfind("KK_", 0) != 0 && text.rfind("kk_", 0) != 0) continue;
        const auto name = text.substr(0, text.find('='));
        if (name == "KK_PIPELINE_ARCHIVE" || name == "KK_METALLIB_DIR" || name == "KK_MSL_COLLECT_DIR" || name.find("_DEBUG") != std::string::npos) continue;
        flags.push_back(text);
    }
    std::sort(flags.begin(), flags.end());
    std::string joined;
    for (const auto& flag : flags) joined += flag + "\n";
    char digits[17];
    std::snprintf(digits, sizeof(digits), "%016llx", static_cast<unsigned long long>(std::hash<std::string>{}(joined)));
    return flags.empty() ? "none" : digits;
}

// nps5: the pipeline stamp (recompiler version, driver build, driver switches, macOS build, GPU) of this run against the one the cache
// was last fully built for. A stale stamp is not an error: the key list (pipelines.journal) is bound to the GPU and the recompiler only,
// so after a KosmicKrisp, macOS or driver-flag change the replay rebuilds the binary caches from it at utility QoS. The stamp
// is rewritten when that replay finishes. A recompiler change moves everything to a new <version> directory whose journal starts
// empty: the old journals hold SPIR-V of the old recompiler and cannot be rebuilt (the journal has no recompile requests).
static void ConfigurePipelineStamp(const std::filesystem::path& directory, const char* version, const GpuCache::StageVersions& stages, const char* driverInfo, const VkPhysicalDeviceProperties& properties) {
    static_cast<void>(driverInfo);
    static_cast<void>(properties);
    PipelineStamp stamp;
    stamp.recompiler = version;
    stamp.driver = stages[GpuCache::Stage::Mesa];
    stamp.flags = driverFlagsHash();
    stamp.macos = stages[GpuCache::Stage::MacOs];
    stamp.gpu = stages[GpuCache::Stage::Gpu];
    const auto file = directory / "pipeline.stamp";
    const auto saved = ReadStampFile(file);
    const auto check = CompareStamp(saved, stamp);
    SetCurrentPipelineStamp(stamp, file, check);
    std::error_code error;
    const bool journal = std::filesystem::exists(directory / version / "pipelines.journal", error);
    if (check.state == StampState::Fresh) {
        std::fprintf(stderr, "[pipeline-stamp] fresh (%s)\n", file.string().c_str());
        return;
    }
    std::string changed;
    for (const auto& name : check.changed) changed += (changed.empty() ? "" : ", ") + name;
    if (check.state == StampState::Absent) {
        std::fprintf(stderr, "[pipeline-stamp] none yet; the journal (%s) is replayed and the stamp written when the replay finishes\n", journal ? "present" : "absent");
        return;
    }
    const bool recompilerChanged = std::find(check.changed.begin(), check.changed.end(), "recompiler") != check.changed.end();
    std::fprintf(stderr, "[pipeline-stamp] stale (changed: %s). %s\n", changed.c_str(),
                 recompilerChanged && !journal ? "The recompiler changed: the older journals cannot be rebuilt; this run records a new one." : journal ? "The key journal is kept: the replay rebuilds the pipeline caches from it in the background (utility QoS), the draws it needs first run its jobs themselves." : "No journal to rebuild from.");
}
#endif

void ConfigureDriverPipelineArchive(const VkPhysicalDeviceProperties& properties, const char* driverInfo) {
#ifdef __APPLE__
    if (driverInfo == nullptr || std::strstr(driverInfo, "KosmicKrisp") == nullptr) return;
    if (std::getenv("KK_PIPELINE_ARCHIVE") != nullptr) return;
    if (const char* off = std::getenv("APS5_KK_PIPELINE_ARCHIVE"); off != nullptr && std::strcmp(off, "0") == 0) return;
    const auto directory = ShaderRecompiler::ShaderCacheDirectory();
    if (directory.empty()) return;
    std::string uuid;
    for (const auto byte : properties.pipelineCacheUUID) {
        char digits[3];
        std::snprintf(digits, sizeof(digits), "%02x", static_cast<unsigned>(byte));
        uuid += digits;
    }
    char osBuild[64] = "unknown";
    std::size_t osBuildSize = sizeof(osBuild);
    if (sysctlbyname("kern.osversion", osBuild, &osBuildSize, nullptr, 0) != 0) std::snprintf(osBuild, sizeof(osBuild), "unknown");
    char version[17];
    std::snprintf(version, sizeof(version), "%016llx", static_cast<unsigned long long>(ShaderRecompiler::ShaderDiskCache::SourceVersion()));
    GpuCache::StageVersions stages;
    stages[GpuCache::Stage::Recompiler] = version;
    stages[GpuCache::Stage::Mesa] = std::string(driverInfo) + "|" + uuid + "|" + std::to_string(properties.driverVersion);
    stages[GpuCache::Stage::MacOs] = osBuild;
    char gpu[320];
    std::snprintf(gpu, sizeof(gpu), "%s|%04x:%04x", properties.deviceName, properties.vendorID, properties.deviceID);
    stages[GpuCache::Stage::Gpu] = gpu;
    std::error_code error;
    std::filesystem::create_directories(directory / version, error);
    if (error) return;
    ConfigurePipelineStamp(directory, version, stages, driverInfo, properties);
    if (ConfigureDriverGpuCacheStore(directory, stages)) return;
    // Loose-file fallback (APS5_GPUCACHE=0): the layout used before the indexed store.
    char name[200];
    std::snprintf(name, sizeof(name), "kk-archive-%04x-%04x-%08x-%s-%s.mtl4a", properties.vendorID, properties.deviceID, properties.driverVersion, uuid.c_str(), osBuild);
    const auto path = (directory / version / name).string();
    ExportDriverPipelineArchivePath(path.c_str());
    std::fprintf(stderr, "[pipeline-cache] KosmicKrisp Metal pipeline archive: %s\n", path.c_str());
    const auto metallibOff = std::getenv("APS5_KK_METALLIB");
    if (metallibOff == nullptr || std::strcmp(metallibOff, "0") != 0) {
        const auto prebuilt = (directory / version / "kk-metallib").string();
        const auto collect = (directory / version / "kk-msl").string();
        const auto collecting = std::getenv("APS5_KK_COLLECT_MSL");
        const bool collectOn = collecting != nullptr && std::strcmp(collecting, "1") == 0;
        ExportDriverMetallibDirs(prebuilt.c_str(), collectOn ? collect.c_str() : nullptr);
        std::fprintf(stderr, "[pipeline-cache] KosmicKrisp prebuilt Metal libraries: %s%s\n", prebuilt.c_str(), collectOn ? " (collecting MSL)" : "");
    }
#else
    static_cast<void>(properties);
    static_cast<void>(driverInfo);
#endif
}

PipelineCache::PipelineCache(const Context& context, const VkPhysicalDeviceProperties& properties, bool loadFromFile) : context(context), properties(properties) {
    static_assert(sizeof(FileHeader) == 24);
    const auto directory = ShaderRecompiler::ShaderCacheDirectory();
    if (!directory.empty()) {
        std::string uuid;
        for (const auto byte : properties.pipelineCacheUUID) {
            char digits[3];
            std::snprintf(digits, sizeof(digits), "%02x", static_cast<unsigned>(byte));
            uuid += digits;
        }
        char version[17];
        std::snprintf(version, sizeof(version), "%016llx", static_cast<unsigned long long>(ShaderRecompiler::ShaderDiskCache::SourceVersion()));
        char name[160];
        std::snprintf(name, sizeof(name), "vk-pipelines-%04x-%04x-%08x-%s.bin", properties.vendorID, properties.deviceID, properties.driverVersion, uuid.c_str());
        path = directory / version / name;
    }
    std::vector<std::byte> initialData;
    if (!path.empty()) {
        std::lock_guard pathLock(pathMutex(path));
        removeStaleTemporaries(path);
        if (loadFromFile) load(initialData);
    }
    VkPipelineCacheCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    info.initialDataSize = initialData.size();
    info.pInitialData = initialData.empty() ? nullptr : initialData.data();
    const auto create = context.Function<PFN_vkCreatePipelineCache>("vkCreatePipelineCache");
    VkResult result = create(context.device, &info, nullptr, &cache);
    if (result != VK_SUCCESS && !initialData.empty()) {
        std::fprintf(stderr, "[pipeline-cache] the driver refused %s (Vulkan result %d); starting empty\n", path.string().c_str(), static_cast<int>(result));
        info.initialDataSize = 0;
        info.pInitialData = nullptr;
        loadedBytes = 0;
        settledBytes = 0;
        settledFile.reset();
        result = create(context.device, &info, nullptr, &cache);
    }
    Check(result, "vkCreatePipelineCache");
    try {
        if (!path.empty()) saver = Libc::StartHostThread([this] { run(); });
    } catch (...) {
        context.Function<PFN_vkDestroyPipelineCache>("vkDestroyPipelineCache")(context.device, cache, nullptr);
        throw;
    }
}

PipelineCache::~PipelineCache() {
    if (saver.joinable()) {
        {
            std::lock_guard lock(mutex);
            stopping = true;
        }
        wake.notify_all();
        saver.join();
        save(true, true);
    }
    context.Function<PFN_vkDestroyPipelineCache>("vkDestroyPipelineCache")(context.device, cache, nullptr);
}

std::span<const std::byte> PipelineCache::File::Payload() const {
    return std::span(contents).subspan(sizeof(FileHeader));
}

PipelineCache::File PipelineCache::readFile() const {
    File file;
    if (path.empty()) {
        file.state = FileState::Missing;
        return file;
    }
    {
        std::error_code sizeError;
        const auto cap = cacheCapBytes();
        const auto onDisk = std::filesystem::file_size(path, sizeError);
        if (!sizeError && cap != 0 && onDisk > cap + sizeof(FileHeader)) {
            file.state = FileState::Invalid;
            file.invalidReason = "larger than the size cap (APS5_VK_PIPELINE_CACHE_MAX_MIB)";
            return file;
        }
    }
    if (!ShaderRecompiler::ReadWholeFile(path, file.contents)) {
        std::error_code error;
#ifdef _WIN32
        const auto native = ShaderRecompiler::NativeCachePath_nid_no_patch(path, error);
        const bool exists = !error && std::filesystem::exists(native, error);
#else
        const bool exists = std::filesystem::exists(path, error);
#endif
        file.state = exists || error ? FileState::Unreadable : FileState::Missing;
        return file;
    }
    file.state = FileState::Invalid;
    if (file.contents.size() < sizeof(FileHeader)) {
        file.invalidReason = "truncated";
        return file;
    }
    std::memcpy(&file.header, file.contents.data(), sizeof(FileHeader));
    if (file.header.magic != FileMagic || file.header.format != FileFormat) {
        file.invalidReason = "another format";
        return file;
    }
    if (file.header.dataBytes != file.contents.size() - sizeof(FileHeader)) {
        file.invalidReason = "truncated";
        return file;
    }
    const auto data = file.Payload();
    if (ShaderRecompiler::HashBytes(data) != file.header.dataHash) {
        file.invalidReason = "checksum mismatch";
        return file;
    }
    struct VulkanHeader {
        std::uint32_t headerSize;
        std::uint32_t headerVersion;
        std::uint32_t vendorID;
        std::uint32_t deviceID;
        std::uint8_t uuid[VK_UUID_SIZE];
    } vulkan{};
    static_assert(sizeof(VulkanHeader) == 32);
    if (data.size() < sizeof(vulkan)) {
        file.invalidReason = "no Vulkan header";
        return file;
    }
    std::memcpy(&vulkan, data.data(), sizeof(vulkan));
    if (vulkan.headerSize < sizeof(vulkan) || vulkan.headerVersion != VK_PIPELINE_CACHE_HEADER_VERSION_ONE || vulkan.vendorID != properties.vendorID || vulkan.deviceID != properties.deviceID || std::memcmp(vulkan.uuid, properties.pipelineCacheUUID, VK_UUID_SIZE) != 0) {
        file.invalidReason = "made for another device or driver";
        return file;
    }
    file.state = FileState::Valid;
    return file;
}

std::optional<PipelineCache::FileHeader> PipelineCache::readHeader() const {
    if (path.empty()) return std::nullopt;
#ifdef _WIN32
    std::error_code error;
    const auto native = ShaderRecompiler::NativeCachePath_nid_no_patch(path, error);
    if (error) return std::nullopt;
    const auto file = CreateFileW(native.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return std::nullopt;
    struct Close { HANDLE file; ~Close() { CloseHandle(file); } } close{file};
    FileHeader header{};
    DWORD readBytes = 0;
    if (!ReadFile(file, &header, sizeof(header), &readBytes, nullptr) || readBytes != sizeof(header)) return std::nullopt;
#else
    std::ifstream file(path, std::ios::binary);
    FileHeader header{};
    if (!file.read(reinterpret_cast<char*>(&header), sizeof(header))) return std::nullopt;
#endif
    if (header.magic != FileMagic || header.format != FileFormat) return std::nullopt;
    return header;
}

void PipelineCache::load(std::vector<std::byte>& initialData) {
    auto file = readFile();
    if (file.state == FileState::Missing) return;
    if (file.state == FileState::Unreadable) {
        std::fprintf(stderr, "[pipeline-cache] cannot read %s; starting empty\n", path.string().c_str());
        return;
    }
    if (file.state == FileState::Invalid) {
        std::fprintf(stderr, "[pipeline-cache] ignoring %s: %s\n", path.string().c_str(), file.invalidReason);
        return;
    }
    initialData = std::move(file.contents);
    initialData.erase(initialData.begin(), initialData.begin() + sizeof(FileHeader));
    loadedBytes = initialData.size();
    settledBytes = initialData.size();
    settledFile = file.header;
    holdsFile = true;
    std::fprintf(stderr, "[pipeline-cache] loaded %.1f KiB from %s\n", static_cast<double>(initialData.size()) / 1024.0, path.string().c_str());
}

void PipelineCache::save(bool final, bool waitForPipelines) try {
    if (path.empty()) return;
    std::lock_guard pathLock(pathMutex(path));
    const auto getData = context.Function<PFN_vkGetPipelineCacheData>("vkGetPipelineCacheData");
    const auto snapshot = [&] { return waitForPipelines ? synchronization.LockForSnapshot() : synchronization.TryLockForSnapshot(); };
    std::size_t liveBytes = 0;
    {
        auto synchronizationLock = snapshot();
        if (!synchronizationLock.owns_lock()) return;
        if (getData(context.device, cache, &liveBytes, nullptr) != VK_SUCCESS || liveBytes == 0) return;
    }
    const auto onDisk = readHeader();
    if (liveBytes == settledBytes && onDisk == settledFile) return;
    const bool fileUnchanged = holdsFile && settledFile.has_value() && onDisk == settledFile;
    const auto started = std::chrono::steady_clock::now();
    const auto settle = [&](const std::optional<FileHeader>& file) {
        settledBytes = liveBytes;
        settledFile = file;
    };
    auto disk = fileUnchanged ? File{} : readFile();
    if (disk.state == FileState::Unreadable) {
        std::fprintf(stderr, "[pipeline-cache] cannot read %s; not saving over it\n", path.string().c_str());
        return;
    }
    if (disk.state == FileState::Invalid) std::fprintf(stderr, "[pipeline-cache] replacing %s: %s\n", path.string().c_str(), disk.invalidReason);
    std::size_t floor = fileUnchanged ? std::max<std::size_t>(loadedBytes, settledFile->dataBytes) : 0;
    VkPipelineCache merged = VK_NULL_HANDLE;
    bool mergedWithFile = false;
    struct DestroyMerged {
        const Context& context;
        VkPipelineCache& cache;
        void Reset() {
            if (cache == VK_NULL_HANDLE) return;
            context.Function<PFN_vkDestroyPipelineCache>("vkDestroyPipelineCache")(context.device, cache, nullptr);
            cache = VK_NULL_HANDLE;
        }
        ~DestroyMerged() { Reset(); }
    } destroyMerged{context, merged};
    if (disk.state == FileState::Valid) {
        const auto data = disk.Payload();
        VkPipelineCacheCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
        info.initialDataSize = data.size();
        info.pInitialData = data.data();
        const auto result = context.Function<PFN_vkCreatePipelineCache>("vkCreatePipelineCache")(context.device, &info, nullptr, &merged);
        if (result == VK_SUCCESS) {
            floor = std::max(loadedBytes, data.size());
            mergedWithFile = true;
        } else {
            merged = VK_NULL_HANDLE;
            std::fprintf(stderr, "[pipeline-cache] replacing %s: the driver refused it (Vulkan result %d)\n", path.string().c_str(), static_cast<int>(result));
        }
    }
    std::vector<std::byte>().swap(disk.contents);
    if (merged != VK_NULL_HANDLE) {
        auto synchronizationLock = snapshot();
        if (!synchronizationLock.owns_lock()) return;
        const auto result = context.Function<PFN_vkMergePipelineCaches>("vkMergePipelineCaches")(context.device, merged, 1, &cache);
        if (result != VK_SUCCESS) {
            std::fprintf(stderr, "[pipeline-cache] cannot merge into %s (Vulkan result %d); not saving over it\n", path.string().c_str(), static_cast<int>(result));
            return;
        }
    }
    const auto source = merged != VK_NULL_HANDLE ? merged : cache;
    std::size_t size = 0;
    std::vector<std::byte> file;
    {
        auto synchronizationLock = source == cache ? snapshot() : PipelineCacheSynchronization::SnapshotLock{};
        if (source == cache && !synchronizationLock.owns_lock()) return;
        if (getData(context.device, source, &size, nullptr) != VK_SUCCESS || size == 0) return;
        if (merged != VK_NULL_HANDLE && size == disk.header.dataBytes) {
            settle(disk.header);
            return;
        }
        file.resize(sizeof(FileHeader) + size);
        const auto result = getData(context.device, source, &size, file.data() + sizeof(FileHeader));
        if (result != VK_SUCCESS && result != VK_INCOMPLETE) return;
        file.resize(sizeof(FileHeader) + size);
    }
    if (const auto cap = cacheCapBytes(); cap != 0 && size > cap) {
        // The driver's blob is opaque: single pipelines cannot be evicted from it. Evict the whole generation instead: the persisted file
        // is removed so the next launch (which would otherwise refuse an over-cap file or, with the old file kept, never persist anything
        // new again) starts empty, rebuilds what it needs (the journal replay does it ahead of the draws) and saves a cache under the cap.
        static std::atomic<bool> reported{false};
        std::error_code removeError;
        const bool removed = std::filesystem::remove(path, removeError);
        if (!reported.exchange(true)) std::fprintf(stderr, "[pipeline-cache] %s: the cache holds %.1f MiB, over the %.0f MiB cap (APS5_VK_PIPELINE_CACHE_MAX_MIB); %s so the next launch starts a new generation\n", path.string().c_str(), static_cast<double>(size) / (1024.0 * 1024.0), static_cast<double>(cap) / (1024.0 * 1024.0), removed ? "the old file is removed" : "the old file could not be removed");
        settle(removed ? std::nullopt : onDisk);
        return;
    }
    destroyMerged.Reset();
    if (size < floor) {
        std::fprintf(stderr, "[pipeline-cache] keeping %s: the merged cache has %.1f KiB, less than the %.1f KiB on disk or loaded at start\n", path.string().c_str(), static_cast<double>(size) / 1024.0, static_cast<double>(floor) / 1024.0);
        settle(fileUnchanged ? *settledFile : disk.header);
        return;
    }
    const FileHeader header{FileMagic, FileFormat, size, ShaderRecompiler::HashBytes(std::span(file).subspan(sizeof(FileHeader)))};
    std::memcpy(file.data(), &header, sizeof(header));
    if (!ShaderRecompiler::WriteFileAtomically(path, file)) {
        std::fprintf(stderr, "[pipeline-cache] cannot write %s\n", path.string().c_str());
        return;
    }
    settle(header);
    if (final || profiling()) std::fprintf(stderr, "[pipeline-cache] saved %.1f KiB to %s in %.1f ms%s%s\n", static_cast<double>(size) / 1024.0, path.string().c_str(), std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count(), mergedWithFile ? " (merged with the file)" : "", final ? " (teardown)" : "");
} catch (const std::bad_alloc&) {
    std::fputs("[pipeline-cache] save skipped: host memory exhausted\n", stderr);
}

void PipelineCache::SaveNow() {
    save(false, true);
}

void PipelineCache::SaveUnlessBusy() {
    save(false, false);
}

void PipelineCache::run() {
    std::unique_lock lock(mutex);
    while (!wake.wait_for(lock, std::chrono::seconds(SaveIntervalSeconds), [&] { return stopping; })) {
        lock.unlock();
        save(false, false);
        lock.lock();
    }
}

}
