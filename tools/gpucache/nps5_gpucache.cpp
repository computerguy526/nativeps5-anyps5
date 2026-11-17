// nps5: maintenance CLI for the per-title indexed GPU cache (see docs/nps5-pipeline-cache.md).
//   nps5_gpucache <command> --cache <gpu-cache dir | store file> [options]
// commands: list stats check invalidate trim ingest rebuild verify export import
#include "prx/libSceAgcDriver/Graphics/include/GpuCacheStore.hpp"
#include <sys/sysctl.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace GpuCache;
namespace fs = std::filesystem;

namespace {

struct Args {
    std::string command;
    fs::path cache;
    std::map<std::string, std::string> opt;
    std::vector<std::string> stageOverrides;
    std::vector<std::string> positional;
    bool has(const char* k) const { return opt.count(k) != 0; }
    std::string get(const char* k, const std::string& d = {}) const { auto i = opt.find(k); return i == opt.end() ? d : i->second; }
};

std::string readText(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

std::string line(const std::string& text, const char* key) {
    const std::string prefix = std::string(key) + "=";
    std::size_t pos = 0;
    while (pos < text.size()) {
        auto end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        if (text.compare(pos, prefix.size(), prefix) == 0) return text.substr(pos + prefix.size(), end - pos - prefix.size());
        pos = end + 1;
    }
    return {};
}

std::string osBuild() {
    char b[64] = "unknown";
    std::size_t n = sizeof(b);
    sysctlbyname("kern.osversion", b, &n, nullptr, 0);
    return b;
}

std::string shell(const std::string& cmd) {
    std::string out;
    if (FILE* p = popen(cmd.c_str(), "r")) {
        char buf[256];
        while (std::fgets(buf, sizeof(buf), p)) out += buf;
        pclose(p);
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
}

std::string human(std::uint64_t n) {
    char b[32];
    if (n >= (1u << 20)) std::snprintf(b, sizeof(b), "%.2f MB", static_cast<double>(n) / (1 << 20));
    else std::snprintf(b, sizeof(b), "%.1f KB", static_cast<double>(n) / 1024);
    return b;
}

fs::path storePath(const fs::path& cache) {
    if (cache.extension() == ".sqlite" || cache.extension() == ".gpucache") return cache;
    return cache / "gpucache.sqlite";
}

StageVersions current(const Args& a, const Store& store) {
    StageVersions v = store.LastSeen();
    for (const auto& o : a.stageOverrides) {
        const auto eq = o.find('=');
        Stage s;
        if (eq == std::string::npos || !ParseStage(o.substr(0, eq), s)) { std::fprintf(stderr, "bad --stage %s (name=value; stages: recompiler mesa macos metalc gpu)\n", o.c_str()); std::exit(2); }
        v[s] = o.substr(eq + 1);
    }
    return v;
}

void printReport(const InvalidationReport& r, const char* verb) {
    std::printf("stages changed:");
    bool any = false;
    for (int s = 0; s < StageCount; ++s) if (r.changed[static_cast<std::size_t>(s)]) { std::printf(" %s", StageName(static_cast<Stage>(s))); any = true; }
    if (!any) std::printf(" none");
    std::printf("\n%-10s %8s %8s\n", "kind", verb, "kept");
    for (int k = 0; k < KindCount; ++k) std::printf("%-10s %8lld %8lld\n", KindName(static_cast<Kind>(k)), static_cast<long long>(r.removed[static_cast<std::size_t>(k)]), static_cast<long long>(r.kept[static_cast<std::size_t>(k)]));
}

int cmdList(Store& s, const Args& a) {
    int kind = -1;
    if (a.has("kind")) { Kind k; if (!ParseKind(a.get("kind"), k)) return 2; kind = static_cast<int>(k); }
    std::printf("%-8s %-34s %10s %10s %5s %5s %s\n", "kind", "name", "raw", "stored", "uses", "warm", "versions(recompiler|mesa|macos|metalc|gpu)");
    for (const auto& e : s.List(kind, !a.has("by-id"))) {
        std::printf("%-8s %-34s %10llu %10llu %5lld %5s ", KindName(e.kind), e.name.c_str(), static_cast<unsigned long long>(e.rawBytes), static_cast<unsigned long long>(e.storedBytes), static_cast<long long>(e.useCount), e.warm ? "yes" : "");
        for (int i = 0; i < StageCount; ++i) std::printf("%s%s", i ? "|" : "", e.versions.value[static_cast<std::size_t>(i)].substr(0, 18).c_str());
        std::printf("\n");
    }
    return 0;
}

int cmdStats(Store& s) {
    const auto st = s.GetStats();
    std::uint64_t raw = 0, stored = 0;
    std::printf("%-10s %6s %12s %12s %7s\n", "kind", "count", "raw", "stored", "ratio");
    for (int k = 0; k < KindCount; ++k) {
        raw += st.raw[static_cast<std::size_t>(k)];
        stored += st.stored[static_cast<std::size_t>(k)];
        if (st.count[static_cast<std::size_t>(k)] == 0) continue;
        std::printf("%-10s %6lld %12s %12s %6.2fx\n", KindName(static_cast<Kind>(k)), static_cast<long long>(st.count[static_cast<std::size_t>(k)]), human(st.raw[static_cast<std::size_t>(k)]).c_str(), human(st.stored[static_cast<std::size_t>(k)]).c_str(),
                    st.stored[static_cast<std::size_t>(k)] ? static_cast<double>(st.raw[static_cast<std::size_t>(k)]) / static_cast<double>(st.stored[static_cast<std::size_t>(k)]) : 0.0);
    }
    std::printf("%-10s %6s %12s %12s %6.2fx\nstore file: %s\n", "total", "", human(raw).c_str(), human(stored).c_str(), stored ? static_cast<double>(raw) / static_cast<double>(stored) : 0.0, human(st.fileBytes).c_str());
    const auto seen = s.LastSeen();
    std::printf("last seen stages:\n");
    for (int i = 0; i < StageCount; ++i) std::printf("  %-10s %s\n", StageName(static_cast<Stage>(i)), seen.value[static_cast<std::size_t>(i)].c_str());
    return 0;
}

// Ingest loose files (the pre-store layout): <dir>/<key>.metal + .opts, <dir>/<key>.metallib, an .mtl4a archive.
int cmdIngest(Store& s, const Args& a) {
    const auto v = current(a, s);
    int n = 0;
    if (a.has("msl")) {
        for (const auto& e : fs::directory_iterator(a.get("msl"))) {
            if (e.path().extension() != ".metal") continue;
            const auto text = readText(e.path());
            auto opts = readText(fs::path(e.path()).replace_extension(".opts"));
            if (s.Put(Kind::Msl, e.path().stem().string(), text.data(), text.size(), Sha256Hex16(text), v, opts)) ++n;
        }
    }
    if (a.has("metallib")) {
        for (const auto& e : fs::directory_iterator(a.get("metallib"))) {
            if (e.path().extension() != ".metallib") continue;
            const auto data = readText(e.path());
            StageVersions mv = v;
            const auto manifest = readText(fs::path(a.get("metallib")) / "MANIFEST.txt");
            if (a.has("metalc")) mv[Stage::MetalCompiler] = a.get("metalc");
            else if (!manifest.empty()) { const auto pos = manifest.find("\n", manifest.find("macos:")); mv[Stage::MetalCompiler] = manifest.substr(pos + 1, manifest.find('\n', pos + 1) - pos - 1); }
            if (s.Put(Kind::Metallib, e.path().stem().string(), data.data(), data.size(), "", mv)) ++n;
        }
    }
    if (a.has("archive")) {
        const auto data = readText(a.get("archive"));
        if (s.Put(Kind::PipelineArchive, fs::path(a.get("archive")).stem().string(), data.data(), data.size(), "", v)) ++n;
    }
    if (a.has("vkcache")) {
        const auto data = readText(a.get("vkcache"));
        if (s.Put(Kind::VkPipelineCache, fs::path(a.get("vkcache")).stem().string(), data.data(), data.size(), "", v)) ++n;
    }
    if (a.has("spirv")) {  // directory of shader disk cache entries (<hash>.bin) or *.spv
        for (const auto& e : fs::recursive_directory_iterator(a.get("spirv"))) {
            if (!e.is_regular_file() || (e.path().extension() != ".bin" && e.path().extension() != ".spv")) continue;
            const auto data = readText(e.path());
            if (s.Put(Kind::SpirV, e.path().stem().string(), data.data(), data.size(), e.path().stem().string(), v)) ++n;
        }
    }
    std::printf("ingested %d artefacts\n", n);
    return 0;
}

// Rebuild: every valid MSL row that has no metallib for the current stamp is compiled with Apple's Metal toolchain.
int cmdRebuild(Store& s, const Args& a) {
    const auto v = current(a, s);
    const std::string os = v[Stage::MacOs].empty() ? osBuild() : v[Stage::MacOs];
    std::string gpu = v[Stage::Gpu];
    gpu = gpu.substr(0, gpu.find('|'));
    if (gpu.empty()) gpu = shell("sysctl -n machdep.cpu.brand_string");
    const std::string metalc = shell("xcrun -sdk macosx metal --version 2>/dev/null | head -1");
    if (metalc.empty()) { std::fprintf(stderr, "Metal toolchain not installed (xcodebuild -downloadComponent MetalToolchain)\n"); return 2; }
    StageVersions mv = v;
    mv[Stage::MetalCompiler] = metalc;
    struct Job { std::string msl, key, srcHash, meta; };
    std::vector<Job> jobs;
    for (const auto& e : s.List(static_cast<int>(Kind::Msl), false)) {
        std::string meta = e.meta;
        const auto stamp = line(meta, "stamp");
        const auto mslv = line(meta, "msl"), math = line(meta, "math"), fp = line(meta, "fp");
        if (stamp.empty() || mslv.empty()) continue;
        // stamp is "kk=<mesa version+sha>;os=<build>;" -> same driver, current OS build
        const auto osPos = stamp.find(";os=");
        const std::string newStamp = (osPos == std::string::npos ? stamp : stamp.substr(0, osPos)) + ";os=" + os + ";";
        char opt[96];
        std::snprintf(opt, sizeof(opt), ";msl=%s;math=%s;fp=%s;", mslv.c_str(), math.c_str(), fp.c_str());
        std::vector<std::byte> text;
        if (!s.Get(Kind::Msl, e.name, text, false)) continue;
        std::string all = newStamp + gpu + opt;
        all.append(reinterpret_cast<const char*>(text.data()), text.size());
        const auto key = Sha256Hex16(all);
        if (s.Has(Kind::Metallib, key)) continue;
        jobs.push_back({e.name, key, e.contentHash, meta});
    }
    std::printf("rebuild: %zu metallibs to build (%s, %s)\n", jobs.size(), os.c_str(), metalc.c_str());
    const fs::path tmp = fs::temp_directory_path() / ("nps5gpucache." + std::to_string(::getpid()));
    fs::create_directories(tmp);
    std::atomic<std::size_t> next{0};
    std::atomic<int> built{0}, failed{0};
    std::mutex lock;
    const int nthreads = std::max(1, std::atoi(a.get("jobs", "3").c_str()));
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<std::thread> pool;
    for (int t = 0; t < nthreads; ++t) pool.emplace_back([&] {
        for (;;) {
            const auto i = next++;
            if (i >= jobs.size()) return;
            const auto& j = jobs[i];
            std::vector<std::byte> text;
            { std::lock_guard g(lock); if (!s.Get(Kind::Msl, j.msl, text, false)) { ++failed; continue; } }
            const auto src = tmp / (j.key + ".metal"), air = tmp / (j.key + ".air"), lib = tmp / (j.key + ".metallib");
            { std::ofstream f(src, std::ios::binary); f.write(reinterpret_cast<const char*>(text.data()), static_cast<std::streamsize>(text.size())); }
            const unsigned mslv = static_cast<unsigned>(std::strtoul(line(j.meta, "msl").c_str(), nullptr, 16));
            const int math = std::atoi(line(j.meta, "math").c_str()), fp = std::atoi(line(j.meta, "fp").c_str());
            const char* mm = math == 2 ? "fast" : math == 1 ? "relaxed" : "safe";
            const char* ff = fp == 0 ? "fast" : "precise";
            char cmd[2048];
            std::snprintf(cmd, sizeof(cmd), "xcrun -sdk macosx metal -c -std=metal%u.%u -fmetal-math-mode=%s -fmetal-math-fp32-functions=%s -w '%s' -o '%s' >/dev/null 2>&1 && xcrun -sdk macosx metallib '%s' -o '%s' >/dev/null 2>&1",
                          mslv >> 16, mslv & 0xffff, mm, ff, src.c_str(), air.c_str(), air.c_str(), lib.c_str());
            if (std::system(cmd) != 0) { ++failed; fs::remove(src); continue; }
            const auto data = readText(lib);
            { std::lock_guard g(lock); s.Put(Kind::Metallib, j.key, data.data(), data.size(), j.srcHash, mv, j.meta); }
            ++built;
            fs::remove(src); fs::remove(air); fs::remove(lib);
        }
    });
    for (auto& t : pool) t.join();
    fs::remove_all(tmp);
    std::printf("rebuild: built %d, failed %d, %.1f s\n", built.load(), failed.load(), std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    return failed ? 1 : 0;
}

int cmdVerify(Store& s) {
    int bad = 0, n = 0;
    for (const auto& e : s.List(-1, false)) {
        std::vector<std::byte> data;
        ++n;
        if (!s.Get(e.kind, e.name, data, false) || Sha256Hex16(data.data(), data.size()) != e.contentHash) { std::printf("BAD %s %s\n", KindName(e.kind), e.name.c_str()); ++bad; }
    }
    std::printf("verified %d entries, %d bad\n", n, bad);
    return bad ? 1 : 0;
}

}

int main(int argc, char** argv) {
    Args a;
    if (argc < 2) { std::fprintf(stderr, "usage: nps5_gpucache <list|stats|check|invalidate|trim|ingest|rebuild|verify|export|import> --cache <dir|file> [options]\n"); return 2; }
    a.command = argv[1];
    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg.rfind("--", 0) != 0) { a.positional.push_back(arg); continue; }
        arg = arg.substr(2);
        std::string value = "1";
        if (i + 1 < argc && std::strncmp(argv[i + 1], "--", 2) != 0) value = argv[++i];
        if (arg == "cache") a.cache = value;
        else if (arg == "stage") a.stageOverrides.push_back(value);
        else a.opt[arg] = value;
    }
    if (a.cache.empty()) { std::fprintf(stderr, "--cache is required\n"); return 2; }
    std::string error;
    auto store = Store::Open(storePath(a.cache), &error);
    if (!store) { std::fprintf(stderr, "cannot open store: %s\n", error.c_str()); return 1; }
    const auto& c = a.command;
    if (c == "list") return cmdList(*store, a);
    if (c == "stats") return cmdStats(*store);
    if (c == "check") { printReport(store->Preview(current(a, *store)), "stale"); return 0; }
    if (c == "invalidate") { printReport(store->Invalidate(current(a, *store)), "removed"); return 0; }
    if (c == "trim") { const auto n = store->Trim(static_cast<std::uint64_t>(std::atof(a.get("max-mb", "256").c_str()) * (1 << 20))); std::printf("trimmed %lld entries\n", static_cast<long long>(n)); return 0; }
    if (c == "ingest") return cmdIngest(*store, a);
    if (c == "rebuild") return cmdRebuild(*store, a);
    if (c == "verify") return cmdVerify(*store);
    if (c == "export") {
        std::string id;
        const auto out = a.get("out", "bundle.gpucache");
        const auto gpu = a.get("gpu", current(a, *store)[Stage::Gpu]);
        const auto n = store->ExportBundle(out, gpu, &id);
        std::printf("exported %lld entries for gpu '%s' to %s (bundle id %s)\n", static_cast<long long>(n), gpu.c_str(), out.c_str(), id.c_str());
        return n < 0;
    }
    if (c == "import") {
        if (a.positional.empty()) return 2;
        const auto n = store->ImportBundle(a.positional[0]);
        std::printf("imported %lld entries (versions are checked on the next launch)\n", static_cast<long long>(n));
        return n < 0;
    }
    std::fprintf(stderr, "unknown command %s\n", c.c_str());
    return 2;
}
