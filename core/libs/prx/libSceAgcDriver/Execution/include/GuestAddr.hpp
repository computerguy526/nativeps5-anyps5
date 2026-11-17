#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_GUESTADDR_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_GUESTADDR_HPP

// The one choke point between guest (GPU-visible) addresses and host pointers (docs/nps5-gpu-addr.md).
//
// Addresses that come from guest data (PM4 words, registers, descriptors, shader headers, export
// arguments, captures) are guest addresses; the driver keeps them as std::uint64_t. A host pointer is
// `guest + bias`; the bias is set once by the host runtime (AgcSetGuestBias) and is 0 by default, in
// which case every function here is the identity and the x86 configuration behaves as before.
//
// Rules:
//   * dereference a guest address only through GuestToHost / HostPtr / GuestToHostChecked;
//   * hand a host address (a pointer the driver or the host allocated: GDS, staging, a Vulkan
//     import alias) to anything that expects a guest address only through HostToGuest;
//   * guest 0 is null in both directions.

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace AgcDriver::GuestAddr {

struct State {
    std::atomic<std::int64_t> bias{0};
    // Guest range [lo, hi) the checked variants accept. Default: the console's 40-bit application space.
    std::atomic<std::uint64_t> lo{0};
    std::atomic<std::uint64_t> hi{1ull << 40};
};

State& Global();

inline std::int64_t Bias() { return Global().bias.load(std::memory_order_relaxed); }
void SetBias(std::int64_t bias);
void SetArena(std::uint64_t lo, std::uint64_t hi);

// Guest address -> host address. NULL is preserved. No validation (see GuestToHostChecked).
inline std::uintptr_t HostAddress(std::uint64_t guest) {
    return guest == 0 ? 0 : static_cast<std::uintptr_t>(guest + static_cast<std::uint64_t>(Bias()));
}

// Host address -> guest address. NULL is preserved.
inline std::uint64_t GuestAddress(std::uintptr_t host) {
    return host == 0 ? 0 : static_cast<std::uint64_t>(host) - static_cast<std::uint64_t>(Bias());
}
inline std::uint64_t HostToGuest(const void* host) { return GuestAddress(reinterpret_cast<std::uintptr_t>(host)); }

// The requested `GuestToHost(gpu_va, len)`: the host pointer of a guest range. `len` documents the
// extent that will be touched; it is only validated by the checked variant.
inline void* GuestToHost(std::uint64_t gpu_va, std::size_t len = 0) {
    static_cast<void>(len);
    return reinterpret_cast<void*>(HostAddress(gpu_va));
}

template <typename T>
inline T* HostPtr(std::uint64_t guest) { return reinterpret_cast<T*>(HostAddress(guest)); }

// Whether [gpu_va, gpu_va + len) lies inside the guest arena the runtime registered (SetArena).
bool InArena(std::uint64_t gpu_va, std::size_t len);

// GuestToHost that fails loudly (std::runtime_error naming the address and the arena) when the range
// is null, wraps, or is not inside the guest arena. Use it where the address comes straight from
// guest-controlled data that is about to be dereferenced.
void* GuestToHostChecked(std::uint64_t gpu_va, std::size_t len);

template <typename T>
inline T* HostPtrChecked(std::uint64_t guest, std::size_t len = sizeof(T)) { return static_cast<T*>(GuestToHostChecked(guest, len)); }

// Driver-owned memory the GPU/shader side must be able to name by guest address (the global data share, the
// float-behaviour probes' buffers). With bias 0 it is ordinary heap memory whose host address is its guest
// address. With a bias, a host address below the bias has no 48-bit guest name, so the block is carved out of
// the scratch window the runtime reserved inside the guest arena (SetScratch): `guest()` is the address to put
// in descriptors and GuestMemory calls, `data()` the host pointer for the driver's own accesses.
class Scratch {
public:
    explicit Scratch(std::size_t bytes, std::size_t alignment = 256);
    ~Scratch();
    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;
    void* data() const { return host; }
    std::uint64_t guest() const { return guestAddress; }
    std::size_t size() const { return length; }

private:
    void* host = nullptr;
    std::uint64_t guestAddress = 0;
    std::size_t length = 0;
    bool heap = false;
};

// Reserve [guest_lo, guest_lo + bytes) of the guest arena for Scratch (the runtime must not hand it to the guest).
void SetScratch(std::uint64_t guest_lo, std::uint64_t bytes);

}

// Host runtime API (C linkage, for nativeps5's nps5_mem to call at init). The bias is
// `host = guest + bias` (nps5_mem: g_nps5_arena.bias); the arena is the guest range it backs.
extern "C" {
void AgcSetGuestBias(std::int64_t bias);
std::int64_t AgcGetGuestBias();
void AgcSetGuestArena(std::uint64_t guest_lo, std::uint64_t guest_hi);
// Guest range the driver may use for its own memory that must have a guest address (see GuestAddr::Scratch).
void AgcSetGuestScratch(std::uint64_t guest_lo, std::uint64_t bytes);
}

#endif
