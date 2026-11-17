// Write watch stress, fuzz, race, edge-page and micro-benchmark tests (WP4 watchreg).
// Design reference for the exact-semantics checks: KytyPS5 (GPL-2.0) textureCache.cpp:295-317, 1942-1966, image.cpp:730-746;
// the tests themselves are original.
//
// modes:
//   query            QueryRange: coverage, shared head/tail pages, flagged page counts, 16 KiB host pages vs 4 KiB guest offsets
//   edge             a write on the shared head/tail page outside the registered bytes is flagged; 4 KiB offsets inside one host page
//   fuzz             single thread randomized ops against a reference shadow (exact with fault-ahead off, superset otherwise)
//   fuzz-exact       same with APS5_WRITE_WATCH_FAULT_AHEAD=0 set by the test: reported == shadow, exactly
//   race-collect     writer threads vs collector vs host-write (GPU completion) thread vs register/unregister churn: no lost write
//   race-query       QueryRange / Covers / Collect concurrent with faults and layout churn: no crash, answers consistent
//   bench            micro-benchmarks (fault, collect, query, register); always passes, prints numbers
//
// A write is "lost" when a consumer that collects and then reads the bytes sees a stale copy: the watch must report every page
// whose bytes changed between two collects (a store racing the collect itself is covered by the read that follows the collect).

#include "prx/libc/include/GuestWriteWatch.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <sys/mman.h>
#include <unistd.h>

#if !defined(__APPLE__)
#error The page protection write watch is the macOS backend
#endif

using namespace GuestWriteWatch;

namespace {

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::string Fmt(const char* format, unsigned long long a = 0, unsigned long long b = 0, unsigned long long c = 0) {
    char buffer[256];
    std::snprintf(buffer, sizeof(buffer), format, a, b, c);
    return buffer;
}

struct Rng {
    std::uint64_t state;
    explicit Rng(std::uint64_t seed) : state(seed * 0x9E3779B97F4A7C15ull + 0x1234567ull) {}
    std::uint64_t Next() {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    }
    std::size_t Below(std::size_t limit) { return static_cast<std::size_t>(Next() % limit); }
};

std::uint64_t Seed(std::uint64_t fallback) {
    if (const char* text = std::getenv("APS5_STRESS_SEED")) return std::strtoull(text, nullptr, 0);
    return fallback;
}

double Seconds(std::chrono::steady_clock::time_point since) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - since).count();
}

struct Region {
    explicit Region(std::size_t pageCount) : page(static_cast<std::size_t>(::getpagesize())), bytes(pageCount * page) {
        base = static_cast<std::uint8_t*>(::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0));
        Require(base != MAP_FAILED, "cannot map the test region");
    }
    ~Region() {
        GuestWriteWatchUnregister_nid_postfix(base, bytes);
        ::munmap(base, bytes);
    }
    Region(const Region&) = delete;
    Region& operator=(const Region&) = delete;
    std::uintptr_t Address(std::size_t index) const { return reinterpret_cast<std::uintptr_t>(base) + index * page; }
    std::size_t Pages() const { return bytes / page; }
    std::size_t page;
    std::size_t bytes;
    std::uint8_t* base = nullptr;
};

struct CollectSink {
    const Region* region;
    std::set<std::size_t> pages;
};

void SinkCallback(void* context, std::uintptr_t begin, std::uintptr_t end) {
    auto& sink = *static_cast<CollectSink*>(context);
    for (auto at = begin; at < end; at += sink.region->page) sink.pages.insert((at - sink.region->Address(0)) / sink.region->page);
}

// Collects [address, address + bytes) and returns the reported page indexes of the region. `ok` receives the return value.
std::set<std::size_t> CollectRange(const Region& region, std::uintptr_t address, std::size_t bytes, bool* ok = nullptr) {
    CollectSink sink{&region, {}};
    const bool result = GuestWriteWatchCollect_nid_postfix(address, bytes, &SinkCallback, &sink);
    if (ok != nullptr) *ok = result;
    return sink.pages;
}

void CheckQuery() {
    Require(GuestWriteWatchAvailable_nid_postfix(), "write watch is unavailable");
    Region region(16);
    const auto page = region.page;
    GuestWriteWatchRangeInfo info;
    Require(!GuestWriteWatchQueryRange_nid_postfix(region.Address(0), page * 2, &info), "an unregistered range is covered");
    Require(!info.covered && info.watchedPages == 0 && info.pageBytes == page, "unregistered range info is wrong");
    Require(!GuestWriteWatchQueryRange_nid_postfix(region.Address(0), 0, &info), "an empty range is covered");
    Require(!GuestWriteWatchQueryRange_nid_postfix(~std::uintptr_t{0} - 8, 64, &info), "a wrapping range is covered");
    GuestWriteWatchQueryRange_nid_postfix(region.Address(0), page, nullptr);  // null info must not crash

    // An unaligned registration: the watch widens to whole host pages, and the query reports the edge pages as shared.
    const auto begin = region.Address(2) + 4096;
    const std::size_t length = 3 * page;  // ends 4 KiB into page 5
    GuestWriteWatchRegister_nid_postfix(reinterpret_cast<void*>(begin), length);
    Require(GuestWriteWatchQueryRange_nid_postfix(begin, length, &info), "the registered range is not covered");
    Require(info.covered && info.headShared && info.tailShared, "the edge pages are not reported as shared");
    Require(info.watchedPages == 4 && info.writtenPages == 4 && info.headWritten && info.tailWritten, "a new registration reads as fully written");
    Require(info.trappedPages == 0, "a new registration is read trapped");
    // A page-aligned part of it has no shared edge.
    Require(GuestWriteWatchQueryRange_nid_postfix(region.Address(3), page * 2, &info) && !info.headShared && !info.tailShared, "aligned query reports shared edges");
    // Part inside, part outside: not covered, and still counts what is watched.
    Require(!GuestWriteWatchQueryRange_nid_postfix(region.Address(4), page * 4, &info), "a range running out of the watch is covered");
    Require(!info.covered && info.watchedPages == 2, "partially watched range counts wrong");
    // Query does not consume: collecting afterwards still reports every page.
    GuestWriteWatchQueryRange_nid_postfix(begin, length, &info);
    const auto all = CollectRange(region, begin, length);
    Require(all == std::set<std::size_t>({2, 3, 4, 5}), "collect after query lost pages");
    Require(GuestWriteWatchQueryRange_nid_postfix(begin, length, &info) && info.writtenPages == 0 && !info.headWritten && !info.tailWritten, "collected pages still read as written");
    // A write marks exactly that page; first and last flags follow.
    region.base[3 * page + 5] = 1;
    GuestWriteWatchQueryRange_nid_postfix(begin, length, &info);
    Require(info.writtenPages == 1 && !info.headWritten && !info.tailWritten, "interior write is not counted once");
    region.base[2 * page + 4096 + 1] = 1;
    region.base[5 * page + 100] = 1;
    GuestWriteWatchQueryRange_nid_postfix(begin, length, &info);
    Require(info.writtenPages == 3 && info.headWritten && info.tailWritten, "edge writes are not flagged");
    // A narrower query only sees its own pages.
    GuestWriteWatchQueryRange_nid_postfix(region.Address(3), page, &info);
    Require(info.watchedPages == 1 && info.writtenPages == 1, "narrow query leaks neighbours");
    // Unregister makes it uncovered again.
    GuestWriteWatchUnregister_nid_postfix(reinterpret_cast<void*>(begin), length);
    Require(!GuestWriteWatchQueryRange_nid_postfix(begin, length, &info) && info.watchedPages == 0, "unregistered range stays covered");
}

void CheckEdge() {
    Region region(12);
    const auto page = region.page;
    Require(page == 16384 || page == 4096, "unexpected host page size");
    // Registration begins and ends in the middle of a host page: bytes of the neighbouring data share both edge pages.
    const auto begin = region.Address(1) + page / 4;
    const auto end = region.Address(6) + page / 2;
    GuestWriteWatchRegister_nid_postfix(reinterpret_cast<void*>(begin), end - begin);
    Require(CollectRange(region, begin, end - begin) == std::set<std::size_t>({1, 2, 3, 4, 5, 6}), "unaligned registration did not widen to host pages");
    // A store to the head page BELOW the registered bytes (another allocation's data) is indistinguishable: the page is flagged.
    region.base[1 * page + 8] = 1;
    Require(CollectRange(region, begin, end - begin) == std::set<std::size_t>({1}), "a store below the tracked bytes on the head page is not flagged");
    region.base[6 * page + page - 8] = 1;  // above the tracked bytes on the tail page
    Require(CollectRange(region, begin, end - begin) == std::set<std::size_t>({6}), "a store above the tracked bytes on the tail page is not flagged");
    // 4 KiB guest pages inside one 16 KiB host page: stores to each 4 KiB quarter flag the same single host page, once.
    for (std::size_t quarter = 0; quarter * 4096 < page; ++quarter) {
        region.base[3 * page + quarter * 4096 + 7] = static_cast<std::uint8_t>(quarter + 1);
        const auto got = CollectRange(region, begin, end - begin);
        Require(got == std::set<std::size_t>({3}), "a 4 KiB quarter store does not flag exactly its host page");
    }
    // A single store reports a page-granular run: one run of exactly one host page (no 4 KiB runs).
    struct Runs {
        std::vector<std::pair<std::uintptr_t, std::uintptr_t>> runs;
    } runs;
    region.base[4 * page + page - 1] = 9;
    GuestWriteWatchCollect_nid_postfix(begin, end - begin, [](void* context, std::uintptr_t first, std::uintptr_t last) { static_cast<Runs*>(context)->runs.emplace_back(first, last); }, &runs);
    Require(runs.runs.size() == 1 && runs.runs[0].first == region.Address(4) && runs.runs[0].second == region.Address(5), "a store is not reported as one host page run");
    // Collect over a sub-range reports only pages that overlap it, even by one byte, and leaves the rest flagged.
    region.base[2 * page] = 1;
    region.base[5 * page + page - 1] = 1;
    Require(CollectRange(region, region.Address(2) + page - 1, 2) == std::set<std::size_t>({2}), "a two byte collect across a page boundary reports the wrong pages");
    Require(CollectRange(region, begin, end - begin) == std::set<std::size_t>({5}), "a sub-range collect disturbed other pages");
    // A range that begins in an unwatched page is not complete: nothing is collected.
    region.base[3 * page] = 1;
    bool ok = true;
    const auto partial = CollectRange(region, region.Address(0), 4 * page, &ok);
    Require(!ok && partial.empty(), "collect over a partly unwatched range reports or succeeds");
    Require(CollectRange(region, begin, end - begin) == std::set<std::size_t>({3}), "the failed collect consumed a flag");
    // Exact end boundary: [begin, end) is half open, so a range ending on a page boundary excludes the next page.
    region.base[7 * page] = 1;  // outside the registration
    Require(CollectRange(region, region.Address(1), page).empty(), "an unwritten page reports");
    // Re-registering a subrange of an already watched range keeps the watch and reads as written again.
    GuestWriteWatchRegister_nid_postfix(region.base + 8 * page, 2 * page);
    Require(CollectRange(region, region.Address(8), 2 * page) == std::set<std::size_t>({8, 9}), "a new registration beside an old one is not fully written");
    GuestWriteWatchRegister_nid_postfix(region.base + 8 * page, page);
    Require(CollectRange(region, region.Address(8), 2 * page) == std::set<std::size_t>({8}), "re-registering one page does not read as written exactly there");
}

// ---------------------------------------------------------------------------------------------------------------------------
// Single threaded fuzz against a reference shadow.
struct Shadow {
    std::vector<bool> watched;
    std::vector<bool> written;
    std::vector<int> hostOpen;  // open BeginHostWrite windows per page
};

struct HostWindow {
    std::size_t first, last;
};

void RunFuzz(bool exact, std::uint64_t seed, int steps) {
    Region region(96);
    const auto page = region.page;
    Rng rng(seed);
    Shadow shadow;
    shadow.watched.assign(region.Pages(), false);
    shadow.written.assign(region.Pages(), false);
    shadow.hostOpen.assign(region.Pages(), 0);
    std::vector<HostWindow> windows;
    std::size_t extraReports = 0;
    std::size_t reports = 0;

    const auto pageRange = [&](std::size_t& first, std::size_t& last) {
        first = rng.Below(region.Pages());
        last = first + 1 + rng.Below(std::min<std::size_t>(24, region.Pages() - first));
    };

    for (int step = 0; step < steps; ++step) {
        const auto op = rng.Below(100);
        std::size_t first = 0;
        std::size_t last = 0;
        pageRange(first, last);
        // Byte level jitter: registrations and collects start and stop anywhere inside their edge pages.
        const auto jitterBegin = rng.Below(2) != 0 ? rng.Below(page) : 0;
        const auto jitterEnd = rng.Below(2) != 0 ? rng.Below(page - 1) + 1 : 0;
        const auto beginByte = first * page + jitterBegin;
        const auto endByte = (last - 1) * page + (jitterEnd == 0 ? page : jitterEnd);
        const std::size_t firstPage = beginByte / page;
        const std::size_t lastPage = (endByte + page - 1) / page;
        if (endByte <= beginByte) continue;
        if (op < 10) {  // register
            GuestWriteWatchRegister_nid_postfix(region.base + beginByte, endByte - beginByte);
            for (auto p = firstPage; p < lastPage; ++p) {
                shadow.watched[p] = true;
                shadow.written[p] = true;
            }
        } else if (op < 16) {  // unregister
            // The host windows of unregistered pages are dropped by the watch: end them in the shadow too.
            GuestWriteWatchUnregister_nid_postfix(region.base + beginByte, endByte - beginByte);
            for (auto p = firstPage; p < lastPage; ++p) {
                shadow.watched[p] = false;
                shadow.written[p] = false;
            }
        } else if (op < 55) {  // store (only to a watched page: an unwatched one is plain memory, also exercised)
            const auto p = firstPage;
            const auto offset = rng.Below(page);
            region.base[p * page + offset] = static_cast<std::uint8_t>(step);
            if (shadow.watched[p]) shadow.written[p] = true;
        } else if (op < 80) {  // collect
            bool ok = false;
            const auto got = CollectRange(region, region.Address(0) + beginByte, endByte - beginByte, &ok);
            bool covered = true;
            for (auto p = firstPage; p < lastPage; ++p) covered = covered && shadow.watched[p];
            Require(ok == covered, "collect completeness differs from the shadow");
            if (!ok) {
                Require(got.empty(), "an incomplete collect reported pages");
                continue;
            }
            std::set<std::size_t> expected;
            for (auto p = firstPage; p < lastPage; ++p)
                if (shadow.written[p]) expected.insert(p);
            for (const auto p : expected) Require(got.count(p) != 0, ("a written page was not reported: " + Fmt("page %llu step %llu", p, step)).c_str());
            for (const auto p : got) {
                Require(p >= firstPage && p < lastPage, "a page outside the collected range was reported");
                if (exact) Require(expected.count(p) != 0, ("a clean page was reported: " + Fmt("page %llu step %llu", p, step)).c_str());
                else if (expected.count(p) == 0) ++extraReports;
            }
            reports += got.size();
            for (auto p = firstPage; p < lastPage; ++p) {
                if (shadow.hostOpen[p] == 0) shadow.written[p] = false;
            }
            // pages reported beyond the shadow (fault-ahead) were flagged by the watch, which has consumed them: the shadow follows
            if (!exact)
                for (const auto p : got)
                    if (shadow.hostOpen[p] == 0) shadow.written[p] = false;
        } else if (op < 87) {  // host write window begin
            if (windows.size() < 6) {
                bool all = true;
                for (auto p = firstPage; p < lastPage; ++p) all = all && shadow.watched[p];
                if (all) {
                    GuestWriteWatchBeginHostWrite_nid_postfix(region.base + beginByte, endByte - beginByte);
                    windows.push_back({firstPage, lastPage});
                    // The window covers whole host pages (the watch widens it) and marks them written.
                    for (auto p = firstPage; p < lastPage; ++p) {
                        ++shadow.hostOpen[p];
                        shadow.written[p] = true;
                    }
                }
            }
        } else if (op < 93) {  // host write window end
            if (!windows.empty()) {
                const auto at = rng.Below(windows.size());
                const auto window = windows[at];
                windows.erase(windows.begin() + static_cast<std::ptrdiff_t>(at));
                GuestWriteWatchEndHostWrite_nid_postfix(region.base + window.first * page, (window.last - window.first) * page);
                for (auto p = window.first; p < window.last; ++p) --shadow.hostOpen[p];
            }
        } else {  // QueryRange against the shadow
            GuestWriteWatchRangeInfo info;
            const bool covered = GuestWriteWatchQueryRange_nid_postfix(region.Address(0) + beginByte, endByte - beginByte, &info);
            bool want = true;
            std::size_t watchedPages = 0;
            std::size_t writtenPages = 0;
            for (auto p = firstPage; p < lastPage; ++p) {
                want = want && shadow.watched[p];
                watchedPages += shadow.watched[p] ? 1 : 0;
                writtenPages += shadow.watched[p] && shadow.written[p] ? 1 : 0;
            }
            Require(covered == want && info.covered == want, "QueryRange coverage differs from the shadow");
            Require(info.watchedPages == watchedPages, "QueryRange watched page count differs from the shadow");
            if (exact) Require(info.writtenPages == writtenPages, "QueryRange written page count differs from the shadow");
            else Require(info.writtenPages >= writtenPages, "QueryRange misses written pages");
            Require(info.headShared == (beginByte % page != 0) && info.tailShared == (endByte % page != 0), "QueryRange shared edge flags are wrong");
            // Querying is free of side effects: the next collect still sees everything (checked by later steps).
        }
    }
    // Close everything and drain: a final full collect must equal the shadow.
    for (const auto& window : windows) GuestWriteWatchEndHostWrite_nid_postfix(region.base + window.first * page, (window.last - window.first) * page);
    for (const auto& window : windows)
        for (auto p = window.first; p < window.last; ++p) --shadow.hostOpen[p];
    std::fprintf(stderr, "[fuzz seed %llu exact %d] %d steps, %zu pages reported, %zu beyond the shadow (fault-ahead)\n", static_cast<unsigned long long>(seed), exact ? 1 : 0, steps, reports, extraReports);
}

void CheckFuzz(bool exact) {
    // exact mode needs APS5_WRITE_WATCH_FAULT_AHEAD=0 in the environment (read when the watch is constructed at load; ctest sets it)
    if (exact) Require(std::getenv("APS5_WRITE_WATCH_FAULT_AHEAD") != nullptr && std::strcmp(std::getenv("APS5_WRITE_WATCH_FAULT_AHEAD"), "0") == 0, "fuzz-exact needs APS5_WRITE_WATCH_FAULT_AHEAD=0");
    const auto base = Seed(0x5eed);
    for (std::uint64_t round = 0; round < 6; ++round) RunFuzz(exact, base + round * 7919, 4000);
}

// ---------------------------------------------------------------------------------------------------------------------------
// Races. Writers store a per-page sequence number; a collector keeps a copy per page and requires every changed page to be
// reported by the next collect.
void CheckRaceCollect() {
    constexpr std::size_t Pages = 64;
    constexpr int Writers = 4;
    Region region(Pages);
    Region hostRegion(16);
    Region churnRegion(32);
    const auto page = region.page;
    GuestWriteWatchRegister_nid_postfix(region.base, region.bytes);
    GuestWriteWatchRegister_nid_postfix(hostRegion.base, hostRegion.bytes);
    // First collect: everything reads as written (new registration).
    CollectRange(region, region.Address(0), region.bytes);
    CollectRange(hostRegion, hostRegion.Address(0), hostRegion.bytes);

    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> stores{0};
    std::vector<std::thread> threads;
    // Each writer owns every Writers-th page and stores an increasing counter at a varying offset (the page word at offset 0 is
    // the sequence the collector compares).
    for (int w = 0; w < Writers; ++w) {
        threads.emplace_back([&, w] {
            Rng rng(static_cast<std::uint64_t>(w) + 99);
            std::uint64_t sequence = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                const auto p = static_cast<std::size_t>(w) + Writers * rng.Below(Pages / Writers);
                ++sequence;
                std::uint64_t value = (static_cast<std::uint64_t>(w) << 56) | sequence;
                // a few unrelated bytes first (same page), then the sequence word: either store must flag the page
                region.base[p * page + 4096 + rng.Below(1024)] = static_cast<std::uint8_t>(sequence);
                std::memcpy(region.base + p * page, &value, sizeof(value));
                stores.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    // GPU completion callback stand-in: host writes over a separate region, partial windows, plus collects of it.
    threads.emplace_back([&] {
        Rng rng(777);
        while (!stop.load(std::memory_order_relaxed)) {
            const auto first = rng.Below(hostRegion.Pages() - 2);
            const auto count = 1 + rng.Below(2);
            GuestWriteWatchBeginHostWrite_nid_postfix(hostRegion.base + first * page, count * page);
            std::memset(hostRegion.base + first * page, 0x5a, count * page);
            GuestWriteWatchEndHostWrite_nid_postfix(hostRegion.base + first * page, count * page);
            // the pages have to read as written after the host write
            if (rng.Below(4) == 0) CollectRange(hostRegion, hostRegion.Address(0), hostRegion.bytes);
        }
    });
    // Layout churn on a third region: register / unregister / protection changes while the others fault.
    threads.emplace_back([&] {
        Rng rng(4242);
        while (!stop.load(std::memory_order_relaxed)) {
            GuestWriteWatchRegister_nid_postfix(churnRegion.base, churnRegion.bytes);
            churnRegion.base[rng.Below(churnRegion.bytes)] = 1;
            CollectRange(churnRegion, churnRegion.Address(0), churnRegion.bytes);
            GuestWriteWatchUnregister_nid_postfix(churnRegion.base, churnRegion.bytes);
        }
    });
    // The collector.
    std::vector<std::uint64_t> copy(Pages, 0);
    std::uint64_t rounds = 0;
    std::uint64_t reported = 0;
    std::string failure;
    const auto start = std::chrono::steady_clock::now();
    while (Seconds(start) < 3.0 && failure.empty()) {
        std::vector<std::uint64_t> before(Pages);
        for (std::size_t p = 0; p < Pages; ++p) std::memcpy(&before[p], region.base + p * page, sizeof(std::uint64_t));
        const auto got = CollectRange(region, region.Address(0), region.bytes);
        for (std::size_t p = 0; p < Pages; ++p) {
            if (before[p] != copy[p] && got.count(p) == 0) failure = "lost write: page " + std::to_string(p) + " changed before the collect and was not reported (round " + std::to_string(rounds) + ")";
        }
        // the consumer reads the bytes after the collect: copies of reported pages
        for (std::size_t p = 0; p < Pages; ++p) {
            if (got.count(p) != 0 || before[p] != copy[p]) std::memcpy(&copy[p], region.base + p * page, sizeof(std::uint64_t));
        }
        reported += got.size();
        ++rounds;
        if (rounds % 8 == 0) std::this_thread::yield();
    }
    stop = true;
    for (auto& thread : threads) thread.join();
    Require(failure.empty(), failure.c_str());
    // Quiescent drain: after the writers stop, one collect settles every page, then nothing is reported.
    CollectRange(region, region.Address(0), region.bytes);
    Require(CollectRange(region, region.Address(0), region.bytes).empty(), "a quiescent range still reports writes");
    for (std::size_t p = 0; p < Pages; ++p) {
        std::uint64_t now = 0;
        std::memcpy(&now, region.base + p * page, sizeof(now));
    }
    std::fprintf(stderr, "[race-collect] %llu stores, %llu collect rounds, %llu pages reported\n", static_cast<unsigned long long>(stores.load()), static_cast<unsigned long long>(rounds), static_cast<unsigned long long>(reported));
    Require(stores.load() > 1000 && rounds > 50, "the race test did not run long enough to mean anything");
}

void CheckRaceQuery() {
    constexpr std::size_t Pages = 48;
    Region region(Pages);
    Region churnRegion(16);
    const auto page = region.page;
    GuestWriteWatchRegister_nid_postfix(region.base, region.bytes);
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> queries{0};
    std::atomic<bool> bad{false};
    std::vector<std::thread> threads;
    for (int w = 0; w < 3; ++w) {
        threads.emplace_back([&, w] {
            Rng rng(static_cast<std::uint64_t>(w) + 5);
            while (!stop.load(std::memory_order_relaxed)) region.base[rng.Below(region.bytes)] = static_cast<std::uint8_t>(rng.Next());
        });
    }
    for (int q = 0; q < 2; ++q) {
        threads.emplace_back([&, q] {
            Rng rng(static_cast<std::uint64_t>(q) + 31);
            while (!stop.load(std::memory_order_relaxed)) {
                const auto first = rng.Below(Pages - 1);
                const auto count = 1 + rng.Below(Pages - first - 1);
                GuestWriteWatchRangeInfo info;
                const bool covered = GuestWriteWatchQueryRange_nid_postfix(region.Address(first) + 3, count * page, &info);
                // the region is registered for the whole run: always covered, written pages never exceed watched pages
                if (!covered || info.watchedPages != count + 1 || info.writtenPages > info.watchedPages || !info.headShared || !info.tailShared) bad = true;
                if (!GuestWriteWatchCovers_nid_postfix(region.Address(first), count * page)) bad = true;
                queries.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    threads.emplace_back([&] {
        Rng rng(1);
        while (!stop.load(std::memory_order_relaxed)) {
            CollectRange(region, region.Address(0), region.bytes);
            GuestWriteWatchRegister_nid_postfix(churnRegion.base, churnRegion.bytes);
            GuestWriteWatchUnregister_nid_postfix(churnRegion.base, churnRegion.bytes);
        }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    stop = true;
    for (auto& thread : threads) thread.join();
    Require(!bad.load(), "a concurrent QueryRange answer was inconsistent");
    Require(queries.load() > 1000, "too few concurrent queries ran");
}

// ---------------------------------------------------------------------------------------------------------------------------
void CheckBench() {
    constexpr std::size_t Pages = 2048;  // 32 MiB of 16 KiB pages
    Region region(Pages);
    const auto page = region.page;
    GuestWriteWatchRegister_nid_postfix(region.base, region.bytes);
    CollectRange(region, region.Address(0), region.bytes);
    // Fault cost: first write to each of N armed pages, spread out (stride 3 defeats fault-ahead), then re-arm by collecting.
    {
        constexpr int Rounds = 20;
        std::size_t faults = 0;
        double total = 0;
        for (int round = 0; round < Rounds; ++round) {
            const auto start = std::chrono::steady_clock::now();
            for (std::size_t p = static_cast<std::size_t>(round % 3); p < Pages; p += 3) {
                region.base[p * page] = 1;
                ++faults;
            }
            total += Seconds(start);
            CollectRange(region, region.Address(0), region.bytes);
        }
        std::fprintf(stderr, "[bench] first-write fault (armed page, stride 3): %.0f ns per fault (%zu faults)\n", total / static_cast<double>(faults) * 1e9, faults);
    }
    // Sequential faults with fault-ahead (default).
    {
        std::size_t faults = 0;
        double total = 0;
        for (int round = 0; round < 10; ++round) {
            const auto start = std::chrono::steady_clock::now();
            for (std::size_t p = 0; p < Pages; ++p) region.base[p * page] = 2;
            total += Seconds(start);
            faults += Pages;
            CollectRange(region, region.Address(0), region.bytes);
        }
        std::fprintf(stderr, "[bench] sequential first-writes: %.0f ns per page (fault-ahead)\n", total / static_cast<double>(faults) * 1e9);
    }
    // Collect cost: dense (all written) and sparse (1 in 64) and clean.
    {
        const auto measure = [&](const char* label, std::size_t stride) {
            constexpr int Rounds = 40;
            double total = 0;
            std::size_t reported = 0;
            for (int round = 0; round < Rounds; ++round) {
                if (stride != 0)
                    for (std::size_t p = 0; p < Pages; p += stride) region.base[p * page] = 3;
                const auto start = std::chrono::steady_clock::now();
                reported += CollectRange(region, region.Address(0), region.bytes).size();
                total += Seconds(start);
            }
            std::fprintf(stderr, "[bench] collect %s: %.1f us per call over %zu pages (%zu pages reported total)\n", label, total / Rounds * 1e6, Pages, reported);
        };
        measure("clean", 0);
        measure("sparse 1/64", 64);
        measure("dense", 1);
    }
    // Fault scaling: the fault handler takes the one watch lock, so concurrent first-writes serialize. faults/s with 1 and 4 writers.
    for (const int threads : {1, 4}) {
        constexpr int Rounds = 12;
        std::atomic<std::uint64_t> total{0};
        double elapsed = 0;
        for (int round = 0; round < Rounds; ++round) {
            std::vector<std::thread> workers;
            const auto start = std::chrono::steady_clock::now();
            for (int t = 0; t < threads; ++t) {
                workers.emplace_back([&, t] {
                    std::uint64_t mine = 0;
                    for (std::size_t p = static_cast<std::size_t>(t) * 3; p < Pages; p += 3 * static_cast<std::size_t>(threads)) {
                        region.base[p * page] = 1;
                        ++mine;
                    }
                    total.fetch_add(mine, std::memory_order_relaxed);
                });
            }
            for (auto& worker : workers) worker.join();
            elapsed += Seconds(start);
            CollectRange(region, region.Address(0), region.bytes);
        }
        std::fprintf(stderr, "[bench] concurrent first-write faults, %d writer thread(s): %.0f faults/s (%llu faults)\n", threads, static_cast<double>(total.load()) / elapsed, static_cast<unsigned long long>(total.load()));
    }
    // Query and covers.
    {
        GuestWriteWatchRangeInfo info;
        constexpr int Calls = 200000;
        auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < Calls; ++i) GuestWriteWatchQueryRange_nid_postfix(region.Address(i % 1000) + 5, 8 * page, &info);
        std::fprintf(stderr, "[bench] QueryRange (8 pages): %.0f ns per call\n", Seconds(start) / Calls * 1e9);
        start = std::chrono::steady_clock::now();
        std::size_t hits = 0;
        for (int i = 0; i < Calls; ++i) hits += GuestWriteWatchCovers_nid_postfix(region.Address(i % 1000), 8 * page) ? 1 : 0;
        std::fprintf(stderr, "[bench] Covers (8 pages): %.0f ns per call (%zu hits)\n", Seconds(start) / Calls * 1e9, hits);
    }
    // Register / unregister of a 1 MiB range.
    {
        Region small(64);
        constexpr int Calls = 2000;
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < Calls; ++i) {
            GuestWriteWatchRegister_nid_postfix(small.base, small.bytes);
            GuestWriteWatchUnregister_nid_postfix(small.base, small.bytes);
        }
        std::fprintf(stderr, "[bench] register + unregister (%zu pages): %.1f us per pair\n", small.Pages(), Seconds(start) / Calls * 1e6);
    }
}

}  // namespace

int main(int argc, char** argv) {
    try {
        Require(argc == 2, "expected query, edge, fuzz, fuzz-exact, race-collect, race-query or bench");
        const std::string_view mode(argv[1]);
        if (mode == "query") CheckQuery();
        else if (mode == "edge") CheckEdge();
        else if (mode == "fuzz") CheckFuzz(false);
        else if (mode == "fuzz-exact") CheckFuzz(true);
        else if (mode == "race-collect") CheckRaceCollect();
        else if (mode == "race-query") CheckRaceQuery();
        else if (mode == "bench") CheckBench();
        else throw std::runtime_error("unknown write watch stress test");
        std::printf("write watch stress %s passed\n", argv[1]);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
