#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_INCLUDE_AGCGUESTMEMORY_H
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_INCLUDE_AGCGUESTMEMORY_H

/*
 * C API for the host runtime (nativeps5's nps5_mem) to tell the AGC driver where guest memory lives.
 * Full contract: docs/nps5-gpu-addr.md, section "What the host runtime must call".
 *
 * Address domains: every `guest` argument below is a GUEST address (the console's address space). The driver finds the
 * memory at `host = guest + bias`. Pass guest values only; never a host pointer.
 *
 * Thread safety: AgcSetGuestBias / AgcSetGuestArena / AgcSetGuestScratch are init-time calls (before any other thread
 * enters the driver, and before any AGC call). The allocation functions may be called from any thread.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* flags for the allocation functions */
#define AGC_GUEST_READ  1u /* CPU-readable (prot & 3)             */
#define AGC_GUEST_WRITE 2u /* CPU-writable (prot & 2); needs READ */
#define AGC_GUEST_GPU   4u /* mapped for the GPU (prot & 0x30)    */
#define AGC_GUEST_WATCH 8u /* Register only: put the range under the write watch (see AgcGuestWriteWatchAvailable) */

/* host = guest + bias. Default 0 (guest == host). Set once, before anything else. */
void AgcSetGuestBias(int64_t bias);
int64_t AgcGetGuestBias(void);

/* The guest range [lo, hi) backed by the host reservation; the checked translations reject anything outside it.
 * Default [0, 2^40). */
void AgcSetGuestArena(uint64_t guest_lo, uint64_t guest_hi);

/* A guest range inside the arena reserved for driver-owned memory that needs a guest address (global data share,
 * probe buffers, fused shaders, register-default tables). It must be inaccessible reserved address space (PROT_NONE)
 * that the runtime never hands to the guest; the driver maps pages in it with MAP_FIXED. Required when the bias is
 * nonzero. 256 MiB is plenty. */
void AgcSetGuestScratch(uint64_t guest_lo, uint64_t bytes);

/* Register a committed, accessible guest mapping with the allocation registry (call after the host mapping exists).
 * Returns 0, or -1 with AgcGuestAllocationLastError() set (overlap with an existing allocation, null/wrapping range). */
int AgcGuestAllocationRegister(uint64_t guest, uint64_t bytes, uint32_t flags);

/* Remove a registration. bytes == 0: the whole allocation that starts at `guest`. bytes != 0: only [guest, guest+bytes)
 * (a partial munmap; the rest stays registered). Call BEFORE the host memory is released or reused: the call waits for GPU
 * work that still holds the range and invalidates the driver's caches. Returns 0 or -1. */
int AgcGuestAllocationUnregister(uint64_t guest, uint64_t bytes);

/* Change the CPU/GPU rights of a registered range. Call before the host mprotect. Returns 0 or -1. */
int AgcGuestAllocationProtect(uint64_t guest, uint64_t bytes, uint32_t flags);

/* Write watch (docs/nps5-coherence.md). 1 when the driver's page write watch runs (macOS: mprotect plus a fault handler)
 * and NPS5_AGC_WRITE_WATCH is not 0. A runtime that registers ranges with AGC_GUEST_WATCH must then
 *  - pass every SIGSEGV/SIGBUS memory fault to GuestWriteWatchHandleAccess_nid_postfix(host address, is write) (libc.prx)
 *    before treating it as a crash, on every thread;
 *  - change the host protection of guest pages only with GuestWriteWatchProtect_nid_postfix (libc.prx), after
 *    AgcGuestAllocationProtect;
 *  - hold GuestArenaBeginHostWrite_nid_postfix / GuestArenaEndHostWrite_nid_postfix (libc.prx) around every kernel-mode
 *    write into guest memory (read, pread, recv, ...) and call GuestArenaPrepareHostRead_nid_no_patch before every
 *    kernel-mode read of guest memory (write, send, ...);
 *  - never watch memory that is mapped at a second address (AgcGuestAllocationWatch(..., 0) for both mappings). */
int AgcGuestWriteWatchAvailable(void);

/* Puts a registered range under the write watch (watch != 0) or takes it out (watch == 0). Returns 0 or -1. */
int AgcGuestAllocationWatch(uint64_t guest, uint64_t bytes, int watch);

/* Text of the last failure on this thread ("" if none). */
const char* AgcGuestAllocationLastError(void);

#ifdef __cplusplus
}
#endif

#endif
