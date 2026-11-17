#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_PIPELINECACHE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_PIPELINECACHE_HPP

#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GpuCacheStore.hpp"
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <span>
#include <thread>
#include <vector>

namespace AgcDriver::Graphics {

class PipelineCache {
public:
    static constexpr int SaveIntervalSeconds = 10;

    PipelineCache(const Context& context, const VkPhysicalDeviceProperties& properties, bool loadFromFile = true);
    ~PipelineCache();

    PipelineCache(const PipelineCache&) = delete;
    PipelineCache& operator=(const PipelineCache&) = delete;
    VkPipelineCache Handle() const { return cache; }
    PipelineCacheSynchronization& Synchronization() { return synchronization; }
    void SaveNow();
    void SaveUnlessBusy();

    friend struct PipelineCacheTestAccess;

private:
    struct FileHeader {
        std::uint32_t magic;
        std::uint32_t format;
        std::uint64_t dataBytes;
        std::uint64_t dataHash;
        bool operator==(const FileHeader&) const = default;
    };
    enum class FileState { Missing, Unreadable, Invalid, Valid };
    struct File {
        FileState state = FileState::Missing;
        const char* invalidReason = nullptr;
        FileHeader header{};
        std::vector<std::byte> contents;
        std::span<const std::byte> Payload() const;
    };

    File readFile() const;
    std::optional<FileHeader> readHeader() const;
    void load(std::vector<std::byte>& initialData);
    void save(bool final, bool waitForPipelines);
    void run();

    Context context;
    VkPhysicalDeviceProperties properties{};
    VkPipelineCache cache = VK_NULL_HANDLE;
    PipelineCacheSynchronization synchronization;
    std::filesystem::path path;
    bool holdsFile = false;
    std::size_t loadedBytes = 0;
    std::size_t settledBytes = 0;
    std::optional<FileHeader> settledFile;
    std::mutex mutex;
    std::condition_variable wake;
    bool stopping = false;
    std::thread saver;
};

// nps5: KosmicKrisp (Vulkan on Metal) can persist the Metal GPU binaries of every pipeline it builds in a
// Metal 4 pipeline archive when KK_PIPELINE_ARCHIVE names a file (patch in the KosmicKrisp tree). This sets
// that variable to a file next to the VkPipelineCache file, named by the same device, driver UUID, OS build
// and shader source version, so a stale archive is never loaded. It must run before vkCreateDevice (KK
// creates its Metal compiler there). It does nothing for other drivers, when the variable is already set,
// or when APS5_KK_PIPELINE_ARCHIVE=0. Stock KosmicKrisp ignores the variable.
// It also points KK_METALLIB_DIR at <cache dir>/<SourceVersion>/kk-metallib (prebuilt libraries, see
// docs/nps5-pipeline-cache.md; off with APS5_KK_METALLIB=0) and, with APS5_KK_COLLECT_MSL=1, KK_MSL_COLLECT_DIR at
// <cache dir>/<SourceVersion>/kk-msl.
void ConfigureDriverPipelineArchive(const VkPhysicalDeviceProperties& properties, const char* driverInfo);

// Sets KK_PIPELINE_ARCHIVE (kept out of PipelineCache.cpp: the shader disk cache key check bans environment
// writes in files that touch the recompiler, and this variable only reaches the Vulkan driver).
void ExportDriverPipelineArchivePath(const char* path);

// Sets KK_METALLIB_DIR (prebuilt Metal libraries the patched KosmicKrisp loads instead of compiling MSL) and
// KK_MSL_COLLECT_DIR (where it writes the MSL of every miss, for tools/kk-metallib/build_metallib.sh). Null skips one.
void ExportDriverMetallibDirs(const char* prebuiltDir, const char* collectDir);

// nps5: routes the three KosmicKrisp artefacts (Metal pipeline archive, prebuilt metallibs, collected MSL) through the
// per-title indexed store <cache dir>/gpucache.sqlite (GpuCacheStore.hpp). Stale entries are dropped by stage version,
// the survivors are decompressed into a per-process run directory that KK_PIPELINE_ARCHIVE / KK_METALLIB_DIR /
// KK_MSL_COLLECT_DIR point at (an explicitly set variable wins and its artefact skips the store), and at exit the new MSL and
// a rewritten archive are compressed back. Returns false (loose-file behaviour) when APS5_GPUCACHE=0 or the store cannot open.
bool ConfigureDriverGpuCacheStore(const std::filesystem::path& cacheDir, const GpuCache::StageVersions& stages);
// Runs the same write-back as the atexit hook (idempotent); for tests and orderly shutdown.
void FinalizeDriverGpuCacheStore();

struct PipelineCacheTestAccess {
    static bool HasHeader(const PipelineCache& cache) {
        return cache.readHeader().has_value();
    }
};

}

#endif
