// nps5: native arm64 host build (APS5_HOST_ARM64). System/src/Exception.cpp is x86-64 register-context code
// (SEH/ucontext frames) and is not built for arm64. The two debug exports below are log-only in the x86
// implementation too, so they are reproduced here; the exception handler install/raise exports are not provided.
// See docs/nps5-arm64.md.
#include "prx/libkernel/include/Api.hpp"
#include <cstdio>

extern "C" {

void APS5_VABI sceKernelDebugRaiseException(int c1, int c2) {
    std::fprintf(stderr, "sceKernelDebugRaiseException c1=0x%08x c2=0x%08x caller=%p\n", static_cast<unsigned>(c1), static_cast<unsigned>(c2), __builtin_return_address(0));
}

void APS5_VABI sceKernelDebugRaiseExceptionOnReleaseMode(int c1, int c2) {
    std::fprintf(stderr, "sceKernelDebugRaiseExceptionOnReleaseMode c1=0x%08x c2=0x%08x caller=%p\n", static_cast<unsigned>(c1), static_cast<unsigned>(c2), __builtin_return_address(0));
}

}
