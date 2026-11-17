#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_PLACEHOLDERTEXEL_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_PLACEHOLDERTEXEL_HPP

// nps5-placeholder: what a sampled texture shows while its guest memory holds no data yet.
//
// A texture whose guest pages were never written reads back as zeros: (0,0,0,0) when sampled, which is a black flash when the
// texture is a late-streamed UE texture. The upload is replaced by a solid mid-grey (0.5, alpha 1) in the texture's own format, so
// the flash is a neutral colour. The grey is written into the upload copy only; the texture cache keeps the real (zero) bytes, so
// the first write the guest makes to the memory is noticed the usual way and the real data replaces the placeholder.
//
// Switches (all read once):
//   APS5_PLACEHOLDER_TEX      default 1: substitute never-written all-zero textures. 0 = off, 2 = also print statistics.
//   APS5_PLACEHOLDER_STATS    1: count and log (also with APS5_PLACEHOLDER_TEX=0).
//   APS5_DELAY_TEXTURES=N     test hook: for the first N presents every newly created sampled texture gets the placeholder
//                             (APS5_PLACEHOLDER_TEX=1) or zeros (=0) instead of its data, then is rebuilt from guest memory.
// No Vulkan dependency, so it is unit-testable.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

namespace AgcDriver::Graphics {

struct PlaceholderPattern {
    std::uint8_t bytes[16] = {};
    std::uint32_t size = 0;  // 0: no placeholder for this format (integer, depth, signed-normalized or HDR block formats)
};

// The repeating element (texel or compression block) of a solid mid-grey for a guest data format.
PlaceholderPattern PlaceholderPatternFor(std::uint32_t guestFormat);
bool AllZero(std::span<const std::byte> bytes);
void FillPattern(std::span<std::byte> bytes, const PlaceholderPattern& pattern);

bool PlaceholderTexturesEnabled();
bool PlaceholderStatsEnabled();
std::uint64_t DelayTexturesPresents();

struct PlaceholderCounters {
    std::atomic<std::uint64_t> created{0};      // sampled textures made from a guest snapshot
    std::atomic<std::uint64_t> zeroUnwritten{0};  // all zero and never written
    std::atomic<std::uint64_t> zeroWritten{0};    // all zero but written (kept as is)
    std::atomic<std::uint64_t> substituted{0};
    std::atomic<std::uint64_t> noPattern{0};      // zero, but no placeholder for the format
    std::atomic<std::uint64_t> delayed{0};
    std::atomic<std::uint64_t> absentSampled{0};  // descriptor slots bound to the zero placeholder image
    std::atomic<std::uint64_t> absentStorage{0};
    std::atomic<std::uint64_t> storageCreated{0};
};
PlaceholderCounters& PlaceholderStats();
// Logs a line when `count` reaches a power of two (the title is killed, so there is no reliable exit hook).
void PlaceholderNote(const char* what, std::uint64_t count, const char* detail);

}

#endif
