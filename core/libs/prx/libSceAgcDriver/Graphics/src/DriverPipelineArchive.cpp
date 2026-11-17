#include "prx/libSceAgcDriver/Graphics/include/PipelineCache.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GpuCacheStore.hpp"
#include <sys/stat.h>
#include <chrono>
#include <cerrno>
#include <csignal>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <sys/time.h>
#include <unistd.h>

namespace AgcDriver::Graphics {

void ExportDriverPipelineArchivePath(const char* path) {
    setenv("KK_PIPELINE_ARCHIVE", path, 0);
}

void ExportDriverMetallibDirs(const char* prebuiltDir, const char* collectDir) {
    if (prebuiltDir != nullptr) setenv("KK_METALLIB_DIR", prebuiltDir, 0);
    if (collectDir != nullptr) setenv("KK_MSL_COLLECT_DIR", collectDir, 0);
}

namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

constexpr const char* ArchiveName = "kk-archive";

struct Runtime {
    std::unique_ptr<GpuCache::Store> store;
    GpuCache::StageVersions stages;
    fs::path runDir;
    fs::path archiveFile;
    fs::path metallibDir;
    time_t archiveMtime = 0;
    off_t archiveSize = -1;
    time_t started = 0;
    bool owningArchive = false;
    bool owningMetallib = false;
    bool collecting = false;
};
Runtime* runtime = nullptr;
std::once_flag finalizeOnce;

std::string readFile(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

bool statFile(const fs::path& path, struct stat& st) { return ::stat(path.c_str(), &st) == 0; }

// Moves what a run left in `dir` into the store: newly collected MSL and a rewritten pipeline archive.
void ingestRunDir(GpuCache::Store& store, const GpuCache::StageVersions& stages, const fs::path& dir, time_t archiveMtime, off_t archiveSize) {
    std::error_code ec;
    int msl = 0;
    const auto mslDir = dir / "kk-msl";
    if (fs::is_directory(mslDir, ec)) {
        for (const auto& entry : fs::directory_iterator(mslDir, ec)) {
            if (entry.path().extension() != ".metal") continue;
            const auto name = entry.path().stem().string();
            if (store.Has(GpuCache::Kind::Msl, name)) continue;
            const auto text = readFile(entry.path());
            const auto opts = readFile(fs::path(entry.path()).replace_extension(".opts"));
            if (!text.empty() && store.Put(GpuCache::Kind::Msl, name, text.data(), text.size(), GpuCache::Sha256Hex16(text), stages, opts)) ++msl;
        }
    }
    int archive = 0;
    struct stat st;
    const auto archiveFile = dir / "kk-archive.mtl4a";
    if (statFile(archiveFile, st) && st.st_size > 0 && (st.st_mtime != archiveMtime || st.st_size != archiveSize)) {
        const auto data = readFile(archiveFile);
        if (store.Put(GpuCache::Kind::PipelineArchive, ArchiveName, data.data(), data.size(), "", stages, "", 3)) archive = 1;
    }
    if (msl != 0 || archive != 0) std::fprintf(stderr, "[gpu-cache] stored %d MSL, %d pipeline archive\n", msl, archive);
}

void finalize() {
    std::call_once(finalizeOnce, [] {
        if (runtime == nullptr) return;
        auto& r = *runtime;
        const auto t0 = Clock::now();
        if (r.owningArchive || r.collecting) ingestRunDir(*r.store, r.stages, r.runDir, r.archiveMtime, r.archiveSize);
        if (r.owningMetallib) {
            // Prewarm bookkeeping: a materialized library the driver opened has a newer access time than the run start.
            std::error_code ec;
            for (const auto& entry : fs::directory_iterator(r.metallibDir, ec)) {
                struct stat st;
                if (entry.path().extension() == ".metallib" && statFile(entry.path(), st) && st.st_atime > st.st_mtime)
                    r.store->MarkWarm(GpuCache::Kind::Metallib, entry.path().stem().string());
            }
        }
        if (std::uint64_t cap = 0; GpuCache::ParseMegabytes(std::getenv("APS5_GPUCACHE_MAX_MB"), cap) && cap > 0) r.store->Trim(cap);
        std::error_code ec;
        fs::remove_all(r.runDir, ec);
        r.store->Checkpoint();
        std::fprintf(stderr, "[gpu-cache] finalized in %.1f ms\n", std::chrono::duration<double, std::milli>(Clock::now() - t0).count());
    });
}

bool pidAlive(long pid) { return pid > 0 && (::kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM); }

}

bool ConfigureDriverGpuCacheStore(const std::filesystem::path& cacheDir, const GpuCache::StageVersions& stages) {
    const char* off = std::getenv("APS5_GPUCACHE");
    if (off != nullptr && std::strcmp(off, "0") == 0) return false;
    if (runtime != nullptr) return true;
    const auto t0 = Clock::now();
    std::string error;
    auto store = GpuCache::Store::Open(cacheDir / "gpucache.sqlite", &error);
    if (!store) {
        std::fprintf(stderr, "[gpu-cache] cannot open %s: %s; falling back to loose files\n", (cacheDir / "gpucache.sqlite").string().c_str(), error.c_str());
        return false;
    }
    auto rt = std::make_unique<Runtime>();
    rt->stages = stages;
    rt->started = ::time(nullptr);
    const auto report = store->Invalidate(stages);
    if (report.RemovedTotal() != 0) {
        std::fprintf(stderr, "[gpu-cache] stage change (");
        for (int s = 0; s < GpuCache::StageCount; ++s) if (report.changed[static_cast<std::size_t>(s)]) std::fprintf(stderr, " %s", GpuCache::StageName(static_cast<GpuCache::Stage>(s)));
        std::fprintf(stderr, " ): dropped");
        for (int k = 0; k < GpuCache::KindCount; ++k) if (report.removed[static_cast<std::size_t>(k)]) std::fprintf(stderr, " %lld %s", static_cast<long long>(report.removed[static_cast<std::size_t>(k)]), GpuCache::KindName(static_cast<GpuCache::Kind>(k)));
        std::fprintf(stderr, ", kept");
        for (int k = 0; k < GpuCache::KindCount; ++k) if (report.kept[static_cast<std::size_t>(k)]) std::fprintf(stderr, " %lld %s", static_cast<long long>(report.kept[static_cast<std::size_t>(k)]), GpuCache::KindName(static_cast<GpuCache::Kind>(k)));
        std::fprintf(stderr, "\n");
    }
    std::error_code ec;
    // Recover what crashed or killed runs left behind (their atexit never ran).
    const auto runRoot = cacheDir / "run";
    if (fs::is_directory(runRoot, ec)) {
        for (const auto& entry : fs::directory_iterator(runRoot, ec)) {
            const long pid = std::strtol(entry.path().filename().string().c_str(), nullptr, 10);
            if (pidAlive(pid)) continue;
            ingestRunDir(*store, stages, entry.path(), 0, -1);
            fs::remove_all(entry.path(), ec);
        }
    }
    rt->runDir = runRoot / std::to_string(::getpid());
    fs::create_directories(rt->runDir, ec);
    store->ClearWarm();

    if (std::getenv("KK_PIPELINE_ARCHIVE") == nullptr) {
        rt->archiveFile = rt->runDir / "kk-archive.mtl4a";
        const auto n = store->Materialize(GpuCache::Kind::PipelineArchive, rt->runDir, ".mtl4a");
        if (n != 0) fs::rename(rt->runDir / (std::string(ArchiveName) + ".mtl4a"), rt->archiveFile, ec);
        struct stat st;
        if (statFile(rt->archiveFile, st)) { rt->archiveMtime = st.st_mtime; rt->archiveSize = st.st_size; }
        setenv("KK_PIPELINE_ARCHIVE", rt->archiveFile.c_str(), 0);
        rt->owningArchive = true;
    }
    const char* metallibOff = std::getenv("APS5_KK_METALLIB");
    if (std::getenv("KK_METALLIB_DIR") == nullptr && (metallibOff == nullptr || std::strcmp(metallibOff, "0") != 0)) {
        rt->metallibDir = rt->runDir / "kk-metallib";
        double decompressMs = 0;
        std::uint64_t limit = 0;  // 0 = everything; a value that is not a size leaves it there
        if (!GpuCache::ParseMegabytes(std::getenv("APS5_GPUCACHE_MATERIALIZE_MB"), limit)) limit = 0;
        const auto n = store->Materialize(GpuCache::Kind::Metallib, rt->metallibDir, ".metallib", limit, &decompressMs);
        fs::create_directories(rt->metallibDir, ec);
        // Usage tracking: push each file's access time behind its modification time so that a later read by the driver
        // (relatime: atime <= mtime updates on read) is visible at exit.
        for (const auto& entry : fs::directory_iterator(rt->metallibDir, ec)) {
            struct stat st;
            if (!statFile(entry.path(), st)) continue;
            struct timeval times[2] = {{st.st_mtime - 10, 0}, {st.st_mtime, 0}};
            ::utimes(entry.path().c_str(), times);
        }
        std::fprintf(stderr, "[gpu-cache] materialized %zu prebuilt metallibs (decompress %.1f ms)\n", n, decompressMs);
        setenv("KK_METALLIB_DIR", rt->metallibDir.c_str(), 0);
        rt->owningMetallib = true;
    }
    const char* collectEnv = std::getenv("APS5_KK_COLLECT_MSL");
    if (std::getenv("KK_MSL_COLLECT_DIR") == nullptr && (collectEnv == nullptr || std::strcmp(collectEnv, "0") != 0)) {
        fs::create_directories(rt->runDir / "kk-msl", ec);
        setenv("KK_MSL_COLLECT_DIR", (rt->runDir / "kk-msl").c_str(), 0);
        rt->collecting = true;
    }
    std::fprintf(stderr, "[gpu-cache] store %s ready in %.1f ms\n", (cacheDir / "gpucache.sqlite").string().c_str(), std::chrono::duration<double, std::milli>(Clock::now() - t0).count());
    rt->store = std::move(store);
    runtime = rt.release();
    // Registered before vkCreateDevice: the driver registers its own atexit flush later, so it runs first and ours sees the rewritten archive.
    std::atexit([] { finalize(); });
    return true;
}

void FinalizeDriverGpuCacheStore() { finalize(); }

}
