// The command-buffer builder under a nonzero guest bias (docs/nps5-gpu-addr.md): `host = guest + bias`.
//
// Convention checked here: pointer parameters are host pointers; everything the library writes into GPU-visible data
// (PM4 address words, CommandBuffer cursors, out-slots, shader header fields, register-default tables) is a guest value;
// pointers read out of such data are guest values that the library translates before dereferencing.
#include "prx/libSceAgc/include/Agc.hpp"
#include "prx/libSceAgc/Misc/include/Semaphore.hpp"
#include "prx/libSceAgc/Misc/include/PacketInfo.hpp"
#include "prx/libSceAgc/Misc/include/RegisterDefaults.hpp"
#include "prx/libSceAgc/Patch/include/Jump.hpp"
#include "prx/libSceAgc/Patch/include/WaitRegMem.hpp"
#include "prx/libSceAgc/Cb/include/Branch.hpp"
#include "prx/libSceAgc/DcbFlow/include/Control.hpp"
#include "prx/libSceAgc/DcbFlow/include/Memory.hpp"
#include "prx/libSceAgc/DcbFlow/include/Sync.hpp"
#include "prx/libSceAgc/DcbDraw/include/Instancing.hpp"
#include "prx/libSceAgc/DcbState/include/Display.hpp"
#include "prx/libSceAgc/Shader/include/GsPrimPayload.hpp"
#include "prx/libSceAgc/Shader/include/ShaderConstants.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestAddr.hpp"
#include "prx/libSceAgcDriver/include/AgcGuestMemory.h"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libSceAgcDriver/Execution/include/FrameRecorder.hpp"
#include "prx/libc/include/Shutdown.hpp"

#include <sys/mman.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace {

constexpr std::uint64_t GuestBase = 0x2'0000'0000ull;
constexpr std::size_t ReservationBytes = 1ull << 30;
constexpr std::size_t MappedBytes = 4ull << 20;
constexpr std::uint64_t ScratchOffset = 0x1000'0000ull;
constexpr std::uint64_t ScratchBytes = 0x0100'0000ull;

std::uint8_t* reservation = nullptr;
std::int64_t bias = 0;

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

// Host pointer (inside the reservation) -> the guest value a correct library stores.
std::uint64_t guestOf(const volatile void* host) {
    return reinterpret_cast<std::uint64_t>(host) - static_cast<std::uint64_t>(bias);
}
template <typename T>
T* hostOf(std::uint64_t guest) {
    return reinterpret_cast<T*>(guest + static_cast<std::uint64_t>(bias));
}
std::uint32_t* words(std::size_t byteOffset) { return reinterpret_cast<std::uint32_t*>(reservation + byteOffset); }

// True when `lo, hi` of `address` appear as adjacent dwords in [packet, packet + count).
bool hasAddress(const std::uint32_t* packet, std::size_t count, std::uint64_t address, std::uint32_t lowMask = ~0u) {
    for (std::size_t i = 0; i + 1 < count; ++i) {
        if ((packet[i] & lowMask) == (static_cast<std::uint32_t>(address) & lowMask) && (packet[i + 1] & 0xffffu) == static_cast<std::uint32_t>(address >> 32u)) return true;
    }
    return false;
}

// A command buffer whose storage is guest memory: the cursors hold guest values.
struct GuestBuffer {
    CommandBuffer buffer{};
    std::uint32_t* host;
    GuestBuffer(std::size_t byteOffset, std::size_t dwords) : host(words(byteOffset)) {
        std::memset(host, 0, dwords * sizeof(std::uint32_t));
        auto* const guest = reinterpret_cast<std::uint32_t*>(guestOf(host));
        buffer.bottom = guest;
        buffer.top = guest + dwords;
        buffer.cursor_up = guest;
        buffer.cursor_down = guest + dwords;
    }
    std::uint64_t cursorDwords() const { return static_cast<std::uint64_t>(buffer.cursor_up - reinterpret_cast<std::uint32_t*>(guestOf(host))); }
};

void testPacketsAreGuestValued() {
    GuestBuffer gb(0, 256);
    auto* const target = words(0x10000);
    auto* const label = words(0x20000);

    auto* jump = sceAgcDcbJump(&gb.buffer, 1, 2, target, 0x40);
    check(jump == gb.host, "the returned packet is not the host pointer of the cursor");
    check(gb.buffer.cursor_up == reinterpret_cast<std::uint32_t*>(guestOf(gb.host)) + 4, "the cursor did not advance in the guest domain");
    check(hasAddress(jump, 4, guestOf(target)), "DCB jump target is not a guest address");
    check(!hasAddress(jump, 4, reinterpret_cast<std::uint64_t>(target)), "DCB jump target is a host address");

    auto* atomic = sceAgcDcbAtomicMem(&gb.buffer, 0x2fu, 0, 0, label, 1, 0, 0);
    check(atomic == gb.host + 4, "the next packet does not follow the first through the guest cursor");
    check(hasAddress(atomic, sceAgcDcbAtomicMemGetSize() / 4, guestOf(label)), "atomic memory address is not a guest address");

    auto* prime = sceAgcDcbPrimeUtcl2(&gb.buffer, label, 0x1000);
    check(hasAddress(prime, 4, guestOf(label)), "prime UTCL2 address is not a guest address");

    auto* indirect = sceAgcDcbSetBaseIndirectArgs(&gb.buffer, 0, label);
    check(hasAddress(indirect, 4, guestOf(label)), "indirect base is not a guest address");

    auto* condWrite = sceAgcCbCondWrite(&gb.buffer, 1, 1, label, 5, label + 2, 1, 0xffffffffu);
    check(hasAddress(condWrite, 8, guestOf(label)) && hasAddress(condWrite, 8, guestOf(label + 2)), "COND_WRITE addresses are not guest addresses");

    // Patches take a host pointer to the packet and write a guest address.
    check(sceAgcJumpPatchSetTarget(jump, words(0x30000), 0x10) == 0, "jump patch failed");
    check(hasAddress(jump, 4, guestOf(words(0x30000))), "patched jump target is not a guest address");
    auto* wait = sceAgcDcbWaitRegMem(&gb.buffer, 0, 0, 0, 0, label, 1, 0xffffffffu, 0);
    check(sceAgcWaitRegMemPatchAddress(wait, label + 4) == 0, "wait address patch failed");
    check(hasAddress(wait, 16, guestOf(label + 4)), "patched wait address is not a guest address");
}

struct Growth {
    GuestBuffer next;
    explicit Growth(std::size_t byteOffset) : next(byteOffset, 64) {}
};

bool APS5_VABI grow(CommandBuffer* buffer, std::uint32_t, void* userData) {
    auto& growth = *static_cast<Growth*>(userData);
    // The callback runs in the guest world: it installs guest-valued cursors.
    *buffer = growth.next.buffer;
    return true;
}

void testCursorGrowthThroughCallback() {
    GuestBuffer first(0x4000, 8);
    Growth growth(0x5000);
    first.buffer.callback = grow;
    first.buffer.user_data = &growth;
    first.buffer.cursor_down = first.buffer.cursor_up + 1;  // no room for a jump
    auto* packet = sceAgcDcbJump(&first.buffer, 1, 2, words(0x10000), 0x40);
    check(packet == growth.next.host, "the packet was not written to the grown buffer");
    check(first.buffer.cursor_up == reinterpret_cast<std::uint32_t*>(guestOf(growth.next.host)) + 4, "the grown cursor is not guest-valued");
}

void testOutSlotsAreGuestValued() {
    auto* const memory = words(0x40000);
    check(sceAgcSetSemaphoreMemory(memory, 0x8000) == 0, "semaphore memory registration failed");
    void* label = nullptr;
    check(sceAgcGetSemaphoreLabel(2, &label) == 0, "semaphore label query failed");
    check(reinterpret_cast<std::uint64_t>(label) == guestOf(reinterpret_cast<std::uint8_t*>(memory) + 2 * 32), "the semaphore label is not a guest address");

    GuestBuffer gb(0x6000, 16);
    auto* packet = sceAgcDcbJump(&gb.buffer, 1, 2, words(0x10000), 0x40);
    std::uint32_t* payload = nullptr;
    check(sceAgcGetDataPacketPayloadAddress(&payload, packet, 1) == 0, "payload address query failed");
    check(reinterpret_cast<std::uint64_t>(payload) == guestOf(packet + 2), "the payload address is not a guest address");
}

void testShaderHeaderFieldsAreGuest() {
    // A shader header in guest memory: its cx_registers pointer is a guest value, the header itself is reached through a
    // host pointer.
    auto* const registers = reinterpret_cast<ShaderRegister*>(reservation + 0x50000);
    registers[0] = {ShaderRegs::SPI_SHADER_IDX_FORMAT, 2u};
    auto* const header = reinterpret_cast<Shader*>(reservation + 0x51000);
    *header = Shader{};
    header->cx_registers = reinterpret_cast<ShaderRegister*>(guestOf(registers));
    header->num_cx_registers = 1;
    std::uint32_t bytes = 0;
    check(sceAgcGetGsPrimPayload(&bytes, header) == 0, "GS prim payload failed");
    check(bytes == 8, "the GS prim payload did not read the guest-valued context register pointer");
}

void testRegisterDefaultsAreGuestMemory() {
    const auto* defaults = static_cast<const std::uint8_t*>(sceAgcGetRegisterDefaults2(3));
    check(defaults != nullptr, "no register defaults");
    // RegisterDefaults: tables[4] (guest pointers), registerCounts[4], types, count.
    const auto* tables = reinterpret_cast<const std::uint64_t*>(defaults);
    const auto* counts = reinterpret_cast<const std::uint32_t*>(defaults + 32);
    const std::uint64_t windowLow = GuestBase + ScratchOffset;
    bool any = false;
    for (int bank = 0; bank < 4; ++bank) {
        if (tables[bank] == 0) continue;
        any = true;
        check(counts[bank] != 0, "a bank with a pointer table has no registers");
        check(tables[bank] >= windowLow && tables[bank] < windowLow + ScratchBytes, "a defaults table is not in the guest scratch window");
        const auto* first = hostOf<const std::uint64_t>(tables[bank]);
        check(*first >= windowLow && *first < windowLow + ScratchBytes, "a defaults table entry is not a guest address");
        const auto* reg = hostOf<const ShaderRegister>(*first);
        check(reg->offset < 0x10000u, "a defaults table entry does not name a register");
    }
    check(any, "no defaults bank has a pointer table");
}

// The live-capture region walk is host-keyed underneath (mach_vm_region_recurse) but reports guest addresses.
void testCaptureWalkIsGuestAddressed() {
    constexpr std::size_t MiB = 1ull << 20;
    check(mprotect(reservation + MiB, MiB, PROT_NONE) == 0, "punch a hole in the guest window");
    const auto regions = AgcDriver::AppleWalkGuestRegions(GuestBase, GuestBase + 4 * MiB);
    check(regions.size() == 2, "the walk did not report the two accessible regions");
    check(regions[0].address == GuestBase && regions[0].bytes == MiB, "the first region is not guest-addressed");
    check(regions[1].address == GuestBase + 2 * MiB && regions[1].bytes == 2 * MiB, "the second region is not guest-addressed");
    check(regions[0].readable && regions[0].privateMemory && regions[0].protection == 0x04u, "region attributes are wrong");
    // A range that starts inside a region is clipped to it.
    const auto clipped = AgcDriver::AppleWalkGuestRegions(GuestBase + 0x1000, GuestBase + 0x3000);
    check(clipped.size() == 1 && clipped[0].address == GuestBase + 0x1000 && clipped[0].bytes == 0x2000, "the walk did not clip to the requested range");
    check(mprotect(reservation + MiB, MiB, PROT_READ | PROT_WRITE) == 0, "restore the guest window");
}

// The registry is keyed by guest address: what the runtime registers through the C API is what the driver looks up.
void testAllocationRegistryIsGuestKeyed() {
    constexpr std::uint64_t at = GuestBase + 0x300000;
    check(AgcGuestAllocationRegister(at, 0x4000, AGC_GUEST_READ | AGC_GUEST_WRITE) == 0, AgcGuestAllocationLastError());
    check(AgcGuestAllocationRegister(at + 0x1000, 0x1000, AGC_GUEST_READ) == -1 && AgcGuestAllocationLastError()[0] != 0, "an overlapping registration was accepted");
    {
        // (a held range is a lease: release it before the registry is mutated again)
        const auto range = GuestAllocations::GuestAllocationsLookup_nid_postfix(static_cast<std::uintptr_t>(at + 0x10), false);
        check(range != nullptr && range->address == at && range->bytes == 0x4000 && range->writable && !range->gpu, "the registered range is not keyed by its guest address");
    }
    check(GuestAllocations::GuestAllocationsLookup_nid_postfix(static_cast<std::uintptr_t>(at + 0x10 + static_cast<std::uint64_t>(bias)), false) == nullptr, "the registry answers to a host address");
    check(AgcGuestAllocationProtect(at, 0x1000, AGC_GUEST_READ) == 0, AgcGuestAllocationLastError());
    check(AgcGuestAllocationUnregister(at + 0x2000, 0x1000) == 0, AgcGuestAllocationLastError());  // partial unmap
    check(AgcGuestAllocationUnregister(at, 0) == 0, AgcGuestAllocationLastError());
    check(GuestAllocations::GuestAllocationsLookup_nid_postfix(static_cast<std::uintptr_t>(at + 0x10), false) == nullptr, "the allocation is still registered");
}

}

int main() {
    try {
        void* const mapped = mmap(nullptr, ReservationBytes, PROT_NONE, MAP_ANON | MAP_PRIVATE | MAP_NORESERVE, -1, 0);
        check(mapped != MAP_FAILED, "reserve the guest window");
        reservation = static_cast<std::uint8_t*>(mapped);
        check(mprotect(reservation, MappedBytes, PROT_READ | PROT_WRITE) == 0, "map the guest window");
        bias = static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(reservation)) - static_cast<std::int64_t>(GuestBase);
        AgcSetGuestBias(bias);
        AgcSetGuestArena(GuestBase, GuestBase + ReservationBytes);
        AgcSetGuestScratch(GuestBase + ScratchOffset, ScratchBytes);

        testPacketsAreGuestValued();
        testCursorGrowthThroughCallback();
        testOutSlotsAreGuestValued();
        testShaderHeaderFieldsAreGuest();
        testRegisterDefaultsAreGuestMemory();
        testCaptureWalkIsGuestAddressed();
        testAllocationRegistryIsGuestKeyed();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "guest bias test failed: %s\n", error.what());
        return 1;
    }
    std::puts("AGC guest bias tests passed");
    return 0;
}
