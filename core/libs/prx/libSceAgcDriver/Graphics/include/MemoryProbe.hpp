#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_MEMORYPROBE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_MEMORYPROBE_HPP

#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestTextureResource.hpp"
#include <array>
#include <cstdint>
#include <string>

namespace AgcDriver::Graphics {

// Set once while the library loads (APS5_MEM_PROBE read by a static initialiser), never written
// afterwards: the off path of every entry point is one load of this and a branch, no atomics.
extern const bool MemoryProbeOn;

// WP0 "memprobe" (design notes: research/kyty-eval-astro.md section 5; the question list comes from
// KytyPS5's memory-ownership study, GPL-2.0, re-implemented here). Counters that answer where the
// renderer's own memory goes. Default off: APS5_MEM_PROBE=<N> prints one `[mem]` line every N
// presents (and at exit); unset, every entry point is one relaxed load and a branch.
// APS5_MEM_PROBE_DEEP=1 adds a `[mem-deep]` line (mincore over the live storage surfaces) with each
// summary; it walks pages, so it is for measurement runs only.
class MemoryProbe {
public:
    // Event counters. The first block is the cause of a storage image creation (why the storage
    // cache did not already hold an image for the request).
    enum class Kind : std::uint8_t {
        StorageNew,      // no cached image at this base address
        StorageExtent,   // same base, different width/height (dynamic resolution, resize)
        StorageMips,     // same base, different mip count
        StorageLayers,   // same base, different array window / depth
        StorageFormat,   // same base, different format
        StorageDcc,      // the DCC metadata moved (surface address kept)
        StorageOther,    // same base, key differs in tile mode / pipe XOR / dimension / pitch
        StorageDirect,   // created outside the storage cache (depth planes, cache disabled)
        StorageDestroyed,
        Upload,          // storage upload to the GPU (guest bytes)
        WriteBack,       // storage write-back to guest memory (bytes)
        OriginalCopy,    // StorageTexture::original CPU copy grown (bytes)
        BaselineCopy,    // StorageTexture::generationBaseline CPU copy grown (bytes)
        Count
    };
    static constexpr std::size_t KindCount = static_cast<std::size_t>(Kind::Count);
    static const char* KindName(Kind kind);

    struct Event {
        std::uint64_t count = 0;
        std::uint64_t bytes = 0;
    };
    using Events = std::array<Event, KindCount>;

    // What one gauge sample reports: live bytes against a budget (0 = none).
    struct Gauge {
        std::string name;
        std::uint64_t live = 0;
        std::uint64_t budget = 0;
    };

    static bool Enabled() { return MemoryProbeOn; }
    static void Add(Kind kind, std::uint64_t bytes) noexcept {
        if (MemoryProbeOn) AddOn(kind, bytes);
    }
    static Events Counters() noexcept;

    // Why a creation of `requested` found no usable image while `existing` (an image at the same
    // base address) was cached; Kind::StorageNew when nothing was cached.
    static Kind ClassifyCreation(const GuestTextureResource* existing, const GuestTextureResource& requested, bool metadataMoved) noexcept;

    // The cause the next StorageTexture::Create on this thread reports (default StorageDirect).
    // Set by the storage cache around its Create; Create takes and clears it.
    struct CauseScope {
        explicit CauseScope(Kind kind) noexcept;
        ~CauseScope();
        CauseScope(const CauseScope&) = delete;
        CauseScope& operator=(const CauseScope&) = delete;
    };
    static Kind TakeCause() noexcept;

    // Called by StorageTexture::Create / the destructor with the image's device bytes.
    static void StorageCreated(Kind cause, std::uint64_t imageBytes) noexcept {
        if (MemoryProbeOn) CreatedOn(cause, imageBytes);
    }
    static void StorageDestroyed(std::uint64_t imageBytes) noexcept {
        if (MemoryProbeOn) DestroyedOn(imageBytes);
    }
    // Images created and not yet destroyed, their device bytes, and the peaks since start.
    static std::uint64_t LiveImages() noexcept;
    static std::uint64_t LiveImageBytes() noexcept;
    static std::uint64_t PeakImages() noexcept;
    static std::uint64_t PeakImageBytes() noexcept;

    // The context the pool gauges read (the first storage image created gives it); starts the probe
    // thread when the flag is set.
    static void NoteContext(const Context& context) noexcept;

    // One summary line from the counters (pure: the unit test checks the text). `presents` is the
    // total so far, `window` the presents since the previous line, `previous` the counters then.
    static std::string FormatLine(const char* tag, std::uint64_t presents, std::uint64_t window, const Events& now, const Events& previous, const std::vector<Gauge>& gauges);

    // Prints the summary now (the exit path; a no-op when the probe is off).
    static void Flush(const char* tag = "final") noexcept;

private:
    static void AddOn(Kind kind, std::uint64_t bytes) noexcept;
    static void CreatedOn(Kind cause, std::uint64_t imageBytes) noexcept;
    static void DestroyedOn(std::uint64_t imageBytes) noexcept;
};

}

#endif
