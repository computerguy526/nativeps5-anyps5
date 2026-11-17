// nps5: unit tests for the indexed GPU cache store (GpuCacheStore): stage-version invalidation only drops dependent
// entries, zstd round trip, prewarm order, trim, materialize, bundle export/import.
#include "prx/libSceAgcDriver/Graphics/include/GpuCacheStore.hpp"
#include <sqlite3.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <atomic>
#include <chrono>
#include <fstream>
#include <vector>

using namespace GpuCache;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

static StageVersions base() {
    StageVersions v;
    v[Stage::Recompiler] = "r1";
    v[Stage::Mesa] = "m1";
    v[Stage::MacOs] = "os1";
    v[Stage::MetalCompiler] = "mc1";
    v[Stage::Gpu] = "gpuA";
    return v;
}

static std::string text(int i) {
    std::string s;
    for (int n = 0; n < 200; ++n) s += "kernel void f" + std::to_string(i) + "_" + std::to_string(n) + "() {}\n";
    return s;
}

static std::unique_ptr<Store> fill(const std::filesystem::path& file) {
    auto store = Store::Open(file);
    CHECK(store != nullptr);
    if (!store) std::exit(1);
    const auto v = base();
    for (int i = 0; i < 4; ++i) {
        const auto t = text(i);
        CHECK(store->Put(Kind::SpirV, "spv" + std::to_string(i), t.data(), t.size(), "req" + std::to_string(i), v));
        CHECK(store->Put(Kind::Msl, "msl" + std::to_string(i), t.data(), t.size(), Sha256Hex16(t), v));
        CHECK(store->Put(Kind::Metallib, "lib" + std::to_string(i), t.data(), t.size(), Sha256Hex16(t), v));
    }
    const auto t = text(99);
    CHECK(store->Put(Kind::PipelineArchive, "archive", t.data(), t.size(), "", v));
    return store;
}

static std::int64_t removed(const InvalidationReport& r, Kind k) { return r.removed[static_cast<std::size_t>(k)]; }
static std::int64_t kept(const InvalidationReport& r, Kind k) { return r.kept[static_cast<std::size_t>(k)]; }

int main() {
    CHECK(Sha256Hex16("abc") == "ba7816bf8f01cfea414140de5dae2223");
    CHECK(Sha256Hex16(std::string()) == "e3b0c44298fc1c149afbf4c8996fb924");

    const auto dir = std::filesystem::temp_directory_path() / ("gpucache_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(dir);

    {   // round trip + compression
        auto store = fill(dir / "a.sqlite");
        std::vector<std::byte> out;
        CHECK(store->Get(Kind::Msl, "msl2", out));
        const auto t = text(2);
        CHECK(out.size() == t.size() && std::memcmp(out.data(), t.data(), t.size()) == 0);
        const auto stats = store->GetStats();
        CHECK(stats.count[static_cast<int>(Kind::Msl)] == 4);
        CHECK(stats.stored[static_cast<int>(Kind::Msl)] * 4 < stats.raw[static_cast<int>(Kind::Msl)]);
        CHECK(!store->Get(Kind::Msl, "missing", out));
    }
    {   // same versions: nothing invalidated
        auto store = Store::Open(dir / "a.sqlite");
        const auto report = store->Invalidate(base());
        CHECK(report.RemovedTotal() == 0);
        CHECK(kept(report, Kind::Metallib) == 4);
    }
    {   // macOS bump: metallib + archive drop; spirv and msl stay
        auto store = Store::Open(dir / "a.sqlite");
        auto v = base();
        v[Stage::MacOs] = "os2";
        const auto preview = store->Preview(v);
        CHECK(removed(preview, Kind::Metallib) == 4 && removed(preview, Kind::PipelineArchive) == 1);
        CHECK(store->List().size() == 13);  // preview deleted nothing
        const auto report = store->Invalidate(v);
        CHECK(removed(report, Kind::Metallib) == 4);
        CHECK(removed(report, Kind::PipelineArchive) == 1);
        CHECK(removed(report, Kind::SpirV) == 0 && removed(report, Kind::Msl) == 0);
        CHECK(report.changed[static_cast<int>(Stage::MacOs)] && !report.changed[static_cast<int>(Stage::Mesa)]);
        CHECK(store->Has(Kind::SpirV, "spv0") && store->Has(Kind::Msl, "msl3") && !store->Has(Kind::Metallib, "lib0"));
        CHECK(store->LastSeen()[Stage::MacOs] == "os2");
    }
    {   // untracked stage (empty current) never invalidates; Metal compiler touches only metallibs
        auto store = fill(dir / "b.sqlite");
        StageVersions v = base();
        v[Stage::MetalCompiler] = "";
        v[Stage::MacOs] = "";
        CHECK(store->Invalidate(v).RemovedTotal() == 0);
        v = base();
        v[Stage::MetalCompiler] = "mc2";
        const auto report = store->Invalidate(v);
        CHECK(removed(report, Kind::Metallib) == 4 && report.RemovedTotal() == 4);
    }
    {   // recompiler bump drops everything; mesa bump keeps spirv; gpu bump keeps spirv
        auto store = fill(dir / "c.sqlite");
        auto v = base();
        v[Stage::Mesa] = "m2";
        auto report = store->Invalidate(v);
        CHECK(removed(report, Kind::SpirV) == 0 && removed(report, Kind::Msl) == 4);
        // nps5-prebuild2: a KosmicKrisp build change keeps the metallibs and the Metal archive (content-addressed / validated by Metal)
        CHECK(removed(report, Kind::Metallib) == 0 && removed(report, Kind::PipelineArchive) == 0);
        auto s2 = fill(dir / "d.sqlite");
        v = base();
        v[Stage::Recompiler] = "r2";
        report = s2->Invalidate(v);
        CHECK(report.RemovedTotal() == 13 - 4);  // metallib does not depend on the recompiler
        CHECK(kept(report, Kind::Metallib) == 4);
        auto s3 = fill(dir / "e.sqlite");
        v = base();
        v[Stage::Gpu] = "gpuB";
        report = s3->Invalidate(v);
        CHECK(removed(report, Kind::SpirV) == 0 && removed(report, Kind::Msl) == 4 && removed(report, Kind::Metallib) == 4);
    }
    {   // prewarm order: warm + most recently used first; trim drops coldest
        auto store = fill(dir / "f.sqlite");
        store->ClearWarm();
        store->MarkWarm(Kind::Metallib, "lib2");
        store->MarkWarm(Kind::Metallib, "lib2");
        store->MarkWarm(Kind::Metallib, "lib1");
        const auto order = store->List(static_cast<int>(Kind::Metallib));
        CHECK(order.size() == 4 && order[0].warm && order[1].warm && !order[3].warm);
        CHECK(order[0].name == "lib2" && order[0].useCount == 2);
        std::uint64_t total = 0;
        for (auto b : store->GetStats().stored) total += b;
        const auto dropped = store->Trim(total - 1);
        CHECK(dropped >= 1);
        CHECK(store->Has(Kind::Metallib, "lib2"));  // warm entries survive
        // materialize
        const auto out = dir / "mat";
        double ms = 0;
        const auto n = store->Materialize(Kind::Metallib, out, ".metallib", 0, &ms);
        CHECK(n == store->List(static_cast<int>(Kind::Metallib)).size());
        CHECK(std::filesystem::exists(out / "lib2.metallib"));
        CHECK(std::filesystem::file_size(out / "lib2.metallib") == text(2).size());
        CHECK(store->Materialize(Kind::Metallib, out, ".metallib") == 0);  // already there
    }
    {   // bundle: export per GPU, import is idempotent
        auto store = fill(dir / "g.sqlite");
        const auto t = text(7);
        auto other = base();
        other[Stage::Gpu] = "gpuB";
        CHECK(store->Put(Kind::Msl, "mslB", t.data(), t.size(), "", other));
        std::string id;
        const auto exported = store->ExportBundle(dir / "bundle.gpucache", "gpuA", &id);
        CHECK(exported == 13);  // gpuB's msl is left out, spirv (gpu independent) included
        CHECK(id.size() == 32);
        auto fresh = Store::Open(dir / "h.sqlite");
        CHECK(fresh->ImportBundle(dir / "bundle.gpucache") == 13);
        CHECK(fresh->ImportBundle(dir / "bundle.gpucache") == 0);
        CHECK(fresh->Has(Kind::PipelineArchive, "archive") && !fresh->Has(Kind::Msl, "mslB"));
    }
    {   // corrupt and partial entries are detected, never returned, and dropped so the next run rebuilds them
        const auto file = dir / "corrupt.sqlite";
        {
            auto store = fill(file);
        }
        sqlite3* db = nullptr;
        CHECK(sqlite3_open(file.string().c_str(), &db) == SQLITE_OK);
        // msl0: one flipped byte in the middle of the zstd blob; msl1: truncated blob; msl2: same-size garbage after the frame header.
        auto exec = [&](const char* sql) { char* err = nullptr; CHECK(sqlite3_exec(db, sql, nullptr, nullptr, &err) == SQLITE_OK); sqlite3_free(err); };
        exec("UPDATE artefact SET data = substr(data,1,length(data)/2) || X'FF' || substr(data,length(data)/2+2) WHERE kind=1 AND name='msl0'");
        exec("UPDATE artefact SET data = substr(data,1,length(data)/2) WHERE kind=1 AND name='msl1'");
        sqlite3_close(db);
        auto store = Store::Open(file);
        std::vector<std::byte> out;
        CHECK(!store->Get(Kind::Msl, "msl0", out));
        CHECK(!store->Get(Kind::Msl, "msl1", out));
        CHECK(!store->Has(Kind::Msl, "msl0") && !store->Has(Kind::Msl, "msl1"));  // dropped
        CHECK(store->Get(Kind::Msl, "msl2", out));                                // neighbours untouched
        const auto t = text(2);
        CHECK(out.size() == t.size() && std::memcmp(out.data(), t.data(), t.size()) == 0);
        // Materialize skips a damaged entry instead of writing it.
        db = nullptr;
        store.reset();
        CHECK(sqlite3_open(file.string().c_str(), &db) == SQLITE_OK);
        exec("UPDATE artefact SET data = substr(data,1,length(data)-3) WHERE kind=1 AND name='msl3'");
        sqlite3_close(db);
        store = Store::Open(file);
        const auto outDir = dir / "mat";
        const auto written = store->Materialize(Kind::Msl, outDir, ".metal");
        CHECK(written == 1);  // only msl2 is left intact
    }
    {   // concurrent writers and readers (the run's exit write-back and the prewarm pool share one store)
        auto store = Store::Open(dir / "conc.sqlite");
        const auto v = base();
        std::vector<std::thread> threads;
        std::atomic<int> bad{0};
        for (int th = 0; th < 8; ++th) {
            threads.emplace_back([&, th] {
                for (int n = 0; n < 40; ++n) {
                    const auto name = "k" + std::to_string(th) + "_" + std::to_string(n);
                    const auto t = text(th * 100 + n);
                    if (!store->Put(Kind::Msl, name, t.data(), t.size(), "", v)) ++bad;
                    std::vector<std::byte> out;
                    if (!store->Get(Kind::Msl, name, out) || out.size() != t.size() || std::memcmp(out.data(), t.data(), t.size()) != 0) ++bad;
                }
            });
        }
        for (auto& t : threads) t.join();
        CHECK(bad == 0);
        CHECK(store->GetStats().count[static_cast<int>(Kind::Msl)] == 320);
    }
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    if (failures == 0) std::puts("gpucache store tests passed");
    return failures == 0 ? 0 : 1;
}
