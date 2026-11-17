// Link stubs for symbols the smoke test does not exercise: they belong to
// libkernel / libc / AgcDriver / AppMetadata / Mouse and are provided by the
// real AnyPS5 objects in the full link.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <span>
#include "prx/libSceAgcDriver/Execution/include/AspectFit.hpp"
#include "prx/libkernel/AppMetadata/include/AppMetadata.hpp"
#include "Decoder/Png.hpp"
extern "C" {
bool GuestCopyIn_nid_no_patch(void* d, const void* s, std::size_t n) { std::memcpy(d, s, n); return true; }
bool GuestCopyOut_nid_no_patch(void* d, const void* s, std::size_t n) { std::memcpy(d, s, n); return true; }
void CopyGuestString_nid_no_patch() { std::abort(); }
void NotImplementedAt_nid_no_patch(const char* f, const char*, int) { std::printf("NotImplemented %s\n", f); std::abort(); }
void AgcDriverLockVulkanLoader_nid_postfix() {}
void AgcDriverUnlockVulkanLoader_nid_postfix() {}
bool MouseIsOpen_nid_no_patch() { return false; }
AgcDriver::AspectFitSize ComputeContainSize_nid_postfix(std::uint32_t sw, std::uint32_t sh, std::uint32_t bw, std::uint32_t bh, bool) {
    double s = std::min(double(bw) / sw, double(bh) / sh); return {std::uint32_t(sw * s), std::uint32_t(sh * s)}; }
AppTitle GetAppTitle_nid_postfix() { AppTitle t{}; std::strcpy(t.value, "nps5-smoke"); return t; }
bool HasAppIcon_nid_postfix() { return false; }
AppIconData GetAppIconData_nid_postfix() { return {nullptr, 0}; }
int sceKernelClose(int) { return -1; } int sceKernelOpen(const char*, int, int) { return -1; }
long sceKernelRead(int, void*, std::size_t) { return -1; } long sceKernelWrite(int, const void*, std::size_t) { return -1; }
long sceKernelPwrite(int, const void*, std::size_t, long) { return -1; } long sceKernelLseek(int, long, int) { return -1; }
int sceKernelStat(const char*, void*) { return -1; } int sceKernelGetFsSandboxRandomWord(char*, std::size_t) { return -1; }
std::uint64_t sceKernelGetProcessTime() { return 0; }
int sceSystemServiceParamGetInt(int, int* v) { if (v) *v = 0; return 0; }
}
namespace Decoder::Png { std::optional<Image> Decode(std::span<const std::uint8_t>) { return std::nullopt; } }
