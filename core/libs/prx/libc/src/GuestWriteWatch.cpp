#include "prx/libc/include/GuestWriteWatch.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <type_traits>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/fs.h>
#include <linux/userfaultfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

#if defined(__APPLE__)
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach/semaphore.h>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/ucontext.h>
#include <unistd.h>
#endif

#if (defined(__linux__) && defined(PAGEMAP_SCAN) && defined(UFFD_FEATURE_WP_ASYNC)) || defined(__APPLE__)
#define APS5_GUEST_WRITE_WATCH 1
#endif

namespace GuestWriteWatch {
namespace {

#if defined(__linux__) && defined(PAGEMAP_SCAN) && defined(UFFD_FEATURE_WP_ASYNC)
constexpr std::uintptr_t PageBytes = 4096;

class Watch {
public:
    static Watch& Get() {
        static auto* watch = new Watch;
        return *watch;
    }

    bool Available() const {
        return _pagemap >= 0;
    }

    void Register(std::uintptr_t begin, std::uintptr_t end) {
        if (!Available() || end <= begin) return;
        std::unique_lock lock(_lock);
        remove(_ranges, begin, end);
        remove(_fresh, begin, end);
        cut(begin, end);
        uffdio_register registration{};
        registration.range.start = begin;
        registration.range.len = end - begin;
        registration.mode = UFFDIO_REGISTER_MODE_WP;
        if (ioctl(_uffd, UFFDIO_REGISTER, &registration) != 0) {
            static bool reported = false;
            if (!reported) {
                reported = true;
                std::fprintf(stderr, "[memory] write watch: cannot register 0x%llx+0x%llx (%s); the range stays unwatched\n", static_cast<unsigned long long>(begin), static_cast<unsigned long long>(end - begin), std::strerror(errno));
            }
            return;
        }
        insert(_ranges, begin, end);
        insert(_fresh, begin, end);
    }

    bool Unregister(std::uintptr_t begin, std::uintptr_t end) {
        if (!Available() || end <= begin) return false;
        std::unique_lock lock(_lock);
        const bool watched = remove(_ranges, begin, end);
        remove(_fresh, begin, end);
        cut(begin, end);
        return watched;
    }

    bool Alias(std::uintptr_t first, std::uintptr_t second, std::uintptr_t bytes) {
        if (!Available() || bytes == 0 || first == second || (first | second | bytes) % PageBytes != 0) return false;
        std::unique_lock lock(_lock);
        if (!covers(first, first + bytes) || !covers(second, second + bytes)) return false;
        link(first, first + bytes, second);
        link(second, second + bytes, first);
        return true;
    }

    bool Covers(std::uintptr_t begin, std::uintptr_t end) {
        if (!Available()) return false;
        std::shared_lock lock(_lock);
        return covers(begin, end);
    }

    bool Overlaps(std::uintptr_t begin, std::uintptr_t end) {
        if (!Available()) return false;
        std::shared_lock lock(_lock);
        const auto next = _ranges.upper_bound(begin);
        return (next != _ranges.begin() && std::prev(next)->second > begin) || (next != _ranges.end() && next->first < end);
    }

    bool Collect(std::uintptr_t begin, std::uintptr_t end, void (*written)(void*, std::uintptr_t, std::uintptr_t), void* context) {
        if (!Available()) return false;
        begin &= ~(PageBytes - 1);
        end = (end + PageBytes - 1) & ~(PageBytes - 1);
        if (end <= begin) return true;
        const auto plan = aliasPlan(begin, end);
        Fanout fanout{&plan.own, written, context};
        if (!takeFresh(begin, end, true, &Fanout::Report, &fanout) || !scanWritten(begin, end, &Fanout::Report, &fanout)) return false;
        for (const auto& alias : plan.aliases) {
            Fanout through{&alias.translations, written, context};
            if (!takeFresh(alias.begin, alias.end, false, &Fanout::Report, &through) || !scanWritten(alias.begin, alias.end, &Fanout::Report, &through)) return false;
        }
        return true;
    }

    bool CollectFresh(std::uintptr_t begin, std::uintptr_t end, void (*written)(void*, std::uintptr_t, std::uintptr_t), void* context) {
        if (!Available()) return false;
        begin &= ~(PageBytes - 1);
        end = (end + PageBytes - 1) & ~(PageBytes - 1);
        if (end <= begin) return true;
        const auto plan = aliasPlan(begin, end);
        if (plan.aliases.empty()) {
            std::shared_lock lock(_lock);
            auto it = _fresh.upper_bound(begin);
            if (it != _fresh.begin()) --it;
            while (it != _fresh.end() && it->first < end && it->second <= begin) ++it;
            if (it == _fresh.end() || it->first >= end) return true;
        }
        Fanout fanout{&plan.own, written, context};
        if (!takeFresh(begin, end, false, &Fanout::Report, &fanout)) return false;
        for (const auto& alias : plan.aliases) {
            Fanout through{&alias.translations, written, context};
            if (!takeFresh(alias.begin, alias.end, false, &Fanout::Report, &through)) return false;
        }
        return true;
    }

    bool CollectArmed(std::uintptr_t begin, std::uintptr_t end, void (*written)(void*, std::uintptr_t, std::uintptr_t), void* context) {
        if (!Available()) return false;
        begin &= ~(PageBytes - 1);
        end = (end + PageBytes - 1) & ~(PageBytes - 1);
        if (end <= begin) return true;
        std::vector<std::pair<std::uintptr_t, std::uintptr_t>> armed;
        AliasPlan plan;
        std::vector<std::vector<std::pair<std::uintptr_t, std::uintptr_t>>> aliasArmed;
        {
            std::shared_lock lock(_lock);
            if (!covers(begin, end)) return false;
            armed = armedParts(begin, end);
            plan = aliasPlanLocked(begin, end);
            for (const auto& alias : plan.aliases) aliasArmed.push_back(armedParts(alias.begin, alias.end));
        }
        Fanout fanout{&plan.own, written, context};
        for (const auto& [from, to] : armed) {
            if (!scanWritten(from, to, &Fanout::Report, &fanout)) return false;
        }
        for (std::size_t index = 0; index < plan.aliases.size(); ++index) {
            Fanout through{&plan.aliases[index].translations, written, context};
            for (const auto& [from, to] : aliasArmed[index]) {
                if (!scanWritten(from, to, &Fanout::Report, &through)) return false;
            }
        }
        return true;
    }

private:
    struct Translation {
        std::uintptr_t begin;
        std::uintptr_t end;
        std::uintptr_t partner;
    };

    struct AliasRange {
        std::uintptr_t begin;
        std::uintptr_t end;
        std::vector<Translation> translations;
    };

    struct AliasPlan {
        std::vector<Translation> own;
        std::vector<AliasRange> aliases;
    };

    struct Fanout {
        const std::vector<Translation>* translations;
        void (*written)(void*, std::uintptr_t, std::uintptr_t);
        void* context;

        static void Report(void* self, std::uintptr_t begin, std::uintptr_t end) {
            auto& fanout = *static_cast<Fanout*>(self);
            fanout.written(fanout.context, begin, end);
            for (const auto& [from, to, partner] : *fanout.translations) {
                const auto left = std::max(from, begin);
                const auto right = std::min(to, end);
                if (left < right) fanout.written(fanout.context, partner + (left - from), partner + (right - from));
            }
        }
    };

    struct Link {
        std::uintptr_t end;
        std::uintptr_t partner;
    };

    AliasPlan aliasPlan(std::uintptr_t begin, std::uintptr_t end) {
        std::shared_lock lock(_lock);
        return aliasPlanLocked(begin, end);
    }

    AliasPlan aliasPlanLocked(std::uintptr_t begin, std::uintptr_t end) const {
        AliasPlan plan;
        plan.own = translations(begin, end);
        for (const auto& [from, to, partner] : plan.own) {
            const auto stop = partner + (to - from);
            plan.aliases.push_back({partner, stop, translations(partner, stop)});
        }
        return plan;
    }

    std::vector<Translation> translations(std::uintptr_t begin, std::uintptr_t end) const {
        std::vector<Translation> found;
        if (_links.empty()) return found;
        for (auto it = _links.lower_bound(begin > _linkBytes ? begin - _linkBytes : 0); it != _links.end() && it->first < end; ++it) {
            const auto from = std::max(it->first, begin);
            const auto to = std::min(it->second.end, end);
            if (from < to) found.push_back({from, to, it->second.partner + (from - it->first)});
        }
        return found;
    }

    std::vector<std::pair<std::uintptr_t, std::uintptr_t>> armedParts(std::uintptr_t begin, std::uintptr_t end) const {
        std::vector<std::pair<std::uintptr_t, std::uintptr_t>> armed;
        auto cursor = begin;
        auto it = _fresh.upper_bound(begin);
        if (it != _fresh.begin()) --it;
        for (; it != _fresh.end() && it->first < end; ++it) {
            if (it->second <= cursor) continue;
            if (it->first > cursor) armed.emplace_back(cursor, it->first);
            cursor = std::max(cursor, it->second);
        }
        if (cursor < end) armed.emplace_back(cursor, end);
        return armed;
    }

    void link(std::uintptr_t begin, std::uintptr_t end, std::uintptr_t partner) {
        _links.emplace(begin, Link{end, partner});
        _linkBytes = std::max(_linkBytes, end - begin);
    }

    void cut(std::uintptr_t begin, std::uintptr_t end) {
        std::vector<std::pair<std::uintptr_t, Link>> kept;
        for (auto it = _links.begin(); it != _links.end();) {
            const auto from = it->first;
            const auto bytes = it->second.end - from;
            const auto partner = it->second.partner;
            if (!(from < end && begin < from + bytes) && !(partner < end && begin < partner + bytes)) {
                ++it;
                continue;
            }
            it = _links.erase(it);
            std::vector<std::pair<std::uintptr_t, std::uintptr_t>> pieces{{0, bytes}};
            for (const auto base : {from, partner}) {
                const auto low = std::min(bytes, begin > base ? begin - base : 0);
                const auto high = std::min(bytes, end > base ? end - base : 0);
                if (low >= high) continue;
                std::vector<std::pair<std::uintptr_t, std::uintptr_t>> remaining;
                for (const auto& [left, right] : pieces) {
                    if (left < low) remaining.emplace_back(left, std::min(right, low));
                    if (right > high) remaining.emplace_back(std::max(left, high), right);
                }
                pieces = std::move(remaining);
            }
            for (const auto& [left, right] : pieces) kept.emplace_back(from + left, Link{from + right, partner + left});
        }
        for (const auto& [from, piece] : kept) _links.emplace(from, piece);
    }

    bool takeFresh(std::uintptr_t begin, std::uintptr_t end, bool whole, void (*written)(void*, std::uintptr_t, std::uintptr_t), void* context) {
        std::vector<std::pair<std::uintptr_t, std::uintptr_t>> fresh;
        {
            std::unique_lock lock(_lock);
            if (whole && !covers(begin, end)) return false;
            auto it = _fresh.upper_bound(begin);
            if (it != _fresh.begin()) --it;
            for (; it != _fresh.end() && it->first < end; ++it) {
                const auto from = std::max(it->first, begin);
                const auto to = std::min(it->second, end);
                if (from < to) fresh.emplace_back(from, to);
            }
            for (const auto& [from, to] : fresh) remove(_fresh, from, to);
        }
        for (const auto& [from, to] : fresh) {
            written(context, from, to);
            if (protect(from, to)) continue;
            std::unique_lock lock(_lock);
            for (const auto& [left, right] : fresh) {
                if (!covers(left, right)) continue;
                remove(_fresh, left, right);
                insert(_fresh, left, right);
            }
            if (whole) written(context, begin, end);
            else for (const auto& [left, right] : fresh) written(context, left, right);
            return false;
        }
        return true;
    }

    bool scanWritten(std::uintptr_t begin, std::uintptr_t end, void (*written)(void*, std::uintptr_t, std::uintptr_t), void* context) const {
        std::array<page_region, 256> regions;
        auto cursor = begin;
        while (cursor < end) {
            pm_scan_arg scan{};
            scan.size = sizeof(scan);
            scan.flags = PM_SCAN_WP_MATCHING | PM_SCAN_CHECK_WPASYNC;
            scan.start = cursor;
            scan.end = end;
            scan.vec = reinterpret_cast<std::uintptr_t>(regions.data());
            scan.vec_len = regions.size();
            scan.category_mask = PAGE_IS_WRITTEN;
            scan.return_mask = PAGE_IS_WRITTEN;
            const auto count = ioctl(_pagemap, PAGEMAP_SCAN, &scan);
            if (count < 0) {
                written(context, cursor, end);
                return false;
            }
            for (long i = 0; i < count; ++i) written(context, regions[i].start, regions[i].end);
            if (scan.walk_end <= cursor || scan.walk_end >= end) break;
            cursor = scan.walk_end;
        }
        return true;
    }

private:
    Watch() {
        if (std::getenv("APS5_NO_WRITE_WATCH") == nullptr) open();
    }

    void open() {
        _uffd = static_cast<int>(syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK));
        if (_uffd < 0 && errno == EPERM) _uffd = static_cast<int>(syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY));
        if (_uffd < 0) return unavailable("userfaultfd");
        uffdio_api api{};
        api.api = UFFD_API;
        api.features = UFFD_FEATURE_WP_ASYNC | UFFD_FEATURE_WP_UNPOPULATED;
        if (ioctl(_uffd, UFFDIO_API, &api) != 0 || (api.features & UFFD_FEATURE_WP_ASYNC) == 0 || (api.features & UFFD_FEATURE_WP_UNPOPULATED) == 0) return unavailable("asynchronous userfaultfd write protection");
        const int pagemap = ::open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
        if (pagemap < 0) return unavailable("/proc/self/pagemap");
        if (!probe(pagemap)) {
            close(pagemap);
            return unavailable("PAGEMAP_SCAN");
        }
        _pagemap = pagemap;
    }

    void unavailable(const char* what) {
        const int error = errno;
        if (_uffd >= 0) close(_uffd);
        _uffd = -1;
        std::fprintf(stderr, "[memory] write watch unavailable: %s failed (%s); guest memory is compared instead\n", what, std::strerror(error));
    }

    bool protect(std::uintptr_t begin, std::uintptr_t end) const {
        uffdio_writeprotect protection{};
        protection.range.start = begin;
        protection.range.len = end - begin;
        protection.mode = UFFDIO_WRITEPROTECT_MODE_WP;
        return ioctl(_uffd, UFFDIO_WRITEPROTECT, &protection) == 0;
    }

    bool probe(int pagemap) {
        constexpr std::uintptr_t probeBytes = 4 * PageBytes;
        void* pages = mmap(nullptr, probeBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (pages == MAP_FAILED) return false;
        const auto address = reinterpret_cast<std::uintptr_t>(pages);
        auto* bytes = static_cast<volatile char*>(pages);
        bytes[0] = 1;
        uffdio_register registration{};
        registration.range.start = address;
        registration.range.len = probeBytes;
        registration.mode = UFFDIO_REGISTER_MODE_WP;
        bool working = ioctl(_uffd, UFFDIO_REGISTER, &registration) == 0 && protect(address, address + probeBytes);
        std::array<page_region, 4> regions{};
        const auto scan = [&]() -> long {
            pm_scan_arg arguments{};
            arguments.size = sizeof(arguments);
            arguments.flags = PM_SCAN_WP_MATCHING | PM_SCAN_CHECK_WPASYNC;
            arguments.start = address;
            arguments.end = address + probeBytes;
            arguments.vec = reinterpret_cast<std::uintptr_t>(regions.data());
            arguments.vec_len = regions.size();
            arguments.category_mask = PAGE_IS_WRITTEN;
            arguments.return_mask = PAGE_IS_WRITTEN;
            const auto count = ioctl(pagemap, PAGEMAP_SCAN, &arguments);
            if (count < 0) return -1;
            long pagesWritten = 0;
            for (long i = 0; i < count; ++i) pagesWritten += static_cast<long>((regions[i].end - regions[i].start) / PageBytes);
            return pagesWritten;
        };
        working = working && scan() == 0;
        if (working) {
            bytes[0] = 2;
            bytes[2 * PageBytes] = 1;
            working = scan() == 2 && scan() == 0;
        }
        munmap(pages, probeBytes);
        return working;
    }

    bool covers(std::uintptr_t begin, std::uintptr_t end) const {
        auto next = _ranges.upper_bound(begin);
        if (next == _ranges.begin()) return false;
        return std::prev(next)->second >= end;
    }

    static void insert(std::map<std::uintptr_t, std::uintptr_t>& ranges, std::uintptr_t begin, std::uintptr_t end) {
        auto next = ranges.upper_bound(begin);
        if (next != ranges.begin() && std::prev(next)->second == begin) {
            begin = std::prev(next)->first;
            ranges.erase(std::prev(next));
        }
        if (next != ranges.end() && next->first == end) {
            end = next->second;
            ranges.erase(next);
        }
        ranges.emplace(begin, end);
    }

    static bool remove(std::map<std::uintptr_t, std::uintptr_t>& ranges, std::uintptr_t begin, std::uintptr_t end) {
        bool removed = false;
        auto it = ranges.upper_bound(begin);
        if (it != ranges.begin()) --it;
        while (it != ranges.end() && it->first < end) {
            const auto rangeBegin = it->first;
            const auto rangeEnd = it->second;
            if (rangeEnd <= begin) {
                ++it;
                continue;
            }
            removed = true;
            it = ranges.erase(it);
            if (rangeBegin < begin) ranges.emplace(rangeBegin, begin);
            if (rangeEnd > end) ranges.emplace(end, rangeEnd);
        }
        return removed;
    }

    int _uffd = -1;
    int _pagemap = -1;
    std::shared_mutex _lock;
    std::map<std::uintptr_t, std::uintptr_t> _ranges;
    std::map<std::uintptr_t, std::uintptr_t> _fresh;
    std::multimap<std::uintptr_t, Link> _links;
    std::uintptr_t _linkBytes = 0;
};

const bool g_opened = (Watch::Get(), true);
#elif defined(__APPLE__)
class Watch {
public:
    static Watch& Get() {
        static auto* watch = new Watch;
        return *watch;
    }

    bool Available() const {
        return _available;
    }

    void Register(std::uintptr_t begin, std::uintptr_t end) {
        if (!_available || end <= begin) return;
        begin = pageStart(begin);
        end = pageEnd(end);
        if (end <= begin) return;
        std::vector<std::uint8_t> pages((end - begin) / _page, Written | Fresh);
        std::vector<bool> queried(pages.size(), false);
        const Lock lock(_lock);
        layoutChanged(begin, end, false);
        for (auto cursor = begin; cursor < end;) {
            mach_vm_address_t address = cursor;
            mach_vm_size_t size = 0;
            vm_region_basic_info_data_64_t info{};
            mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
            mach_port_t object = MACH_PORT_NULL;
            if (mach_vm_region(mach_task_self(), &address, &size, VM_REGION_BASIC_INFO_64, reinterpret_cast<vm_region_info_t>(&info), &count, &object) != KERN_SUCCESS || address >= end) break;
            const auto first = std::max<std::uintptr_t>(cursor, address);
            const auto last = std::min<std::uintptr_t>(end, address + size);
            const std::uint8_t protection = ((info.protection & VM_PROT_READ) ? PROT_READ : 0) | ((info.protection & VM_PROT_WRITE) ? PROT_WRITE : 0) | ((info.protection & VM_PROT_EXECUTE) ? PROT_EXEC : 0);
            for (auto page = first; page < last; page += _page) {
                pages[(page - begin) / _page] = Written | Fresh | protection;
                queried[(page - begin) / _page] = true;
            }
            cursor = last;
        }
        forEachPage(begin, end, [&](std::uintptr_t page, std::uint8_t& state) {
            auto& entry = pages[(page - begin) / _page];
            const std::uint8_t recorded = state & ProtectionMask;
            const bool armed = (state & Protected) != 0 && (entry & ProtectionMask) == (recorded & ~PROT_WRITE);
            // A page the host does not map (a hole in the range) keeps protection 0, whatever the table recorded for it before:
            // the recorded protection is the one of a mapping that is gone, and the page would read as accessible.
            if (!queried[(page - begin) / _page]) return;
            if (armed || (state & ReadTrapped) != 0) entry = static_cast<std::uint8_t>(Written | Fresh | recorded);
            else state = static_cast<std::uint8_t>(state & ~Protected);
        });
        dropTraps(begin, end);
        erase(begin, end);
        _ranges.emplace(begin, Range{end, std::move(pages)});
    }

    bool Unregister(std::uintptr_t begin, std::uintptr_t end) {
        if (!_available || end <= begin) return false;
        begin = pageStart(begin);
        end = pageEnd(end);
        if (end <= begin) return false;
        const Lock lock(_lock);
        layoutChanged(begin, end);
        dropTraps(begin, end);
        return erase(begin, end);
    }

    bool Covers(std::uintptr_t begin, std::uintptr_t end) {
        if (!_available) return false;
        const Lock lock(_lock);
        return covers(begin, end);
    }

    bool Overlaps(std::uintptr_t begin, std::uintptr_t end) {
        if (!_available) return false;
        const Lock lock(_lock);
        const auto next = _ranges.upper_bound(begin);
        return (next != _ranges.begin() && std::prev(next)->second.end > begin) || (next != _ranges.end() && next->first < end);
    }

    // One answer under one lock acquisition (design note: Kyty's TrackImage asks "tracked, and are the edge pages shared" before it
    // trusts the write watch, KytyPS5 textureCache.cpp:295-317, GPL-2.0; re-implemented here). Reads only; nothing is collected.
    bool QueryRange(std::uintptr_t begin, std::uintptr_t end, GuestWriteWatchRangeInfo& info) {
        info = {};
        info.pageBytes = _page;
        if (!_available || end <= begin) return false;
        info.headShared = (begin % _page) != 0;
        info.tailShared = (end % _page) != 0;
        const auto first = pageStart(begin);
        const auto last = pageEnd(end);
        const Lock lock(_lock);
        info.covered = covers(first, last);
        forEachPage(first, last, [&](std::uintptr_t page, std::uint8_t& state) {
            ++info.watchedPages;
            if ((state & Written) != 0) {
                ++info.writtenPages;
                if (page == first) info.headWritten = true;
                if (page + _page == last) info.tailWritten = true;
            }
            if ((state & ReadTrapped) != 0) ++info.trappedPages;
        });
        return info.covered;
    }

    bool HandleFault(std::uintptr_t address) {
        return _available && record(address);
    }

    bool HandleAccess(std::uintptr_t address, bool write) {
        if (!_available) return false;
        if (resolveTrap(address, write)) return true;
        return write && record(address);
    }

    enum class Take { All, Armed, Fresh };

    bool Collect(std::uintptr_t begin, std::uintptr_t end, void (*written)(void*, std::uintptr_t, std::uintptr_t), void* context, Take take = Take::All) {
        if (!_available) return false;
        begin = pageStart(begin);
        end = pageEnd(end);
        if (end <= begin) return true;
        std::vector<std::pair<std::uintptr_t, std::uintptr_t>> runs;
        {
            const Lock lock(_lock);
            if (take != Take::Fresh && !covers(begin, end)) return false;
            // The table says which pages are armed (host read-only). A mapping replaced or reopened without telling the watch (munmap /
            // MAP_FIXED / mprotect) leaves such a page host-writable: its writes never faulted, so count it as written.
            std::uintptr_t regionBegin = 0;
            std::uintptr_t regionEnd = 0;
            bool regionWritable = false;
            const auto hostWritable = [&](std::uintptr_t page) {
                if (page < regionBegin || page >= regionEnd) {
                    int host = 0;
                    regionBegin = page;
                    regionEnd = page + _page;
                    if (!hostProtection(page, host, &regionBegin, &regionEnd)) return false;
                    regionWritable = (host & PROT_WRITE) != 0;
                }
                return regionWritable;
            };
            forEachPage(begin, end, [&](std::uintptr_t page, std::uint8_t& state) {
                if (_validate && (state & Protected) != 0 && (state & ReadTrapped) == 0 && hostWritable(page)) state = static_cast<std::uint8_t>((state & (ProtectionMask | Fresh | ReadTrapped)) | Written);
                if ((state & Written) == 0) return;
                const bool fresh = (state & Fresh) != 0;
                if ((take == Take::Armed && fresh) || (take == Take::Fresh && !fresh)) return;
                state &= static_cast<std::uint8_t>(~Fresh);
                if (!runs.empty() && runs.back().second == page) runs.back().second = page + _page;
                else runs.emplace_back(page, page + _page);
                for (const auto& write : _hostWrites)
                    if (page >= write.begin && page < write.end) return;
                state &= static_cast<std::uint8_t>(~Written);
                if ((state & PROT_WRITE) != 0) state |= Protected;
            });
            for (const auto& [first, last] : runs) protect(first, last);
        }
        for (const auto& [first, last] : runs) written(context, first, last);
        return true;
    }

    void ProtectionChanged(std::uintptr_t begin, std::uintptr_t end, int protection) {
        if (!_available || end <= begin) return;
        begin = pageStart(begin);
        end = pageEnd(end);
        if (end <= begin) return;
        withoutTraps(begin, end, false, [&] {
            layoutChanged(begin, end);
            forEachPage(begin, end, [&](std::uintptr_t, std::uint8_t& state) {
                state = static_cast<std::uint8_t>(Written | (state & Fresh) | (protection & ProtectionMask));
            });
        });
    }

    int Protect(std::uintptr_t begin, std::uintptr_t end, int protection) {
        if (!_available) return ::mprotect(reinterpret_cast<void*>(begin), end - begin, protection) == 0 ? 0 : errno;
        const auto first = pageStart(begin);
        const auto last = pageEnd(end);
        return withoutTraps(first, last, true, [&] {
            layoutChanged(first, last);
            if (::mprotect(reinterpret_cast<void*>(begin), end - begin, protection) != 0) return errno;
            if (first < last) {
                forEachPage(first, last, [&](std::uintptr_t, std::uint8_t& state) {
                    state = static_cast<std::uint8_t>(Written | (state & Fresh) | (protection & ProtectionMask));
                });
            }
            return 0;
        });
    }

    // Protect for a caller that holds a lock the read trap resolver needs (the guest's mprotect inside a mutation of the allocation
    // registry: the resolver stores GPU results under the driver's host imports mutex, and an import holds that mutex while it
    // asks the registry for its ranges). Nothing is resolved and nothing is waited for: only the watch lock is taken. A page whose
    // read trap is armed keeps the trap and stays closed; its logical protection changes, and the page takes the new protection
    // when the trap resolves. Such a page is not marked written (the bytes behind a trap are stale, see BeginHostWriteKeepingTraps);
    // when the new protection lets the CPU write it, it is armed so the first store after the trap is recorded. Resolve the traps
    // before taking the lock (ResolveReadTraps) so that a kept trap is the exception.
    int ProtectKeepingTraps(std::uintptr_t begin, std::uintptr_t end, int protection) {
        if (!_available) return ::mprotect(reinterpret_cast<void*>(begin), end - begin, protection) == 0 ? 0 : errno;
        const auto first = pageStart(begin);
        const auto last = pageEnd(end);
        const Lock lock(_lock);
        layoutChanged(first, last);
        // The host protection of every page but the closed trapped ones.
        auto cursor = begin;
        int error = 0;
        const auto apply = [&](std::uintptr_t stop) {
            if (error == 0 && stop > cursor && ::mprotect(reinterpret_cast<void*>(cursor), stop - cursor, protection) != 0) error = errno;
        };
        forEachPage(first, last, [&](std::uintptr_t page, std::uint8_t& state) {
            if ((state & ReadTrapped) == 0 || trapKeptOpen(page)) return;
            apply(std::min(page, end));
            cursor = std::max(cursor, page + _page);
        });
        apply(end);
        if (error != 0) return error;
        forEachPage(first, last, [&](std::uintptr_t, std::uint8_t& state) {
            if ((state & ReadTrapped) == 0) {
                state = static_cast<std::uint8_t>(Written | (state & Fresh) | (protection & ProtectionMask));
                return;
            }
            const bool arm = (state & Written) == 0 && (protection & PROT_WRITE) != 0;
            state = static_cast<std::uint8_t>((state & (Fresh | ReadTrapped | Written)) | (protection & ProtectionMask) | (arm ? Protected : 0));
        });
        return 0;
    }

    // Resolves the read traps over the range on the calling thread, as Protect and BeginHostWrite do before they change the pages.
    // For a caller that is about to take a lock under which it cannot run the resolver (see ProtectKeepingTraps).
    void ResolveReadTraps(std::uintptr_t begin, std::uintptr_t end) {
        if (!_available || end <= begin || !_trapping.load(std::memory_order_acquire)) return;
        waitForResolving(begin, end);
        resolve(begin, end);
    }

    void BeginHostWrite(std::uintptr_t begin, std::uintptr_t end) {
        if (!_available || end <= begin) return;
        begin = pageStart(begin);
        end = pageEnd(end);
        if (end <= begin) return;
        withoutTraps(begin, end, true, [&] {
            _hostWrites.push_back({begin, end, 0, false, {}});
            forEachPage(begin, end, [&](std::uintptr_t page, std::uint8_t& state) {
                if ((state & Protected) != 0) ::mprotect(reinterpret_cast<void*>(page), _page, state & ProtectionMask);
                state = static_cast<std::uint8_t>((state & (ProtectionMask | Fresh)) | Written);
            });
        });
    }

    // The host write of a host import (the driver's ImportWatched): the pages have to be accessible while the import is made, but
    // nothing reads or keeps their bytes. BeginHostWrite resolves the read traps over the range first, and the resolver stores
    // GPU results into guest memory, which needs the lock the importing thread holds (the driver's host imports mutex): the
    // thread waited for itself, or for a resolver on another thread that waited for it. Here nothing is resolved and nothing
    // is waited for. A trapped page is only opened for the length of the write: its trap, its ReadTrapped bit and its write
    // state stay as they are, so it does not read as written afterwards (the bytes behind a trap are stale, and a page that
    // reads as written tells the driver the CPU stored over the pending image), and EndHostWrite closes it again unless the
    // trap was resolved or released meanwhile. While the page is open a CPU access to it does not fault and a store to it
    // is not recorded, as with a page the GPU-mutex bypass opened. Untrapped pages are opened and marked as in BeginHostWrite.
    //
    // A trapped page is opened with its full logical protection, write included, also when it is armed for write detection.
    // Opening it without write would make a CPU store fault into the trap, but the import needs every page of its range
    // writable while it is made: Metal builds a no-copy buffer over a range with one read-only page without an error, and
    // what the GPU then writes through that buffer never reaches the range (measured on macOS 26, M4 Pro: GPU -> CPU lost for the
    // whole buffer, CPU -> GPU kept; a PROT_NONE page fails the import outright). So a store inside the window cannot fault.
    // With APS5_IMPORT_WINDOW_STORES=1 it is found by its bytes instead: a trapped page that holds no write is hashed when
    // it is opened and again when the write ends, each time while it is readable only (a store at that moment faults and
    // waits for the watch lock), and a page whose bytes changed is marked written, as a store that faulted would have marked
    // it. A store of the bytes the page already held is not seen, and does not matter. Off by default: what the GPU writes
    // into such a page through a host import inside the window changes its bytes as well, and would read as a CPU store
    // over the pending image; whether that happens on a title is not known.
    //
    // The window is named by the token returned here, and only EndHostWriteKeepingTraps with that token ends it: a plain host
    // write over the same range (EndHostWrite matches its windows by range) cannot end it and close the pages under the import.
    std::uint64_t BeginHostWriteKeepingTraps(std::uintptr_t begin, std::uintptr_t end) {
        if (!_available || end <= begin) return 0;
        begin = pageStart(begin);
        end = pageEnd(end);
        if (end <= begin) return 0;
        const Lock lock(_lock);
        const auto token = ++_hostWriteTokens;
        std::vector<std::pair<std::uintptr_t, std::uint64_t>> opened;
        forEachPage(begin, end, [&](std::uintptr_t page, std::uint8_t& state) {
            if ((state & ReadTrapped) != 0) {
                if (_windowStores && (state & Written) == 0 && (state & PROT_READ) != 0) {
                    // (a page another trap-keeping write holds open stays writable for that import: hashed as it is)
                    std::uint64_t hash = 0;
                    if ((trapKeptOpen(page) || ::mprotect(reinterpret_cast<void*>(page), _page, PROT_READ) == 0) && hashPage(page, hash)) opened.emplace_back(page, hash);
                }
                ::mprotect(reinterpret_cast<void*>(page), _page, state & ProtectionMask);
                return;
            }
            if ((state & Protected) != 0) ::mprotect(reinterpret_cast<void*>(page), _page, state & ProtectionMask);
            state = static_cast<std::uint8_t>((state & (ProtectionMask | Fresh)) | Written);
        });
        _hostWrites.push_back({begin, end, token, true, std::move(opened)});
        return token;
    }

    void EndHostWrite(std::uintptr_t begin, std::uintptr_t end) {
        if (!_available || end <= begin) return;
        begin = pageStart(begin);
        end = pageEnd(end);
        if (end <= begin) return;
        const Lock lock(_lock);
        // Plain windows over one range are interchangeable; a trap-keeping window is never ended from here.
        const auto found = std::find_if(_hostWrites.begin(), _hostWrites.end(), [&](const HostWrite& write) { return !write.keepsTraps && write.begin == begin && write.end == end; });
        if (found != _hostWrites.end()) _hostWrites.erase(found);
    }

    void EndHostWriteKeepingTraps(std::uint64_t token) {
        if (!_available || token == 0) return;
        const Lock lock(_lock);
        const auto found = std::find_if(_hostWrites.begin(), _hostWrites.end(), [&](const HostWrite& write) { return write.keepsTraps && write.token == token; });
        if (found == _hostWrites.end()) return;
        const auto begin = found->begin;
        const auto end = found->end;
        const auto opened = std::move(found->opened);
        _hostWrites.erase(found);
        // The CPU stores that landed in the trapped pages while they were open (see BeginHostWriteKeepingTraps).
        for (const auto& [page, before] : opened) {
            auto* state = pageState(page);
            if (state == nullptr || (*state & (ReadTrapped | Written)) != ReadTrapped) continue;
            std::uint64_t after = 0;
            if (!trapKeptOpen(page) && ::mprotect(reinterpret_cast<void*>(page), _page, PROT_READ) != 0) continue;
            if (hashPage(page, after) && after != before) *state = static_cast<std::uint8_t>((*state & (ProtectionMask | Fresh | ReadTrapped)) | Written);
        }
        // Close the pages whose trap outlived the write (a resolved or released trap cleared ReadTrapped and left its pages open).
        std::uintptr_t runStart = 0;
        std::uintptr_t runEnd = 0;
        const auto close = [&] {
            if (runEnd > runStart) ::mprotect(reinterpret_cast<void*>(runStart), runEnd - runStart, PROT_NONE);
        };
        forEachPage(begin, end, [&](std::uintptr_t page, std::uint8_t& state) {
            if ((state & ReadTrapped) == 0 || trapKeptOpen(page)) return;
            if (page != runEnd) {
                close();
                runStart = page;
            }
            runEnd = page + _page;
        });
        close();
    }

    int Protection(std::uintptr_t address, std::uintptr_t limit, std::uintptr_t* runEnd) {
        const auto page = pageStart(address);
        const Lock lock(_lock);
        auto it = _ranges.upper_bound(page);
        if (_available && it != _ranges.begin() && page < std::prev(it)->second.end) {
            --it;
            const auto& pages = it->second.pages;
            constexpr std::uint8_t Key = ProtectionMask | ReadTrapped;
            const std::uint8_t key = pages[(page - it->first) / _page] & Key;
            auto end = page + _page;
            while (end < it->second.end && end < limit && (pages[(end - it->first) / _page] & Key) == key) end += _page;
            *runEnd = std::min(end, limit);
            return (key & ProtectionMask) | ((key & ReadTrapped) != 0 ? ProtectionTrapped : 0);
        }
        *runEnd = it != _ranges.end() ? std::min(it->first, limit) : limit;
        return -1;
    }

    // The host query runs under the watch lock: every protection change the watch makes (arming, releasing or resolving a read trap,
    // write protection) holds it, so the host mapping and the table describe one state. Queried apart, a trap released in between
    // left a page PROT_NONE to the host and untrapped to the table: an unreadable page that was readable all along.
    int Access(std::uintptr_t address, std::uintptr_t limit, std::uintptr_t* runEnd) {
        const auto page = pageStart(address);
#ifdef APS5_WRITE_WATCH_TEST_HOOKS
        if (const auto hook = _accessHook.load(std::memory_order_acquire)) hook(address);
#endif
        const Lock lock(_lock);
        mach_vm_address_t region = address;
        mach_vm_size_t regionSize = 0;
        vm_region_basic_info_data_64_t info{};
        mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t object = MACH_PORT_NULL;
        const auto status = mach_vm_region(mach_task_self(), &region, &regionSize, VM_REGION_BASIC_INFO_64, reinterpret_cast<vm_region_info_t>(&info), &count, &object);
        if (status == KERN_INVALID_ADDRESS) {
            *runEnd = limit;
            return ProtectionUnmapped;
        }
        if (status != KERN_SUCCESS || regionSize == 0) return -1;
        if (region > address) {
            *runEnd = std::min<std::uintptr_t>(limit, region);
            return ProtectionUnmapped;
        }
        auto end = std::min<std::uintptr_t>(limit, region + regionSize);
        bool readable = (info.protection & VM_PROT_READ) != 0;
        bool writable = readable && (info.protection & VM_PROT_WRITE) != 0;
        bool trapped = false;
        auto it = _ranges.upper_bound(page);
        if (_available && it != _ranges.begin() && page < std::prev(it)->second.end) {
            --it;
            const auto& pages = it->second.pages;
            constexpr std::uint8_t Key = ProtectionMask | ReadTrapped;
            const std::uint8_t key = pages[(page - it->first) / _page] & Key;
            auto keyEnd = page + _page;
            while (keyEnd < it->second.end && keyEnd < end && (pages[(keyEnd - it->first) / _page] & Key) == key) keyEnd += _page;
            end = std::min(end, keyEnd);
            trapped = (key & ReadTrapped) != 0;
            readable = (readable || trapped) && (key & PROT_READ) != 0;
            writable = readable && (key & PROT_WRITE) != 0;
        } else if (_available && it != _ranges.end()) {
            end = std::min(end, it->first);
        }
        *runEnd = end;
        return (readable ? PROT_READ : 0) | (writable ? PROT_WRITE : 0) | (trapped ? ProtectionTrapped : 0);
    }

    // The logical protection of the watched page at `address`, from the table alone. The watch owns the host protection of every
    // page it covers (write protection, read traps, and the guest's own mprotect, which comes through Protect), so the table is
    // what the guest sees there and no host query is needed. [*runBegin, *runEnd) is the run of pages around `address` with that
    // protection, kept inside the page's RunBlock so the scan under the lock stays short. -1 when the page is not watched.
    int Logical(std::uintptr_t address, std::uintptr_t* runBegin, std::uintptr_t* runEnd, std::uint64_t* generation) {
        if (!_available) return -1;
        const auto page = pageStart(address);
        const Lock lock(_lock);
        auto it = _ranges.upper_bound(page);
        if (it == _ranges.begin() || page >= std::prev(it)->second.end) return -1;
        --it;
        const auto& pages = it->second.pages;
        const std::uint8_t key = pages[(page - it->first) / _page] & ProtectionMask;
        const auto blockBegin = std::max(it->first, page & ~(RunBlock - 1));
        const auto blockEnd = std::min(it->second.end, (page & ~(RunBlock - 1)) + RunBlock);
        auto begin = page;
        while (begin > blockBegin && (pages[(begin - _page - it->first) / _page] & ProtectionMask) == key) begin -= _page;
        auto end = page + _page;
        while (end < blockEnd && (pages[(end - it->first) / _page] & ProtectionMask) == key) end += _page;
        *runBegin = begin;
        *runEnd = end;
        *generation = layoutSlot(page).load(std::memory_order_relaxed);
        const bool readable = (key & PROT_READ) != 0;
        return (readable ? PROT_READ : 0) | (readable && (key & PROT_WRITE) != 0 ? PROT_WRITE : 0);
    }

    std::uint64_t LayoutChanges() const {
        return _layoutChanges.load(std::memory_order_relaxed);
    }

    std::uint64_t LayoutGeneration(std::uintptr_t address) {
        return layoutSlot(address).load(std::memory_order_acquire);
    }

#ifdef APS5_WRITE_WATCH_TEST_HOOKS
    void SetAccessHook(void (*hook)(std::uintptr_t)) {
        _accessHook.store(hook, std::memory_order_release);
    }
#endif

    bool SetReadTrapResolver(void (*resolver)(std::uintptr_t, std::uintptr_t)) {
        if (!_available || resolver == nullptr) return false;
        std::call_once(_resolverStarted, [this] { startResolver(); });
        if (!_resolverRunning.load(std::memory_order_acquire)) return false;
        _resolver.store(resolver, std::memory_order_release);
        return true;
    }

    void SetReadTrapBypass(bool (*bypass)()) {
        _bypass.store(bypass, std::memory_order_release);
    }

    bool TrapReads(std::uintptr_t begin, std::uintptr_t end) {
        if (!_available || end <= begin || _resolver.load(std::memory_order_acquire) == nullptr) return false;
        const auto first = pageStart(begin);
        const auto last = pageEnd(end);
        const Lock lock(_lock);
        if (!covers(first, last)) return false;
        const auto [same, other] = _traps.equal_range(begin);
        const bool known = std::any_of(same, other, [&](const auto& trap) { return trap.second == end; });
        for (const auto& write : _hostWrites)
            if (write.begin < last && first < write.end) return false;
        if (known) {
            // A page of a known trap can have been opened behind the entry's back (the GPU-mutex bypass in resolveTrap clears ReadTrapped
            // and opens the page but keeps the entry). Reporting "trapped" for it would let a CPU read see stale bytes with no fault.
            std::vector<std::pair<std::uintptr_t, std::uintptr_t>> opened;
            forEachPage(first, last, [&](std::uintptr_t page, std::uint8_t& state) {
                if ((state & ReadTrapped) != 0) return;
                if (!opened.empty() && opened.back().second == page) opened.back().second = page + _page;
                else opened.emplace_back(page, page + _page);
            });
            for (const auto& [openBegin, openEnd] : opened) {
                if (::mprotect(reinterpret_cast<void*>(openBegin), openEnd - openBegin, PROT_NONE) != 0) return false;
                forEachPage(openBegin, openEnd, [](std::uintptr_t, std::uint8_t& state) { state |= ReadTrapped; });
            }
            return true;
        }
        if (::mprotect(reinterpret_cast<void*>(first), last - first, PROT_NONE) != 0) return false;
        _traps.emplace(begin, end);
        forEachPage(first, last, [](std::uintptr_t, std::uint8_t& state) { state |= ReadTrapped; });
        _trapping.store(true, std::memory_order_release);
        return true;
    }

    void ReadTraps(std::uintptr_t begin, std::uintptr_t end, void (*trapped)(void*, std::uintptr_t, std::uintptr_t), void* context) {
        if (!_available || end <= begin || !_trapping.load(std::memory_order_acquire)) return;
        std::vector<std::pair<std::uintptr_t, std::uintptr_t>> runs;
        {
            const Lock lock(_lock);
            forEachPage(pageStart(begin), pageEnd(end), [&](std::uintptr_t page, std::uint8_t& state) {
                if ((state & ReadTrapped) == 0) return;
                if (!runs.empty() && runs.back().second == page) runs.back().second = page + _page;
                else runs.emplace_back(page, page + _page);
            });
        }
        for (const auto& [runBegin, runEnd] : runs) trapped(context, runBegin, runEnd);
    }

    void ReleaseReads(std::uintptr_t begin, std::uintptr_t end) {
        if (!_available || end <= begin) return;
        const Lock lock(_lock);
        const auto [first, last] = _traps.equal_range(begin);
        const auto found = std::find_if(first, last, [&](const auto& trap) { return trap.second == end; });
        if (found == last) return;
        _traps.erase(found);
        untrap(begin, end);
    }

private:
    static constexpr std::uint8_t ProtectionMask = PROT_READ | PROT_WRITE | PROT_EXEC;
    static constexpr std::uint8_t Written = 0x10;
    static constexpr std::uint8_t Protected = 0x20;
    static constexpr std::uint8_t Fresh = 0x40;
    static constexpr std::uintptr_t FaultAheadBlock = 65536;
    static constexpr std::uint8_t ReadTrapped = 0x80;
    static constexpr std::uintptr_t RunBlock = 2u << 20;
    static constexpr std::size_t LayoutSlots = 4096;
    static constexpr std::size_t ResolveSlots = 32;
    static constexpr std::size_t ResolvingCapacity = 4096;
    static constexpr std::size_t RetrySlots = 64;
    static constexpr unsigned UntrappedRetries = 16;
    static constexpr unsigned GenerationRetries = RetrySlots * UntrappedRetries;

    enum SlotState : unsigned { Free, Claimed, Queued, Running, Done };
    enum class Owner { Any, Self, Others };

    struct Slot {
        std::atomic<unsigned> state{Free};
        std::uintptr_t page = 0;
        semaphore_t done = MACH_PORT_NULL;
    };

    struct Resolving {
        std::uintptr_t begin;
        std::uintptr_t end;
        pthread_t owner;
    };

    struct Retry {
        std::uintptr_t page = 0;
        pthread_t thread = nullptr;
        std::uint64_t generation = 0;
        std::uint64_t used = 0;
        unsigned count = 0;
    };

    struct Range {
        std::uintptr_t end;
        std::vector<std::uint8_t> pages;
    };

    // An open host write. `token` names a trap-keeping one (0 for a plain one, which is ended by its range); `opened` holds the
    // hash of each trapped page a trap-keeping one opened without a write on record.
    struct HostWrite {
        std::uintptr_t begin;
        std::uintptr_t end;
        std::uint64_t token;
        bool keepsTraps;
        std::vector<std::pair<std::uintptr_t, std::uint64_t>> opened;
    };

    struct Mutex {
        Mutex() {
            pthread_mutexattr_t attributes;
            pthread_mutexattr_init(&attributes);
            pthread_mutexattr_setpolicy_np(&attributes, PTHREAD_MUTEX_POLICY_FIRSTFIT_NP);
            pthread_mutex_init(&mutex, &attributes);
            pthread_mutexattr_destroy(&attributes);
            pthread_cond_init(&idle, nullptr);
        }
        pthread_mutex_t mutex;
        pthread_cond_t idle;
        std::atomic<unsigned> faulting{0};
        // The thread holding `mutex` (null when free). A fault taken by that thread while it holds the
        // lock (a store or load of a watched page inside the watch itself) must not lock again: the
        // mutex is not recursive, so that would hang the thread in its own signal handler. The fault
        // handler declines such a fault instead (rps5e memory/lock.rs), and it becomes a crash report.
        std::atomic<pthread_t> owner{nullptr};
        bool OwnedBySelf() const {
            const auto held = owner.load(std::memory_order_acquire);
            return held != nullptr && pthread_equal(held, pthread_self()) != 0;
        }
    };

    struct Lock {
        explicit Lock(Mutex& mutex) : _mutex(mutex) {
            pthread_mutex_lock(&_mutex.mutex);
            while (_mutex.faulting.load(std::memory_order_acquire) != 0) pthread_cond_wait(&_mutex.idle, &_mutex.mutex);
            _mutex.owner.store(pthread_self(), std::memory_order_release);
        }
        ~Lock() {
            _mutex.owner.store(nullptr, std::memory_order_release);
            pthread_mutex_unlock(&_mutex.mutex);
        }
        Mutex& _mutex;
    };

    struct FaultLock {
        explicit FaultLock(Mutex& mutex) : _mutex(mutex) {
            _mutex.faulting.fetch_add(1, std::memory_order_acq_rel);
            pthread_mutex_lock(&_mutex.mutex);
            _mutex.owner.store(pthread_self(), std::memory_order_release);
        }
        ~FaultLock() {
            _mutex.owner.store(nullptr, std::memory_order_release);
            if (_mutex.faulting.fetch_sub(1, std::memory_order_acq_rel) == 1) pthread_cond_broadcast(&_mutex.idle);
            pthread_mutex_unlock(&_mutex.mutex);
        }
        Mutex& _mutex;
    };

    // Under the watch lock, before the watched ranges or the logical protection of watched pages change inside [begin, end): an
    // answer of Logical that a caller kept for a page there is stale from here on. One generation per RunBlock (hashed), so the
    // driver unwatching and rewatching one buffer does not drop what callers kept about the rest of guest memory.
    void layoutChanged(std::uintptr_t begin, std::uintptr_t end, bool watchedOnly = true) {
        if (end <= begin) return;
        // Nothing is kept about pages the table does not hold, so a change to unwatched pages only (the driver unwatching a
        // buffer it has unwatched already, the guest's mprotect of unwatched memory) leaves every kept answer true.
        if (watchedOnly) {
            const auto next = _ranges.upper_bound(begin);
            if (!((next != _ranges.begin() && std::prev(next)->second.end > begin) || (next != _ranges.end() && next->first < end))) return;
        }
        _layoutChanges.fetch_add(1, std::memory_order_relaxed);
        const auto first = begin / RunBlock;
        const auto last = (end - 1) / RunBlock;
        if (last - first >= LayoutSlots) {
            for (auto& slot : _layout) slot.fetch_add(1, std::memory_order_release);
            return;
        }
        for (auto block = first; block <= last; ++block) _layout[block % LayoutSlots].fetch_add(1, std::memory_order_release);
    }

    std::atomic<std::uint64_t>& layoutSlot(std::uintptr_t address) {
        return _layout[(address / RunBlock) % LayoutSlots];
    }

    // A fault on the thread that holds the watch lock: declined (async-signal-safe note, once).
    bool declineReentrantFault() {
        if (!_lock.OwnedBySelf()) return false;
        static std::atomic<bool> noted{false};
        if (!noted.exchange(true, std::memory_order_relaxed)) {
            static const char message[] = "[write-watch] fault taken while this thread holds the watch lock: declined (not re-entered)\n";
            [[maybe_unused]] const auto written = ::write(2, message, sizeof(message) - 1);
        }
        return true;
    }

    Watch() {
        if (const char* ahead = std::getenv("APS5_WRITE_WATCH_FAULT_AHEAD")) _faultAhead = static_cast<std::size_t>(std::strtoul(ahead, nullptr, 0));
        if (std::getenv("APS5_NO_WRITE_WATCH") != nullptr) return;
        if (const char* validate = std::getenv("APS5_WRITE_WATCH_VALIDATE")) _validate = std::strcmp(validate, "0") != 0;
        if (const char* stores = std::getenv("APS5_IMPORT_WINDOW_STORES")) _windowStores = std::strcmp(stores, "0") != 0;
        struct sigaction action {};
        action.sa_sigaction = &Watch::fault;
        action.sa_flags = SA_SIGINFO | SA_RESTART;
        sigemptyset(&action.sa_mask);
        if (sigaction(SIGBUS, &action, &_previousBus) != 0 || sigaction(SIGSEGV, &action, &_previousSegv) != 0) {
            std::fprintf(stderr, "[memory] write watch: cannot install the fault handler (%s); guest buffers are fully re-read every use\n", std::strerror(errno));
            return;
        }
        _available = true;
    }

    static bool isWriteFault(const siginfo_t* info, void* context) {
        if (info->si_code <= 0 || info->si_code >= SI_USER || context == nullptr) return false;
        const auto state = static_cast<ucontext_t*>(context)->uc_mcontext;
        if (state == nullptr) return false;
#if defined(__x86_64__)
        constexpr unsigned PageFault = 14;
        constexpr unsigned WriteAccess = 1u << 1;
        return state->__es.__trapno == PageFault && (state->__es.__err & WriteAccess) != 0;
#elif defined(__aarch64__)
        constexpr unsigned DataAbort = 0x24;
        constexpr unsigned ExceptionClassShift = 26;
        constexpr unsigned WriteAccess = 1u << 6;
        return (state->__es.__esr >> ExceptionClassShift) == DataAbort && (state->__es.__esr & WriteAccess) != 0;
#else
        return false;
#endif
    }

    static void fault(int signal, siginfo_t* info, void* context) {
        auto& watch = Get();
        const bool write = isWriteFault(info, context);
        if (info->si_code > 0 && info->si_code < SI_USER && watch.resolveTrap(reinterpret_cast<std::uintptr_t>(info->si_addr), write)) return;
        if (write && watch.record(reinterpret_cast<std::uintptr_t>(info->si_addr))) return;
        const auto& previous = signal == SIGBUS ? watch._previousBus : watch._previousSegv;
        if ((previous.sa_flags & SA_SIGINFO) != 0 && previous.sa_sigaction != nullptr) {
            previous.sa_sigaction(signal, info, context);
            return;
        }
        if (previous.sa_handler != SIG_DFL && previous.sa_handler != SIG_IGN && previous.sa_handler != nullptr) {
            previous.sa_handler(signal);
            return;
        }
        ::signal(signal, SIG_DFL);
        if (previous.sa_handler == SIG_DFL) ::raise(signal);
    }

    bool record(std::uintptr_t address) {
        if (declineReentrantFault()) return false;
        const auto page = pageStart(address);
        const FaultLock lock(_lock);
        auto it = _ranges.upper_bound(page);
        if (it == _ranges.begin()) return false;
        --it;
        if (page >= it->second.end) return false;
        auto& pages = it->second.pages;
        const auto index = static_cast<std::size_t>((page - it->first) / _page);
        auto& state = pages[index];
        if ((state & Protected) != 0) {
            const auto protection = state & ProtectionMask;
            const auto writtenOpen = [&](std::size_t at) { return (pages[at] & (Written | Protected)) == Written && (pages[at] & ProtectionMask) == protection; };
            std::size_t count = 1;
            if (_faultAhead > 1 && index >= 2 && writtenOpen(index - 1) && writtenOpen(index - 2)) {
                const auto blockPages = static_cast<std::size_t>(((page | (FaultAheadBlock - 1)) + 1 - page) / _page);
                const auto limit = std::min({_faultAhead, blockPages, pages.size() - index});
                while (count < limit && (pages[index + count] & (Protected | ReadTrapped)) == Protected && (pages[index + count] & ProtectionMask) == protection) ++count;
            }
            if (::mprotect(reinterpret_cast<void*>(page), count * _page, protection) != 0) return false;
            for (std::size_t at = index; at < index + count; ++at) pages[at] = static_cast<std::uint8_t>((pages[at] & (ProtectionMask | Fresh)) | Written);
            return true;
        }
        if ((state & PROT_WRITE) == 0 || (state & ReadTrapped) != 0) return false;
        // The table says this page is open for writing and not armed. A fault means the host disagrees: either a racing thread just
        // reopened it (retry) or the mapping was changed behind the table's back (raw mprotect / remap). Retrying the latter forever is a
        // hang, so resync the table to the host and let the fault take its normal course.
        int host = 0;
        if (!hostProtection(page, host)) return false;
        if ((host & PROT_WRITE) != 0) {
            // (a page whose backing is gone, e.g. a truncated file, is "writable" and still faults: only a readable one is retried)
            std::uint8_t value = 0;
            mach_vm_size_t copied = 0;
            return mach_vm_read_overwrite(mach_task_self(), address, sizeof(value), reinterpret_cast<mach_vm_address_t>(&value), &copied) == KERN_SUCCESS && copied == sizeof(value);
        }
        // The logical protection of the page changes here: what callers kept about its block (Logical) is stale.
        layoutChanged(page, page + _page);
        state = static_cast<std::uint8_t>((state & Fresh) | Written | host);
        return false;
    }

    // The host protection of the page at `page` (PROT_NONE for an unmapped page); false when the query itself fails.
    static bool hostProtection(std::uintptr_t page, int& protection, std::uintptr_t* regionBegin = nullptr, std::uintptr_t* regionEnd = nullptr) {
        mach_vm_address_t address = page;
        mach_vm_size_t size = 0;
        vm_region_basic_info_data_64_t info{};
        mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t object = MACH_PORT_NULL;
        const auto status = mach_vm_region(mach_task_self(), &address, &size, VM_REGION_BASIC_INFO_64, reinterpret_cast<vm_region_info_t>(&info), &count, &object);
        if (status == KERN_INVALID_ADDRESS) {
            protection = 0;
            return true;
        }
        if (status != KERN_SUCCESS) return false;
        if (address > page) {
            protection = 0;
            if (regionEnd != nullptr) *regionEnd = address;
            return true;
        }
        protection = ((info.protection & VM_PROT_READ) ? PROT_READ : 0) | ((info.protection & VM_PROT_WRITE) ? PROT_WRITE : 0) | ((info.protection & VM_PROT_EXECUTE) ? PROT_EXEC : 0);
        if (regionBegin != nullptr) *regionBegin = address;
        if (regionEnd != nullptr) *regionEnd = address + size;
        return true;
    }

    // The bytes of the readable page at `page`, hashed from a copy the kernel makes (no fault if the page went away meanwhile).
    bool hashPage(std::uintptr_t page, std::uint64_t& hash) const {
        std::array<std::uint64_t, 16384 / sizeof(std::uint64_t)> words;
        if (_page > sizeof(words)) return false;
        mach_vm_size_t copied = 0;
        if (mach_vm_read_overwrite(mach_task_self(), page, _page, reinterpret_cast<mach_vm_address_t>(words.data()), &copied) != KERN_SUCCESS || copied != _page) return false;
        std::array<std::uint64_t, 4> lanes{0x9e3779b97f4a7c15ull, 0xbf58476d1ce4e5b9ull, 0x94d049bb133111ebull, 0xd6e8feb86659fd93ull};
        const auto count = _page / sizeof(std::uint64_t);
        for (std::size_t index = 0; index + 4 <= count; index += 4) {
            for (std::size_t lane = 0; lane < 4; ++lane) {
                lanes[lane] = (lanes[lane] ^ words[index + lane]) * 0xff51afd7ed558ccdull;
                lanes[lane] ^= lanes[lane] >> 29;
            }
        }
        hash = lanes[0] ^ (lanes[1] * 3) ^ (lanes[2] * 5) ^ (lanes[3] * 7);
        return true;
    }

    bool resolveTrap(std::uintptr_t address, bool write) {
        if (!_trapping.load(std::memory_order_acquire)) return false;
        if (declineReentrantFault()) return false;
        const auto page = pageStart(address);
        {
            const FaultLock lock(_lock);
            auto* state = pageState(page);
            if (state == nullptr) return false;
            if ((*state & ReadTrapped) == 0) return retryUntrapped(page, *state, write);
            const bool own = resolvingCovers(page, page + _page, Owner::Self) || pthread_equal(pthread_self(), _resolverThread);
            const auto bypass = _bypass.load(std::memory_order_acquire);
            if (own || (bypass != nullptr && bypass()) || (!trapCovers(page, page + _page) && !resolvingCovers(page, page + _page, Owner::Any))) {
                // A page a trap-keeping host write holds open is accessible already (the fault came from before the write, or from
                // one of its two hashes): the access is retried on the open page. Opening it here would write-protect an armed
                // page in the middle of the import, which needs it writable.
                if (trapKeptOpen(page) && (*state & (write ? PROT_WRITE : PROT_READ)) != 0) return true;
                if (own) deferCovering(page);
                *state = static_cast<std::uint8_t>(*state & ~ReadTrapped);
                ++_untrapGeneration;
                return ::mprotect(reinterpret_cast<void*>(page), _page, untrappedProtection(*state)) == 0;
            }
        }
        Slot* slot = nullptr;
        while (slot == nullptr) {
            for (auto& candidate : _slots) {
                unsigned expected = Free;
                if (candidate.state.compare_exchange_strong(expected, Claimed, std::memory_order_acq_rel)) {
                    slot = &candidate;
                    break;
                }
            }
            if (slot == nullptr) sched_yield();
        }
        slot->page = page;
        slot->state.store(Queued, std::memory_order_release);
        semaphore_signal(_wake);
        while (semaphore_wait(slot->done) == KERN_ABORTED) {}
        slot->state.store(Free, std::memory_order_release);
        return true;
    }

    void startResolver() {
        _resolving.reserve(ResolvingCapacity);
        _deferred.reserve(ResolvingCapacity);
        if (semaphore_create(mach_task_self(), &_wake, SYNC_POLICY_FIFO, 0) != KERN_SUCCESS) return;
        for (auto& slot : _slots)
            if (semaphore_create(mach_task_self(), &slot.done, SYNC_POLICY_FIFO, 0) != KERN_SUCCESS) return;
        pthread_attr_t attributes;
        pthread_attr_init(&attributes);
        pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
        const bool created = pthread_create(&_resolverThread, &attributes, [](void* watch) -> void* {
            static_cast<Watch*>(watch)->resolveLoop();
            return nullptr;
        }, this) == 0;
        pthread_attr_destroy(&attributes);
        _resolverRunning.store(created, std::memory_order_release);
    }

    void resolveLoop() {
        pthread_setname_np("write watch read traps");
        for (;;) {
            if (semaphore_wait(_wake) != KERN_SUCCESS) continue;
            for (auto& slot : _slots) {
                unsigned expected = Queued;
                if (!slot.state.compare_exchange_strong(expected, Running, std::memory_order_acq_rel)) continue;
                const auto page = slot.page;
                waitForResolving(page, page + _page);
                resolve(page, page + _page);
                {
                    const Lock lock(_lock);
                    untrap(page, page + _page);
                }
                slot.state.store(Done, std::memory_order_release);
                semaphore_signal(slot.done);
            }
        }
    }

    void resolve(std::uintptr_t begin, std::uintptr_t end) {
        std::vector<std::pair<std::uintptr_t, std::uintptr_t>> taken;
        const auto self = pthread_self();
        {
            const Lock lock(_lock);
            taken = takeTraps(begin, end);
            for (const auto& [first, last] : taken) _resolving.push_back({first, last, self});
        }
        if (taken.empty()) return;
        const auto resolver = _resolver.load(std::memory_order_acquire);
        for (const auto& [first, last] : taken) resolver(first, last);
        for (;;) {
            std::pair<std::uintptr_t, std::uintptr_t> deferred;
            {
                const Lock lock(_lock);
                const auto found = std::find_if(_deferred.begin(), _deferred.end(), [&](const Resolving& entry) { return pthread_equal(entry.owner, self); });
                if (found == _deferred.end()) break;
                deferred = {found->begin, found->end};
                _deferred.erase(found);
                auto [first, last] = _traps.equal_range(deferred.first);
                while (first != last && first->second != deferred.second) ++first;
                if (first == last) {
                    const auto entry = std::find_if(_resolving.begin(), _resolving.end(), [&](const Resolving& resolving) { return resolving.begin == deferred.first && resolving.end == deferred.second && pthread_equal(resolving.owner, self); });
                    if (entry != _resolving.end()) _resolving.erase(entry);
                    continue;
                }
                _traps.erase(first);
            }
            taken.push_back(deferred);
            resolver(deferred.first, deferred.second);
        }
        const Lock lock(_lock);
        for (const auto& [first, last] : taken) {
            const auto found = std::find_if(_resolving.begin(), _resolving.end(), [&](const Resolving& entry) { return entry.begin == first && entry.end == last && pthread_equal(entry.owner, self); });
            if (found != _resolving.end()) _resolving.erase(found);
        }
        for (const auto& [first, last] : taken) untrap(first, last);
    }

    void deferCovering(std::uintptr_t page) {
        const auto self = pthread_self();
        for (auto it = _traps.begin(); it != _traps.end() && it->first < page + _page; ++it) {
            if (it->second <= page || _deferred.size() == _deferred.capacity() || _resolving.size() == _resolving.capacity()) continue;
            if (std::any_of(_deferred.begin(), _deferred.end(), [&](const Resolving& entry) { return entry.begin == it->first && entry.end == it->second; })) continue;
            _resolving.push_back({it->first, it->second, self});
            _deferred.push_back({it->first, it->second, self});
        }
    }

    void waitForResolving(std::uintptr_t begin, std::uintptr_t end) {
        for (;;) {
            {
                const Lock lock(_lock);
                if (!resolvingCovers(begin, end, Owner::Others)) return;
            }
            sched_yield();
        }
    }

    bool resolvingCovers(std::uintptr_t begin, std::uintptr_t end, Owner owner) const {
        const auto self = pthread_self();
        return std::any_of(_resolving.begin(), _resolving.end(), [&](const Resolving& entry) {
            if (pageStart(entry.begin) >= end || begin >= pageEnd(entry.end)) return false;
            return owner == Owner::Any || (pthread_equal(entry.owner, self) != 0) == (owner == Owner::Self);
        });
    }

    bool retryUntrapped(std::uintptr_t page, std::uint8_t state, bool write) {
        if ((untrappedProtection(state) & (write ? PROT_WRITE : PROT_READ)) == 0) return false;
        if (_retryGeneration != _untrapGeneration) {
            _retryGeneration = _untrapGeneration;
            _generationRetries = 0;
        }
        if (++_generationRetries > GenerationRetries) return false;
        const auto thread = pthread_self();
        auto found = std::find_if(_retries.begin(), _retries.end(), [&](const Retry& retry) { return retry.used != 0 && retry.page == page && pthread_equal(retry.thread, thread); });
        if (found == _retries.end()) found = std::min_element(_retries.begin(), _retries.end(), [](const Retry& left, const Retry& right) { return left.used < right.used; });
        if (found->used == 0 || found->page != page || !pthread_equal(found->thread, thread) || found->generation != _untrapGeneration) *found = {page, thread, _untrapGeneration, 0, 0};
        found->used = ++_retryUses;
        return ++found->count <= UntrappedRetries;
    }

    template <typename TFunction>
    std::invoke_result_t<TFunction> withoutTraps(std::uintptr_t begin, std::uintptr_t end, bool restore, TFunction function) {
        for (;;) {
            if (_trapping.load(std::memory_order_acquire)) {
                waitForResolving(begin, end);
                resolve(begin, end);
            }
            const Lock lock(_lock);
            if (trapCovers(pageStart(begin), pageEnd(end)) || resolvingCovers(pageStart(begin), pageEnd(end), Owner::Others)) continue;
            if (restore) untrap(begin, end);
            else forEachPage(pageStart(begin), pageEnd(end), [](std::uintptr_t, std::uint8_t& state) { state = static_cast<std::uint8_t>(state & ~ReadTrapped); });
            return function();
        }
    }

    std::vector<std::pair<std::uintptr_t, std::uintptr_t>> takeTraps(std::uintptr_t begin, std::uintptr_t end) {
        std::vector<std::pair<std::uintptr_t, std::uintptr_t>> taken;
        const auto first = pageStart(begin);
        const auto last = pageEnd(end);
        for (auto it = _traps.begin(); it != _traps.end();) {
            if (it->first >= last || it->second <= first) {
                ++it;
                continue;
            }
            taken.emplace_back(*it);
            it = _traps.erase(it);
        }
        return taken;
    }

    void dropTraps(std::uintptr_t begin, std::uintptr_t end) {
        for (const auto& [first, last] : takeTraps(begin, end)) untrap(first, last);
        untrap(begin, end);
    }

    // Whether a host write that keeps read traps (BeginHostWriteKeepingTraps) holds the page open.
    bool trapKeptOpen(std::uintptr_t page) const {
        return std::any_of(_hostWrites.begin(), _hostWrites.end(), [&](const HostWrite& write) { return write.keepsTraps && write.begin <= page && page < write.end; });
    }

    bool trapCovers(std::uintptr_t begin, std::uintptr_t end) const {
        return std::any_of(_traps.begin(), _traps.end(), [&](const auto& trap) { return trap.first < end && begin < trap.second; });
    }

    void untrap(std::uintptr_t begin, std::uintptr_t end) {
        const auto first = pageStart(begin);
        const auto last = pageEnd(end);
        std::vector<std::pair<std::uintptr_t, std::uintptr_t>> remaining;
        for (const auto& trap : _traps)
            if (trap.first < last && first < trap.second) remaining.push_back(trap);
        forEachPage(first, last, [&](std::uintptr_t page, std::uint8_t& state) {
            if ((state & ReadTrapped) == 0) return;
            if (std::any_of(remaining.begin(), remaining.end(), [&](const auto& trap) { return trap.first < page + _page && page < trap.second; })) return;
            if (resolvingCovers(page, page + _page, Owner::Others)) return;
            state = static_cast<std::uint8_t>(state & ~ReadTrapped);
            // Inside an open trap-keeping host write the page joins the write: open and written, as its untrapped pages are.
            if (trapKeptOpen(page)) state = static_cast<std::uint8_t>((state & (ProtectionMask | Fresh)) | Written);
            ::mprotect(reinterpret_cast<void*>(page), _page, untrappedProtection(state));
            ++_untrapGeneration;
        });
    }

    static int untrappedProtection(std::uint8_t state) {
        return (state & Protected) != 0 ? (state & ProtectionMask & ~PROT_WRITE) : (state & ProtectionMask);
    }

    std::uint8_t* pageState(std::uintptr_t page) {
        auto it = _ranges.upper_bound(page);
        if (it == _ranges.begin()) return nullptr;
        --it;
        if (page >= it->second.end) return nullptr;
        return &it->second.pages[(page - it->first) / _page];
    }

    std::uintptr_t pageStart(std::uintptr_t address) const {
        return address & ~(_page - 1);
    }

    std::uintptr_t pageEnd(std::uintptr_t address) const {
        return (address + _page - 1) & ~(_page - 1);
    }

    template <typename TFunction>
    void forEachPage(std::uintptr_t begin, std::uintptr_t end, TFunction function) {
        auto it = _ranges.upper_bound(begin);
        if (it != _ranges.begin()) --it;
        for (; it != _ranges.end() && it->first < end; ++it) {
            const auto first = std::max(begin, it->first);
            const auto last = std::min(end, it->second.end);
            for (auto page = first; page < last; page += _page) function(page, it->second.pages[(page - it->first) / _page]);
        }
    }

    void protect(std::uintptr_t begin, std::uintptr_t end) {
        std::uintptr_t runStart = 0;
        std::uintptr_t previous = begin;
        int runProtection = -1;
        const auto flush = [&](std::uintptr_t runEnd) {
            if (runProtection >= 0) ::mprotect(reinterpret_cast<void*>(runStart), runEnd - runStart, runProtection);
            runProtection = -1;
        };
        forEachPage(begin, end, [&](std::uintptr_t page, std::uint8_t& state) {
            const int wanted = (state & ReadTrapped) != 0 ? (trapKeptOpen(page) ? -1 : PROT_NONE) : (state & Protected) != 0 ? (state & ProtectionMask & ~PROT_WRITE) : -1;
            if (wanted != runProtection || page != previous) {
                flush(previous);
                runStart = page;
                runProtection = wanted;
            }
            previous = page + _page;
        });
        flush(previous);
    }

    bool covers(std::uintptr_t begin, std::uintptr_t end) const {
        auto it = _ranges.upper_bound(begin);
        if (it == _ranges.begin()) return false;
        --it;
        auto cursor = begin;
        for (; it != _ranges.end() && it->first <= cursor; ++it) {
            if (it->second.end > cursor) cursor = it->second.end;
            if (cursor >= end) return true;
        }
        return false;
    }

    bool erase(std::uintptr_t begin, std::uintptr_t end) {
        bool removed = false;
        auto it = _ranges.upper_bound(begin);
        if (it != _ranges.begin()) --it;
        while (it != _ranges.end() && it->first < end) {
            const auto rangeBegin = it->first;
            const auto rangeEnd = it->second.end;
            if (rangeEnd <= begin) {
                ++it;
                continue;
            }
            removed = true;
            auto pages = std::move(it->second.pages);
            it = _ranges.erase(it);
            const auto cutBegin = std::max(begin, rangeBegin);
            const auto cutEnd = std::min(end, rangeEnd);
            for (auto page = cutBegin; page < cutEnd; page += _page) {
                const auto state = pages[(page - rangeBegin) / _page];
                if ((state & (Protected | ReadTrapped)) != 0) ::mprotect(reinterpret_cast<void*>(page), _page, state & ProtectionMask);
            }
            if (rangeBegin < cutBegin) _ranges.emplace(rangeBegin, Range{cutBegin, std::vector<std::uint8_t>(pages.begin(), pages.begin() + (cutBegin - rangeBegin) / _page)});
            if (cutEnd < rangeEnd) it = _ranges.emplace(cutEnd, Range{rangeEnd, std::vector<std::uint8_t>(pages.begin() + (cutEnd - rangeBegin) / _page, pages.end())}).first;
        }
        return removed;
    }

    const std::uintptr_t _page = static_cast<std::uintptr_t>(::getpagesize());
    std::size_t _faultAhead = 32;
    bool _available = false;
    bool _validate = true;
    bool _windowStores = false;
    Mutex _lock;
    std::map<std::uintptr_t, Range> _ranges;
    std::vector<HostWrite> _hostWrites;
    std::uint64_t _hostWriteTokens = 0;
    std::multimap<std::uintptr_t, std::uintptr_t> _traps;
    std::vector<Resolving> _resolving;
    std::vector<Resolving> _deferred;
    std::array<Retry, RetrySlots> _retries{};
    std::uint64_t _retryUses = 0;
    std::uint64_t _retryGeneration = 0;
    unsigned _generationRetries = 0;
    std::uint64_t _untrapGeneration = 0;
    std::atomic<bool> _trapping{false};
    std::array<std::atomic<std::uint64_t>, LayoutSlots> _layout{};
    std::atomic<std::uint64_t> _layoutChanges{0};
    std::atomic<void (*)(std::uintptr_t, std::uintptr_t)> _resolver{nullptr};
    std::atomic<bool (*)()> _bypass{nullptr};
#ifdef APS5_WRITE_WATCH_TEST_HOOKS
    std::atomic<void (*)(std::uintptr_t)> _accessHook{nullptr};
#endif
    std::once_flag _resolverStarted;
    std::atomic<bool> _resolverRunning{false};
    pthread_t _resolverThread{};
    semaphore_t _wake = MACH_PORT_NULL;
    std::array<Slot, ResolveSlots> _slots;
    struct sigaction _previousBus {};
    struct sigaction _previousSegv {};
};

const bool g_opened = (Watch::Get(), true);
#endif

}

bool GuestWriteWatchAvailable_nid_postfix() {
#ifdef APS5_GUEST_WRITE_WATCH
    return Watch::Get().Available();
#else
    return false;
#endif
}

void GuestWriteWatchRegister_nid_postfix(const void* pointer, std::size_t bytes) {
#ifdef APS5_GUEST_WRITE_WATCH
    const auto begin = reinterpret_cast<std::uintptr_t>(pointer);
    Watch::Get().Register(begin, begin + bytes);
#else
    static_cast<void>(pointer);
    static_cast<void>(bytes);
#endif
}

bool GuestWriteWatchUnregister_nid_postfix(const void* pointer, std::size_t bytes) {
#ifdef APS5_GUEST_WRITE_WATCH
    const auto begin = reinterpret_cast<std::uintptr_t>(pointer);
    return Watch::Get().Unregister(begin, begin + bytes);
#else
    static_cast<void>(pointer);
    static_cast<void>(bytes);
    return false;
#endif
}

bool GuestWriteWatchAlias_nid_postfix(const void* first, const void* second, std::size_t bytes) {
#if defined(__linux__) && defined(PAGEMAP_SCAN) && defined(UFFD_FEATURE_WP_ASYNC)
    return Watch::Get().Alias(reinterpret_cast<std::uintptr_t>(first), reinterpret_cast<std::uintptr_t>(second), bytes);
#else
    static_cast<void>(first);
    static_cast<void>(second);
    static_cast<void>(bytes);
    return false;
#endif
}

bool GuestWriteWatchCovers_nid_postfix(std::uintptr_t address, std::size_t bytes) {
#ifdef APS5_GUEST_WRITE_WATCH
    return bytes != 0 && address + bytes > address && Watch::Get().Covers(address, address + bytes);
#else
    static_cast<void>(address);
    static_cast<void>(bytes);
    return false;
#endif
}

bool GuestWriteWatchOverlaps_nid_postfix(std::uintptr_t address, std::size_t bytes) {
#ifdef APS5_GUEST_WRITE_WATCH
    return bytes != 0 && address + bytes > address && Watch::Get().Overlaps(address, address + bytes);
#else
    static_cast<void>(address);
    static_cast<void>(bytes);
    return false;
#endif
}

bool GuestWriteWatchQueryRange_nid_postfix(std::uintptr_t address, std::size_t bytes, GuestWriteWatchRangeInfo* info) {
    GuestWriteWatchRangeInfo local;
    if (info == nullptr) info = &local;
#if defined(__APPLE__)
    if (bytes == 0 || address + bytes < address) {
        *info = {};
        return false;
    }
    return Watch::Get().QueryRange(address, address + bytes, *info);
#else
    static_cast<void>(address);
    static_cast<void>(bytes);
    *info = {};
    return false;
#endif
}

bool GuestWriteWatchHandleFault_nid_postfix(std::uintptr_t address) {
#if defined(__APPLE__)
    return Watch::Get().HandleFault(address);
#else
    static_cast<void>(address);
    return false;
#endif
}

bool GuestWriteWatchHandleAccess_nid_postfix(std::uintptr_t address, int write) {
#if defined(__APPLE__)
    return Watch::Get().HandleAccess(address, write != 0);
#else
    static_cast<void>(address);
    static_cast<void>(write);
    return false;
#endif
}

bool GuestWriteWatchCollect_nid_postfix(std::uintptr_t address, std::size_t bytes, void (*written)(void* context, std::uintptr_t begin, std::uintptr_t end), void* context) {
#ifdef APS5_GUEST_WRITE_WATCH
    if (bytes == 0 || address + bytes < address) return false;
    return Watch::Get().Collect(address, address + bytes, written, context);
#else
    static_cast<void>(address);
    static_cast<void>(bytes);
    static_cast<void>(written);
    static_cast<void>(context);
    return false;
#endif
}

void GuestWriteWatchBeginHostWrite_nid_postfix(const void* pointer, std::size_t bytes) {
#if defined(__APPLE__)
    const auto begin = reinterpret_cast<std::uintptr_t>(pointer);
    Watch::Get().BeginHostWrite(begin, begin + bytes);
#else
    static_cast<void>(pointer);
    static_cast<void>(bytes);
#endif
}

std::uint64_t GuestWriteWatchBeginHostWriteKeepingTraps_nid_postfix(const void* pointer, std::size_t bytes) {
#if defined(__APPLE__)
    const auto begin = reinterpret_cast<std::uintptr_t>(pointer);
    return Watch::Get().BeginHostWriteKeepingTraps(begin, begin + bytes);
#else
    static_cast<void>(pointer);
    static_cast<void>(bytes);
    return 0;
#endif
}

void GuestWriteWatchEndHostWriteKeepingTraps_nid_postfix(std::uint64_t token) {
#if defined(__APPLE__)
    Watch::Get().EndHostWriteKeepingTraps(token);
#else
    static_cast<void>(token);
#endif
}

void GuestWriteWatchEndHostWrite_nid_postfix(const void* pointer, std::size_t bytes) {
#if defined(__APPLE__)
    const auto begin = reinterpret_cast<std::uintptr_t>(pointer);
    Watch::Get().EndHostWrite(begin, begin + bytes);
#else
    static_cast<void>(pointer);
    static_cast<void>(bytes);
#endif
}

void GuestWriteWatchProtectionChanged_nid_postfix(const void* pointer, std::size_t bytes, int protection) {
#if defined(__APPLE__)
    const auto begin = reinterpret_cast<std::uintptr_t>(pointer);
    Watch::Get().ProtectionChanged(begin, begin + bytes, protection);
#else
    static_cast<void>(pointer);
    static_cast<void>(bytes);
    static_cast<void>(protection);
#endif
}

int GuestWriteWatchProtect_nid_postfix(void* pointer, std::size_t bytes, int protection) {
#if defined(__APPLE__)
    const auto begin = reinterpret_cast<std::uintptr_t>(pointer);
    return Watch::Get().Protect(begin, begin + bytes, protection);
#else
    static_cast<void>(pointer);
    static_cast<void>(bytes);
    static_cast<void>(protection);
    return ENOSYS;
#endif
}

int GuestWriteWatchProtectKeepingTraps_nid_postfix(void* pointer, std::size_t bytes, int protection) {
#if defined(__APPLE__)
    const auto begin = reinterpret_cast<std::uintptr_t>(pointer);
    return Watch::Get().ProtectKeepingTraps(begin, begin + bytes, protection);
#else
    static_cast<void>(pointer);
    static_cast<void>(bytes);
    static_cast<void>(protection);
    return ENOSYS;
#endif
}

void GuestWriteWatchResolveReadTraps_nid_postfix(std::uintptr_t address, std::size_t bytes) {
#if defined(__APPLE__)
    if (bytes != 0 && address + bytes > address) Watch::Get().ResolveReadTraps(address, address + bytes);
#else
    static_cast<void>(address);
    static_cast<void>(bytes);
#endif
}

int GuestWriteWatchProtection_nid_postfix(std::uintptr_t address, std::uintptr_t limit, std::uintptr_t* runEnd) {
#if defined(__APPLE__)
    return Watch::Get().Protection(address, limit, runEnd);
#else
    static_cast<void>(address);
    *runEnd = limit;
    return -1;
#endif
}

int GuestWriteWatchAccess_nid_postfix(std::uintptr_t address, std::uintptr_t limit, std::uintptr_t* runEnd) {
#if defined(__APPLE__)
    return Watch::Get().Access(address, limit, runEnd);
#else
    static_cast<void>(address);
    static_cast<void>(limit);
    static_cast<void>(runEnd);
    return -1;
#endif
}

int GuestWriteWatchLogical_nid_postfix(std::uintptr_t address, std::uintptr_t* runBegin, std::uintptr_t* runEnd, std::uint64_t* generation) {
#if defined(__APPLE__)
    return Watch::Get().Logical(address, runBegin, runEnd, generation);
#else
    static_cast<void>(address);
    static_cast<void>(runBegin);
    static_cast<void>(runEnd);
    static_cast<void>(generation);
    return -1;
#endif
}

std::uint64_t GuestWriteWatchLayoutChanges_nid_postfix() {
#if defined(__APPLE__)
    return Watch::Get().LayoutChanges();
#else
    return 0;
#endif
}

std::uint64_t GuestWriteWatchLayoutGeneration_nid_postfix(std::uintptr_t address) {
#if defined(__APPLE__)
    return Watch::Get().LayoutGeneration(address);
#else
    static_cast<void>(address);
    return 0;
#endif
}

#if defined(__APPLE__) && defined(APS5_WRITE_WATCH_TEST_HOOKS)
void GuestWriteWatchSetAccessHook_nid_postfix(void (*hook)(std::uintptr_t address)) {
    Watch::Get().SetAccessHook(hook);
}
#endif

bool GuestWriteWatchCollectFresh_nid_postfix(std::uintptr_t address, std::size_t bytes, void (*written)(void* context, std::uintptr_t begin, std::uintptr_t end), void* context) {
#if defined(__linux__) && defined(PAGEMAP_SCAN) && defined(UFFD_FEATURE_WP_ASYNC)
    if (bytes == 0 || address + bytes < address) return false;
    return Watch::Get().CollectFresh(address, address + bytes, written, context);
#elif defined(__APPLE__)
    if (bytes == 0 || address + bytes < address) return false;
    return Watch::Get().Collect(address, address + bytes, written, context, Watch::Take::Fresh);
#else
    static_cast<void>(address);
    static_cast<void>(bytes);
    static_cast<void>(written);
    static_cast<void>(context);
    return false;
#endif
}

bool GuestWriteWatchCollectArmed_nid_postfix(std::uintptr_t address, std::size_t bytes, void (*written)(void* context, std::uintptr_t begin, std::uintptr_t end), void* context) {
#if defined(__linux__) && defined(PAGEMAP_SCAN) && defined(UFFD_FEATURE_WP_ASYNC)
    if (bytes == 0 || address + bytes < address) return false;
    return Watch::Get().CollectArmed(address, address + bytes, written, context);
#elif defined(__APPLE__)
    if (bytes == 0 || address + bytes < address) return false;
    return Watch::Get().Collect(address, address + bytes, written, context, Watch::Take::Armed);
#else
    static_cast<void>(address);
    static_cast<void>(bytes);
    static_cast<void>(written);
    static_cast<void>(context);
    return false;
#endif
}

bool GuestWriteWatchSetReadTrapResolver_nid_postfix(void (*resolve)(std::uintptr_t begin, std::uintptr_t end)) {
#if defined(__APPLE__)
    return Watch::Get().SetReadTrapResolver(resolve);
#else
    static_cast<void>(resolve);
    return false;
#endif
}

void GuestWriteWatchSetReadTrapBypass_nid_postfix(bool (*bypass)()) {
#if defined(__APPLE__)
    Watch::Get().SetReadTrapBypass(bypass);
#else
    static_cast<void>(bypass);
#endif
}

bool GuestWriteWatchTrapReads_nid_postfix(std::uintptr_t address, std::size_t bytes) {
#if defined(__APPLE__)
    return bytes != 0 && address + bytes > address && Watch::Get().TrapReads(address, address + bytes);
#else
    static_cast<void>(address);
    static_cast<void>(bytes);
    return false;
#endif
}

void GuestWriteWatchReadTraps_nid_postfix(std::uintptr_t address, std::size_t bytes, void (*trapped)(void* context, std::uintptr_t begin, std::uintptr_t end), void* context) {
#if defined(__APPLE__)
    if (bytes != 0 && address + bytes > address) Watch::Get().ReadTraps(address, address + bytes, trapped, context);
#else
    static_cast<void>(address);
    static_cast<void>(bytes);
    static_cast<void>(trapped);
    static_cast<void>(context);
#endif
}

void GuestWriteWatchReleaseReads_nid_postfix(std::uintptr_t address, std::size_t bytes) {
#if defined(__APPLE__)
    if (bytes != 0 && address + bytes > address) Watch::Get().ReleaseReads(address, address + bytes);
#else
    static_cast<void>(address);
    static_cast<void>(bytes);
#endif
}

}
