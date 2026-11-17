// WP0 memprobe unit tests: creation-cause classification, counters, the cause scope and the summary
// line. No Vulkan device is needed (the live-image walk is covered by the title run).
#include "prx/libSceAgcDriver/Graphics/include/MemoryProbe.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <string>

using namespace AgcDriver::Graphics;

namespace {

int failures = 0;

void Check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

GuestTextureResource Base() {
    GuestTextureResource resource{};
    resource.baseAddress = 0x100000;
    resource.width = 1920;
    resource.height = 1080;
    resource.depthOrLastArray = 0;
    resource.baseArray = 0;
    resource.mipCount = 1;
    resource.format = 10;
    return resource;
}

}

// Probe off (the default): nothing is counted, nothing is live, Flush is silent and harmless.
// ns per counter update, best of several runs, one thread (on: relaxed fetch_add x2; off: a load and a branch).
double NsPerUpdate() {
    double best = 1e9;
    for (int run = 0; run < 5; ++run) {
        constexpr int N = 2000000;
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < N; ++i) MemoryProbe::Add(MemoryProbe::Kind::WriteBack, 4096);
        const auto elapsed = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count();
        best = std::min(best, elapsed / N);
    }
    return best;
}

int OffMain() {
    Check(!MemoryProbe::Enabled(), "unset APS5_MEM_PROBE leaves the probe off");
    MemoryProbe::StorageCreated(MemoryProbe::Kind::StorageNew, 1u << 20u);
    MemoryProbe::Add(MemoryProbe::Kind::WriteBack, 1u << 20u);
    MemoryProbe::StorageDestroyed(1u << 20u);
    MemoryProbe::Flush();
    const auto counters = MemoryProbe::Counters();
    for (const auto& event : counters) Check(event.count == 0 && event.bytes == 0, "off: no event is counted");
    Check(MemoryProbe::LiveImages() == 0 && MemoryProbe::LiveImageBytes() == 0 && MemoryProbe::PeakImages() == 0 && MemoryProbe::PeakImageBytes() == 0, "off: no live or peak state");
    const auto off = NsPerUpdate();
    std::printf("memory_probe bench: off %.2f ns per update\n", off);
    Check(off < 20.0, "off: an update costs a load and a branch (under 20 ns even on a loaded machine)");
    if (failures == 0) std::printf("memory_probe off: all checks passed\n");
    return failures == 0 ? 0 : 1;
}

int OnMain();

// The probe reads APS5_MEM_PROBE once while the library loads, so each mode is its own process.
int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "off") return OffMain();
    if (argc > 1 && std::string(argv[1]) == "on") return OnMain();
    const std::string self = std::string("\"") + argv[0] + "\"";
    Check(std::system(("env -u APS5_MEM_PROBE " + self + " off").c_str()) == 0, "the off-mode process passed");
    Check(std::system(("env APS5_MEM_PROBE=30 " + self + " on").c_str()) == 0, "the on-mode process passed");
    return failures == 0 ? 0 : 1;
}

int OnMain() {
    Check(MemoryProbe::Enabled(), "APS5_MEM_PROBE=30 enables the probe");

    const auto base = Base();
    Check(MemoryProbe::ClassifyCreation(nullptr, base, false) == MemoryProbe::Kind::StorageNew, "nothing cached at the address: new");
    auto other = base;
    other.width = 1280;
    other.height = 720;
    Check(MemoryProbe::ClassifyCreation(&other, base, false) == MemoryProbe::Kind::StorageExtent, "smaller extent at the same base: extent");
    other = base;
    other.mipCount = 4;
    Check(MemoryProbe::ClassifyCreation(&other, base, false) == MemoryProbe::Kind::StorageMips, "mip count differs: mips");
    other = base;
    other.depthOrLastArray = 5;
    Check(MemoryProbe::ClassifyCreation(&other, base, false) == MemoryProbe::Kind::StorageLayers, "array window differs: layers");
    other = base;
    other.baseArray = 2;
    Check(MemoryProbe::ClassifyCreation(&other, base, false) == MemoryProbe::Kind::StorageLayers, "base array differs: layers");
    other = base;
    other.format = 22;
    Check(MemoryProbe::ClassifyCreation(&other, base, false) == MemoryProbe::Kind::StorageFormat, "format differs: format");
    other = base;
    other.pipeBankXor = 3;
    Check(MemoryProbe::ClassifyCreation(&other, base, false) == MemoryProbe::Kind::StorageOther, "only the pipe XOR differs: other");
    Check(MemoryProbe::ClassifyCreation(&other, base, true) == MemoryProbe::Kind::StorageDcc, "moved DCC metadata wins over every other difference");

    // The scope names the next Create's cause once, then falls back to direct.
    Check(MemoryProbe::TakeCause() == MemoryProbe::Kind::StorageDirect, "default cause is direct");
    {
        const MemoryProbe::CauseScope scope(MemoryProbe::Kind::StorageExtent);
        Check(MemoryProbe::TakeCause() == MemoryProbe::Kind::StorageExtent, "scope sets the cause");
        Check(MemoryProbe::TakeCause() == MemoryProbe::Kind::StorageDirect, "Take clears the cause");
    }

    const auto before = MemoryProbe::Counters();
    MemoryProbe::StorageCreated(MemoryProbe::Kind::StorageExtent, 8u << 20u);
    MemoryProbe::StorageCreated(MemoryProbe::Kind::StorageNew, 4u << 20u);
    MemoryProbe::StorageDestroyed(8u << 20u);
    MemoryProbe::Add(MemoryProbe::Kind::OriginalCopy, 1u << 20u);
    MemoryProbe::Add(MemoryProbe::Kind::WriteBack, 2u << 20u);
    MemoryProbe::Add(MemoryProbe::Kind::WriteBack, 2u << 20u);
    const auto after = MemoryProbe::Counters();
    const auto delta = [&](MemoryProbe::Kind kind) {
        const auto index = static_cast<std::size_t>(kind);
        return std::pair{after[index].count - before[index].count, after[index].bytes - before[index].bytes};
    };
    Check(delta(MemoryProbe::Kind::StorageExtent) == std::pair<std::uint64_t, std::uint64_t>{1, 8u << 20u}, "extent creation counted with its bytes");
    Check(delta(MemoryProbe::Kind::StorageNew) == std::pair<std::uint64_t, std::uint64_t>{1, 4u << 20u}, "new creation counted with its bytes");
    Check(delta(MemoryProbe::Kind::StorageDestroyed) == std::pair<std::uint64_t, std::uint64_t>{1, 8u << 20u}, "destroy counted");
    Check(delta(MemoryProbe::Kind::WriteBack) == std::pair<std::uint64_t, std::uint64_t>{2, 4u << 20u}, "write-backs accumulate");
    Check(delta(MemoryProbe::Kind::OriginalCopy).second == (1u << 20u), "original growth counted");

    const std::vector<MemoryProbe::Gauge> gauges{{"storage_images", 7, 0}, {"original", 3u << 20u, 0}, {"pool_large", 100u << 20u, 512u << 20u}};
    const auto line = MemoryProbe::FormatLine("mem", 600, 30, after, before, gauges);
    std::printf("%s\n", line.c_str());
    const auto has = [&](const char* text) { return line.find(text) != std::string::npos; };
    Check(has("[mem] presents=600 window=30"), "line names presents and window");
    Check(has("created +2") && has("extent=1") && has("new=1"), "line gives the creations by cause");
    Check(has("destroyed +1 (8.0 MiB)"), "line gives destroyed images and bytes");
    Check(has("writeback +4.0 MiB (0.13 MiB/present, 2 ops)"), "line gives write-back per present");
    Check(has("original=3.0") && has("pool_large=100.0/512") && has("storage_images=7"), "line gives the gauges against their budgets");

    // Churn: exact live accounting after creates, destroys and a peak, single thread.
    const auto liveBefore = MemoryProbe::LiveImages(), bytesBefore = MemoryProbe::LiveImageBytes();
    // The three images above: two created, one destroyed.
    Check(liveBefore == 1 && bytesBefore == (4u << 20u), "live image count and bytes follow create and destroy exactly");
    std::uint64_t expectBytes = bytesBefore, expectLive = liveBefore, peakBytes = MemoryProbe::PeakImageBytes();
    std::uint64_t seed = 12345;
    std::vector<std::uint64_t> held;
    const auto churnBefore = MemoryProbe::Counters();
    std::uint64_t created = 0, destroyedCount = 0, createdBytes = 0, destroyedBytes = 0;
    for (int i = 0; i < 2000; ++i) {
        seed = seed * 6364136223846793005ull + 1442695040888963407ull;
        if ((seed >> 33u) % 3 != 0 || held.empty()) {
            const std::uint64_t bytes = (((seed >> 20u) % 64) + 1) * 65536;
            MemoryProbe::StorageCreated(static_cast<MemoryProbe::Kind>((seed >> 40u) % 8), bytes);
            held.push_back(bytes);
            expectBytes += bytes;
            ++expectLive;
            ++created;
            createdBytes += bytes;
            peakBytes = std::max(peakBytes, expectBytes);
        } else {
            const auto bytes = held.back();
            held.pop_back();
            MemoryProbe::StorageDestroyed(bytes);
            expectBytes -= bytes;
            --expectLive;
            ++destroyedCount;
            destroyedBytes += bytes;
        }
    }
    const auto churnAfter = MemoryProbe::Counters();
    std::uint64_t causeCount = 0, causeBytes = 0;
    for (std::size_t i = 0; i <= static_cast<std::size_t>(MemoryProbe::Kind::StorageDirect); ++i) {
        causeCount += churnAfter[i].count - churnBefore[i].count;
        causeBytes += churnAfter[i].bytes - churnBefore[i].bytes;
    }
    const auto destroyedIndex = static_cast<std::size_t>(MemoryProbe::Kind::StorageDestroyed);
    Check(causeCount == created && causeBytes == createdBytes, "creations by cause add up to the creations made");
    Check(churnAfter[destroyedIndex].count - churnBefore[destroyedIndex].count == destroyedCount && churnAfter[destroyedIndex].bytes - churnBefore[destroyedIndex].bytes == destroyedBytes, "destroy count and bytes match after churn");
    Check(MemoryProbe::LiveImages() == expectLive && MemoryProbe::LiveImageBytes() == expectBytes, "live images and bytes equal creates minus destroys after churn");
    Check(MemoryProbe::PeakImageBytes() == peakBytes, "peak bytes is the maximum live bytes seen");
    Check(MemoryProbe::PeakImages() >= MemoryProbe::LiveImages(), "peak image count is at least the live count");
    // Drain: everything destroyed returns the live state to its start.
    while (!held.empty()) {
        MemoryProbe::StorageDestroyed(held.back());
        held.pop_back();
    }
    Check(MemoryProbe::LiveImages() == liveBefore && MemoryProbe::LiveImageBytes() == bytesBefore, "destroying every held image restores the live state");

    // Concurrent churn: no lost updates.
    {
        const auto start = MemoryProbe::Counters();
        std::vector<std::thread> threads;
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([] {
                for (int i = 0; i < 5000; ++i) {
                    MemoryProbe::StorageCreated(MemoryProbe::Kind::StorageNew, 4096);
                    MemoryProbe::Add(MemoryProbe::Kind::Upload, 16);
                    MemoryProbe::StorageDestroyed(4096);
                }
            });
        }
        for (auto& thread : threads) thread.join();
        const auto end = MemoryProbe::Counters();
        const auto n = static_cast<std::size_t>(MemoryProbe::Kind::StorageNew), d = static_cast<std::size_t>(MemoryProbe::Kind::StorageDestroyed), u = static_cast<std::size_t>(MemoryProbe::Kind::Upload);
        Check(end[n].count - start[n].count == 20000 && end[d].count - start[d].count == 20000 && end[u].bytes - start[u].bytes == 20000u * 16u, "concurrent counters lose no updates");
        Check(MemoryProbe::LiveImages() == liveBefore && MemoryProbe::LiveImageBytes() == bytesBefore, "concurrent create/destroy leaves the live state unchanged");
    }

    // Summary consistency: consecutive windows tile the counters (deltas sum to the totals) and the
    // per-present rates are the window totals over the window.
    {
        const auto s0 = MemoryProbe::Counters();
        MemoryProbe::StorageCreated(MemoryProbe::Kind::StorageMips, 3u << 20u);
        MemoryProbe::Add(MemoryProbe::Kind::Upload, 6u << 20u);
        const auto s1 = MemoryProbe::Counters();
        MemoryProbe::StorageCreated(MemoryProbe::Kind::StorageMips, 3u << 20u);
        MemoryProbe::StorageCreated(MemoryProbe::Kind::StorageLayers, 1u << 20u);
        MemoryProbe::Add(MemoryProbe::Kind::Upload, 6u << 20u);
        MemoryProbe::StorageDestroyed(3u << 20u);
        MemoryProbe::StorageDestroyed(3u << 20u);
        MemoryProbe::StorageDestroyed(1u << 20u);
        const auto s2 = MemoryProbe::Counters();
        const auto first = MemoryProbe::FormatLine("mem", 30, 30, s1, s0, {});
        const auto second = MemoryProbe::FormatLine("mem", 60, 30, s2, s1, {});
        Check(first.find("created +1 (3.0 MiB; by cause: mips=1)") != std::string::npos, "window 1 reports its one mips creation");
        Check(second.find("created +2 (4.0 MiB; by cause: mips=1 layers=1) destroyed +3 (7.0 MiB)") != std::string::npos, "window 2 reports only its own creations and destroys");
        Check(second.find("upload +6.0 MiB (0.20 MiB/present, 1 ops)") != std::string::npos, "per-present rate is the window bytes over the window presents");
        Check(second.find("[mem] presents=60 window=30") != std::string::npos, "the second line carries the running present total");
        const auto none = MemoryProbe::FormatLine("mem", 90, 30, s2, s2, {});
        Check(none.find("created +0 (0.0 MiB; by cause: none) destroyed +0") != std::string::npos, "an idle window reports zeros");
        const auto zero = MemoryProbe::FormatLine("final", 0, 0, s2, s0, {});
        Check(zero.find("window=0") != std::string::npos && zero.find("nan") == std::string::npos && zero.find("inf") == std::string::npos, "a zero-present window does not divide by zero");
    }
    MemoryProbe::Flush("test");
    const auto on = NsPerUpdate();
    std::printf("memory_probe bench: on %.2f ns per update (two relaxed fetch_add, no lock, no allocation)\n", on);
    Check(on < 200.0, "on: an update stays far below a lock or an allocation (under 200 ns even on a loaded machine)");
    if (failures == 0) std::printf("memory_probe: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
