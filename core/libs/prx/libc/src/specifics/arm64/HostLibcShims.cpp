// nps5: native arm64 macOS host build (APS5_HOST_ARM64).
//
// The guest C library files that cannot be compiled for arm64 (x87 long double, SSE intrinsics,
// GuestCpu) are left out of the libc target. These shims provide the few exports that the
// remaining host-support files still call, forwarding to the macOS libc. Format strings are
// ordinary C formats; guest long double (%Lf) is not supported here.
// See docs/nps5-arm64.md.
#if defined(__APPLE__) && defined(__aarch64__)

#include "prx/libc/include/General.hpp"
#include "prx/libc/include/LegacySignals.hpp"
#include <atomic>
#include <cstdarg>
#include <cstddef>
#include <cstdio>

extern "C" int snprintf_nid_postfix(char* buffer, std::size_t size, const char* format, ...) {
    va_list args;
    va_start(args, format);
    const int result = std::vsnprintf(buffer, size, format, args);
    va_end(args);
    return result;
}

// Registration hooks that libkernel calls at load time. The files that normally define them
// (AllocatingStrings.cpp, Syslog.cpp, LegacySignals.cpp) depend on the guest printf/long double code.
namespace {
std::atomic<GuestWriteHook_nid_no_patch> g_hostGuestWriteHook{nullptr};
std::atomic<const char* const*> g_hostProgramName{nullptr};
std::atomic<LegacySigaction_nid_no_patch> g_hostSigvecAction{nullptr};
}

extern "C" {

void LibcSetGuestWriteHook_nid_no_patch(GuestWriteHook_nid_no_patch hook) {
    g_hostGuestWriteHook.store(hook, std::memory_order_release);
}

void LibcRegisterProgramName_nid_no_patch(const char* const* name) {
    g_hostProgramName.store(name, std::memory_order_release);
}

void RegisterSigvec_nid_no_patch(LegacySigaction_nid_no_patch action) {
    g_hostSigvecAction.store(action, std::memory_order_release);
}

}

#endif
