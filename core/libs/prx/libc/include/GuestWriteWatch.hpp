#ifndef CORE_LIBS_PRX_LIBC_INCLUDE_GUESTWRITEWATCH_HPP
#define CORE_LIBS_PRX_LIBC_INCLUDE_GUESTWRITEWATCH_HPP

#include <cstddef>
#include <cstdint>
#include <utility>

namespace GuestWriteWatch {

// Bit GuestWriteWatchProtection adds to the logical protection of a page whose host mapping is deliberately PROT_NONE (a read trap).
constexpr int ProtectionTrapped = 0x100;
// Bit GuestWriteWatchAccess returns for a run with no host mapping.
constexpr int ProtectionUnmapped = 0x200;

// What GuestWriteWatchQueryRange reports about [address, address + bytes), widened to host pages (16 KiB on Apple silicon).
struct GuestWriteWatchRangeInfo {
    std::uintptr_t pageBytes = 0;
    bool covered = false;      // every page of the widened range is watched
    bool headShared = false;   // the first page starts before `address`: other data lives on it
    bool tailShared = false;   // the last page ends after address + bytes
    bool headWritten = false;  // the first / last page is flagged written and not collected yet
    bool tailWritten = false;
    std::size_t watchedPages = 0;
    std::size_t writtenPages = 0;  // flagged and not collected: a collect would report them
    std::size_t trappedPages = 0;  // under a read trap
};

extern "C" {

bool GuestWriteWatchAvailable_nid_postfix();
void GuestWriteWatchRegister_nid_postfix(const void* pointer, std::size_t bytes);
bool GuestWriteWatchUnregister_nid_postfix(const void* pointer, std::size_t bytes);
bool GuestWriteWatchAlias_nid_postfix(const void* first, const void* second, std::size_t bytes);
bool GuestWriteWatchCovers_nid_postfix(std::uintptr_t address, std::size_t bytes);
bool GuestWriteWatchOverlaps_nid_postfix(std::uintptr_t address, std::size_t bytes);
// One lock acquisition, no collection, no syscall: is the range fully watched, are its first and last pages shared with bytes
// outside it, how many of its pages are flagged. Returns `covered`. macOS only (false elsewhere).
bool GuestWriteWatchQueryRange_nid_postfix(std::uintptr_t address, std::size_t bytes, GuestWriteWatchRangeInfo* info);
bool GuestWriteWatchHandleFault_nid_postfix(std::uintptr_t address);
bool GuestWriteWatchHandleAccess_nid_postfix(std::uintptr_t address, int write);
bool GuestWriteWatchCollect_nid_postfix(std::uintptr_t address, std::size_t bytes, void (*written)(void* context, std::uintptr_t begin, std::uintptr_t end), void* context);
void GuestWriteWatchBeginHostWrite_nid_postfix(const void* pointer, std::size_t bytes);
// A host write that needs the pages accessible but not their bytes (a host import): read traps over the range are neither resolved
// nor waited for; their pages are open (writable: the import needs that) until GuestWriteWatchEndHostWriteKeepingTraps, which
// closes the ones still trapped. They do not read as written for having been opened. A CPU store to one of them inside the
// write does not fault and is not recorded; with APS5_IMPORT_WINDOW_STORES=1 a page whose bytes changed inside the write
// reads as written from its end on. Returns the token that ends this write (0: nothing was opened);
// GuestWriteWatchEndHostWrite never ends it. macOS only.
std::uint64_t GuestWriteWatchBeginHostWriteKeepingTraps_nid_postfix(const void* pointer, std::size_t bytes);
void GuestWriteWatchEndHostWriteKeepingTraps_nid_postfix(std::uint64_t token);
// Ends one host write begun with GuestWriteWatchBeginHostWrite over exactly this range.
void GuestWriteWatchEndHostWrite_nid_postfix(const void* pointer, std::size_t bytes);
void GuestWriteWatchProtectionChanged_nid_postfix(const void* pointer, std::size_t bytes, int protection);
int GuestWriteWatchProtect_nid_postfix(void* pointer, std::size_t bytes, int protection);
// GuestWriteWatchProtect for a caller that holds a lock the read trap resolver may need (a mutation of the guest allocation
// registry): the resolver is not called and not waited for. A page with an armed read trap keeps the trap, stays closed and
// takes the new protection when the trap resolves. Call GuestWriteWatchResolveReadTraps before taking the lock.
int GuestWriteWatchProtectKeepingTraps_nid_postfix(void* pointer, std::size_t bytes, int protection);
// Resolves the read traps over the range on the calling thread (which must hold no lock the resolver needs).
void GuestWriteWatchResolveReadTraps_nid_postfix(std::uintptr_t address, std::size_t bytes);
int GuestWriteWatchProtection_nid_postfix(std::uintptr_t address, std::uintptr_t limit, std::uintptr_t* runEnd);
// The protection the guest sees over [address, *runEnd): the host protection, with a read-trapped page widened to its logical
// protection and a watched page limited to it, read consistently with the watch table. -1 when the host cannot be queried.
int GuestWriteWatchAccess_nid_postfix(std::uintptr_t address, std::uintptr_t limit, std::uintptr_t* runEnd);
// The logical protection (PROT_READ | PROT_WRITE) of the watched page at `address`, read from the watch table alone, and the run
// [*runBegin, *runEnd) of pages around it that share it (at most the 2 MiB block of the page). -1 when the page is not watched
// (ask GuestWriteWatchAccess). *generation is the layout generation of the page's block the answer was read at: the answer holds
// for as long as GuestWriteWatchLayoutGeneration(address) returns that value, so a caller may keep it and skip the watch lock and
// the host query.
int GuestWriteWatchLogical_nid_postfix(std::uintptr_t address, std::uintptr_t* runBegin, std::uintptr_t* runEnd, std::uint64_t* generation);
// The layout generation of the 2 MiB block of `address` (blocks share generations by hash). It moves before the watched ranges
// or the logical protection of a watched page in the block change (register, unregister, mprotect). Arming, write faults and
// read traps do not move it: they change the host protection, not what the guest sees.
std::uint64_t GuestWriteWatchLayoutGeneration_nid_postfix(std::uintptr_t address);
// How many changes have moved a layout generation so far (statistics).
std::uint64_t GuestWriteWatchLayoutChanges_nid_postfix();
#ifdef APS5_WRITE_WATCH_TEST_HOOKS
void GuestWriteWatchSetAccessHook_nid_postfix(void (*hook)(std::uintptr_t address));
#endif
bool GuestWriteWatchCollectArmed_nid_postfix(std::uintptr_t address, std::size_t bytes, void (*written)(void* context, std::uintptr_t begin, std::uintptr_t end), void* context);
bool GuestWriteWatchCollectFresh_nid_postfix(std::uintptr_t address, std::size_t bytes, void (*written)(void* context, std::uintptr_t begin, std::uintptr_t end), void* context);
bool GuestWriteWatchSetReadTrapResolver_nid_postfix(void (*resolve)(std::uintptr_t begin, std::uintptr_t end));
void GuestWriteWatchSetReadTrapBypass_nid_postfix(bool (*bypass)());
bool GuestWriteWatchTrapReads_nid_postfix(std::uintptr_t address, std::size_t bytes);
void GuestWriteWatchReadTraps_nid_postfix(std::uintptr_t address, std::size_t bytes, void (*trapped)(void* context, std::uintptr_t begin, std::uintptr_t end), void* context);
void GuestWriteWatchReleaseReads_nid_postfix(std::uintptr_t address, std::size_t bytes);

}

// Releases or replaces the host mapping under [pointer, pointer + bytes) (munmap, mmap MAP_FIXED, a reset to PROT_NONE): the
// watch forgets the range BEFORE `replace` touches the host mapping. Unregistering moves the layout generation of the range, so
// an accessibility answer another thread kept from the watch table (GuestWriteWatchLogical) is dropped before the pages
// change; unregistering after the change left a window in which the table called a page accessible that the host had already
// taken away, and a reader that trusted it faulted. Register the new mapping after this returns.
template <typename TReplace>
decltype(auto) ReplaceMapping(const void* pointer, std::size_t bytes, TReplace&& replace) {
    GuestWriteWatchUnregister_nid_postfix(pointer, bytes);
    return std::forward<TReplace>(replace)();
}

}

#endif
