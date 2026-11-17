#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_GPUCACHESTORE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_GPUCACHESTORE_HPP

// nps5: indexed, zstd-compressed per-title GPU cache (SQLite index + blobs in one file).
// One row per artefact (SPIR-V, MSL, metallib, Metal pipeline archive, VkPipelineCache blob). Each row records the
// version of every build stage it depends on; a stage version change drops only the rows that depend on that stage.
// No Vulkan or recompiler dependency: this header is shared with tools/gpucache and the unit tests.
// See docs/nps5-pipeline-cache.md.

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace GpuCache {

enum class Kind : int { SpirV = 0, Msl = 1, Metallib = 2, PipelineArchive = 3, VkPipelineCache = 4 };
inline constexpr int KindCount = 5;
const char* KindName(Kind kind);
// A size in megabytes from the environment ("512", "0.5") as bytes. False for anything else (empty, text, negative,
// not finite, above 2^40 MB): the caller keeps its default.
bool ParseMegabytes(const char* text, std::uint64_t& bytes);
// A kind value read from a file is inside the enum.
bool ValidKind(std::int64_t kind);
// An entry name is a plain file name: 1 to 200 bytes, no path separator, no control character, not "." or "..".
bool ValidName(const std::string& name);
bool ParseKind(const std::string& text, Kind& kind);

enum class Stage : int { Recompiler = 0, Mesa = 1, MacOs = 2, MetalCompiler = 3, Gpu = 4 };
inline constexpr int StageCount = 5;
const char* StageName(Stage stage);
bool ParseStage(const std::string& text, Stage& stage);

// Bit mask of the stages an artefact of this kind depends on.
unsigned DependencyMask(Kind kind);

// One version string per stage. An empty string means "not known / not tracked" and never causes an invalidation.
struct StageVersions {
    std::array<std::string, StageCount> value;
    std::string& operator[](Stage stage) { return value[static_cast<std::size_t>(stage)]; }
    const std::string& operator[](Stage stage) const { return value[static_cast<std::size_t>(stage)]; }
};

struct Entry {
    std::int64_t id = 0;
    Kind kind = Kind::Msl;
    std::string name;          // file stem / lookup key (for KosmicKrisp artefacts: the KK key)
    std::string sourceHash;    // hash of what this was derived from (metallib: content hash of its MSL; spirv: request key)
    std::string contentHash;   // SHA-256/128 of the uncompressed payload (content address)
    StageVersions versions;    // empty for stages the kind does not depend on
    std::uint64_t rawBytes = 0;
    std::uint64_t storedBytes = 0;
    std::int64_t created = 0;
    std::int64_t lastUsed = 0;
    std::int64_t useCount = 0;
    bool warm = false;         // used by the last run (prewarm candidate)
    std::string meta;          // free-form key=value lines (MSL options, KK stamp, ...)
};

struct InvalidationReport {
    std::array<std::int64_t, KindCount> removed{};
    std::array<std::int64_t, KindCount> kept{};
    std::array<bool, StageCount> changed{};  // stage whose version differs from what the entries recorded
    std::int64_t invalid = 0;                // unusable rows (kind outside the enum, name that is not a file name): removed
    std::int64_t RemovedTotal() const { std::int64_t n = 0; for (auto r : removed) n += r; return n; }
};

struct Stats {
    std::array<std::int64_t, KindCount> count{};
    std::array<std::uint64_t, KindCount> raw{};
    std::array<std::uint64_t, KindCount> stored{};
    std::uint64_t fileBytes = 0;
};

std::string Sha256Hex16(const void* data, std::size_t size);  // first 16 bytes of SHA-256, 32 hex digits
std::string Sha256Hex16(const std::string& text);

class Store {
public:
    static std::unique_ptr<Store> Open(const std::filesystem::path& file, std::string* error = nullptr);
    ~Store();
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;

    // Compares `current` with the versions recorded by each entry and deletes only the entries that depend on a
    // changed stage. Also remembers `current` as the last-seen versions.
    InvalidationReport Invalidate(const StageVersions& current);
    // Same decision without deleting (used by the CLI's dry run).
    InvalidationReport Preview(const StageVersions& current) const;

    // Insert or replace an entry (same kind + name). `versions` is masked to the kind's dependencies.
    bool Put(Kind kind, const std::string& name, const void* data, std::size_t size, const std::string& sourceHash,
             const StageVersions& versions, const std::string& meta = {}, int zstdLevel = 0);
    bool Has(Kind kind, const std::string& name) const;
    // Decompresses an entry and counts it as a use (updates last_used / use_count / warm).
    bool Get(Kind kind, const std::string& name, std::vector<std::byte>& out, bool touch = true);
    bool GetMeta(Kind kind, const std::string& name, std::string& meta) const;
    bool Remove(Kind kind, const std::string& name);

    // Entries ordered for prewarming: warm first, then last_used descending, then use_count descending.
    std::vector<Entry> List(int kindFilter = -1, bool prewarmOrder = true) const;
    Stats GetStats() const;
    // Deletes least recently used / least used entries until the stored bytes are <= maxStoredBytes. Returns removed count.
    std::int64_t Trim(std::uint64_t maxStoredBytes);
    // Clears the warm flag everywhere (start of a run).
    void ClearWarm();
    void MarkWarm(Kind kind, const std::string& name);
    StageVersions LastSeen() const;
    bool SetLastSeen(const StageVersions& versions);

    // Writes every entry of `kind` as <dir>/<name><extension>, prewarm order, skipping files that already have the
    // right size. `limitBytes` (0 = unlimited) caps the uncompressed bytes written. Returns files written.
    std::size_t Materialize(Kind kind, const std::filesystem::path& dir, const char* extension, std::uint64_t limitBytes = 0,
                            double* decompressMs = nullptr);

    // Bundle: a self-contained store file holding the entries valid for `gpu` (entries whose gpu version matches or
    // that do not depend on the GPU), content addressed. Import merges entries that are not present by content hash.
    std::int64_t ExportBundle(const std::filesystem::path& bundle, const std::string& gpu, std::string* bundleId = nullptr);
    std::int64_t ImportBundle(const std::filesystem::path& bundle);

    void Checkpoint();
    const std::filesystem::path& Path() const { return path; }

private:
    Store() = default;
    struct Impl;
    std::unique_ptr<Impl> impl;
    std::filesystem::path path;
};

}

#endif
