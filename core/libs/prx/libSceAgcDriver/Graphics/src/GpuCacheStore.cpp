#include "prx/libSceAgcDriver/Graphics/include/GpuCacheStore.hpp"
#include <sqlite3.h>
#include <zstd.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <set>
#include <system_error>

namespace GpuCache {

namespace {

constexpr const char* KindNames[KindCount] = {"spirv", "msl", "metallib", "archive", "vkcache"};
constexpr const char* StageNames[StageCount] = {"recompiler", "mesa", "macos", "metalc", "gpu"};
constexpr const char* StageColumns[StageCount] = {"v_recompiler", "v_mesa", "v_macos", "v_metalc", "v_gpu"};

std::int64_t now() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// SHA-256 (public domain style implementation).
struct Sha256 {
    std::uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::uint8_t buffer[64];
    std::uint64_t total = 0;
    std::size_t fill = 0;
    static std::uint32_t rotr(std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
    void block(const std::uint8_t* p) {
        static const std::uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
            0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
            0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
            0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
            0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
            0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        std::uint32_t w[64];
        for (int i = 0; i < 16; ++i) w[i] = (std::uint32_t(p[i * 4]) << 24) | (std::uint32_t(p[i * 4 + 1]) << 16) | (std::uint32_t(p[i * 4 + 2]) << 8) | p[i * 4 + 3];
        for (int i = 16; i < 64; ++i) {
            const auto s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const auto s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        auto a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            const auto t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            const auto t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    void update(const void* data, std::size_t size) {
        auto p = static_cast<const std::uint8_t*>(data);
        total += size;
        while (size > 0) {
            const auto n = std::min<std::size_t>(size, 64 - fill);
            std::memcpy(buffer + fill, p, n);
            fill += n; p += n; size -= n;
            if (fill == 64) { block(buffer); fill = 0; }
        }
    }
    void final(std::uint8_t out[32]) {
        const std::uint64_t bits = total * 8;
        const std::uint8_t one = 0x80, zero = 0;
        update(&one, 1);
        while (fill != 56) update(&zero, 1);
        std::uint8_t len[8];
        for (int i = 0; i < 8; ++i) len[i] = std::uint8_t(bits >> (56 - 8 * i));
        update(len, 8);
        for (int i = 0; i < 8; ++i) { out[i * 4] = h[i] >> 24; out[i * 4 + 1] = h[i] >> 16; out[i * 4 + 2] = h[i] >> 8; out[i * 4 + 3] = h[i]; }
    }
};

struct Statement {
    sqlite3_stmt* s = nullptr;
    Statement(sqlite3* db, const char* sql) { if (sqlite3_prepare_v2(db, sql, -1, &s, nullptr) != SQLITE_OK) s = nullptr; }
    ~Statement() { if (s) sqlite3_finalize(s); }
    explicit operator bool() const { return s != nullptr; }
    void text(int i, const std::string& v) { sqlite3_bind_text(s, i, v.c_str(), static_cast<int>(v.size()), SQLITE_TRANSIENT); }
    void i64(int i, std::int64_t v) { sqlite3_bind_int64(s, i, v); }
    std::string col(int i) const { auto p = sqlite3_column_text(s, i); return p ? std::string(reinterpret_cast<const char*>(p), static_cast<std::size_t>(sqlite3_column_bytes(s, i))) : std::string(); }
    std::int64_t num(int i) const { return sqlite3_column_int64(s, i); }
};

constexpr const char* EntryColumns = "id,kind,name,source_hash,content_hash,v_recompiler,v_mesa,v_macos,v_metalc,v_gpu,raw_bytes,stored_bytes,created,last_used,use_count,warm,meta";

Entry readEntry(const Statement& st) {
    Entry e;
    e.id = st.num(0);
    e.kind = static_cast<Kind>(st.num(1));
    e.name = st.col(2);
    e.sourceHash = st.col(3);
    e.contentHash = st.col(4);
    for (int i = 0; i < StageCount; ++i) e.versions.value[static_cast<std::size_t>(i)] = st.col(5 + i);
    e.rawBytes = static_cast<std::uint64_t>(st.num(10));
    e.storedBytes = static_cast<std::uint64_t>(st.num(11));
    e.created = st.num(12);
    e.lastUsed = st.num(13);
    e.useCount = st.num(14);
    e.warm = st.num(15) != 0;
    e.meta = st.col(16);
    return e;
}

constexpr const char* Schema =
    "CREATE TABLE IF NOT EXISTS meta(k TEXT PRIMARY KEY, v TEXT);"
    "CREATE TABLE IF NOT EXISTS artefact("
    " id INTEGER PRIMARY KEY, kind INTEGER NOT NULL, name TEXT NOT NULL, source_hash TEXT NOT NULL DEFAULT '',"
    " content_hash TEXT NOT NULL, v_recompiler TEXT NOT NULL DEFAULT '', v_mesa TEXT NOT NULL DEFAULT '', v_macos TEXT NOT NULL DEFAULT '',"
    " v_metalc TEXT NOT NULL DEFAULT '', v_gpu TEXT NOT NULL DEFAULT '', raw_bytes INTEGER NOT NULL, stored_bytes INTEGER NOT NULL,"
    " created INTEGER NOT NULL, last_used INTEGER NOT NULL, use_count INTEGER NOT NULL DEFAULT 0, warm INTEGER NOT NULL DEFAULT 0,"
    " meta TEXT NOT NULL DEFAULT '', data BLOB NOT NULL, UNIQUE(kind, name));"
    "CREATE INDEX IF NOT EXISTS artefact_use ON artefact(kind, last_used);"
    "CREATE INDEX IF NOT EXISTS artefact_content ON artefact(content_hash);";

constexpr int SchemaVersion = 1;

// Does an entry recorded with `recorded` need to be dropped when the stages are now `current`?
bool dependsOnChange(Kind kind, const StageVersions& recorded, const StageVersions& current, std::array<bool, StageCount>* changed) {
    const unsigned mask = DependencyMask(kind);
    bool stale = false;
    for (int s = 0; s < StageCount; ++s) {
        if (!(mask & (1u << s))) continue;
        const auto stage = static_cast<Stage>(s);
        if (current[stage].empty()) continue;
        if (recorded[stage] != current[stage]) {
            stale = true;
            if (changed) (*changed)[static_cast<std::size_t>(s)] = true;
        }
    }
    return stale;
}

}

const char* KindName(Kind kind) { return KindNames[static_cast<int>(kind)]; }
bool ParseKind(const std::string& text, Kind& kind) {
    for (int i = 0; i < KindCount; ++i) if (text == KindNames[i]) { kind = static_cast<Kind>(i); return true; }
    return false;
}
const char* StageName(Stage stage) { return StageNames[static_cast<int>(stage)]; }
bool ParseStage(const std::string& text, Stage& stage) {
    for (int i = 0; i < StageCount; ++i) if (text == StageNames[i]) { stage = static_cast<Stage>(i); return true; }
    return false;
}

unsigned DependencyMask(Kind kind) {
    auto bit = [](Stage s) { return 1u << static_cast<int>(s); };
    switch (kind) {
    case Kind::SpirV: return bit(Stage::Recompiler);
    case Kind::Msl: return bit(Stage::Recompiler) | bit(Stage::Mesa) | bit(Stage::Gpu);
    // nps5-prebuild2: a KosmicKrisp build change keeps the Metal-side artefacts. A metallib is a pure function of the MSL text, the MSL options and
    // the Metal compiler: KK names it by a hash of exactly those (plus its own version string), so a KK build that emits other MSL asks for another
    // name and the old one is only trimmed later. A Metal archive entry is looked up by Metal from the pipeline descriptor and the library's own
    // identity and is validated by Metal (another GPU/OS is rejected), so a stale entry misses instead of loading. Only what KK itself writes (the MSL
    // text, the VkPipelineCache blob that holds it) depends on the build.
    case Kind::Metallib: return bit(Stage::MacOs) | bit(Stage::MetalCompiler) | bit(Stage::Gpu);
    case Kind::PipelineArchive: return bit(Stage::Recompiler) | bit(Stage::MacOs) | bit(Stage::Gpu);
    case Kind::VkPipelineCache: return bit(Stage::Recompiler) | bit(Stage::Mesa) | bit(Stage::Gpu);
    }
    return 0;
}

std::string Sha256Hex16(const void* data, std::size_t size) {
    Sha256 sha;
    sha.update(data, size);
    std::uint8_t digest[32];
    sha.final(digest);
    char out[33];
    for (int i = 0; i < 16; ++i) std::snprintf(out + i * 2, 3, "%02x", digest[i]);
    return out;
}
std::string Sha256Hex16(const std::string& text) { return Sha256Hex16(text.data(), text.size()); }

struct Store::Impl {
    sqlite3* db = nullptr;
    mutable std::recursive_mutex mutex;
    bool exec(const char* sql) { return sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK; }
    ~Impl() { if (db) sqlite3_close(db); }
};

Store::~Store() = default;

std::unique_ptr<Store> Store::Open(const std::filesystem::path& file, std::string* error) {
    std::error_code ec;
    if (file.has_parent_path()) std::filesystem::create_directories(file.parent_path(), ec);
    std::unique_ptr<Store> store(new Store);
    store->impl = std::make_unique<Impl>();
    store->path = file;
    if (sqlite3_open_v2(file.string().c_str(), &store->impl->db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK) {
        if (error) *error = store->impl->db ? sqlite3_errmsg(store->impl->db) : "sqlite3_open failed";
        return nullptr;
    }
    sqlite3_busy_timeout(store->impl->db, 10000);
    store->impl->exec("PRAGMA auto_vacuum=INCREMENTAL;PRAGMA journal_mode=WAL;PRAGMA synchronous=NORMAL;");
    if (!store->impl->exec(Schema)) {
        if (error) *error = sqlite3_errmsg(store->impl->db);
        return nullptr;
    }
    // A store written by a newer/other schema is discarded (it is only a cache).
    {
        Statement st(store->impl->db, "SELECT v FROM meta WHERE k='schema'");
        std::string v;
        if (st && sqlite3_step(st.s) == SQLITE_ROW) v = st.col(0);
        if (!v.empty() && v != std::to_string(SchemaVersion)) {
            store->impl->exec("DELETE FROM artefact;DELETE FROM meta;");
        }
        Statement set(store->impl->db, "INSERT OR REPLACE INTO meta(k,v) VALUES('schema',?1)");
        set.text(1, std::to_string(SchemaVersion));
        if (set) sqlite3_step(set.s);
    }
    return store;
}

InvalidationReport Store::Preview(const StageVersions& current) const {
    std::lock_guard lock(impl->mutex);
    InvalidationReport report;
    Statement st(impl->db, (std::string("SELECT ") + EntryColumns + " FROM artefact").c_str());
    if (!st) return report;
    while (sqlite3_step(st.s) == SQLITE_ROW) {
        const auto e = readEntry(st);
        if (dependsOnChange(e.kind, e.versions, current, &report.changed)) ++report.removed[static_cast<std::size_t>(e.kind)];
        else ++report.kept[static_cast<std::size_t>(e.kind)];
    }
    return report;
}

InvalidationReport Store::Invalidate(const StageVersions& current) {
    std::lock_guard lock(impl->mutex);
    InvalidationReport report;
    std::vector<std::int64_t> drop;
    {
        Statement st(impl->db, (std::string("SELECT ") + EntryColumns + " FROM artefact").c_str());
        if (!st) return report;
        while (sqlite3_step(st.s) == SQLITE_ROW) {
            const auto e = readEntry(st);
            if (dependsOnChange(e.kind, e.versions, current, &report.changed)) {
                drop.push_back(e.id);
                ++report.removed[static_cast<std::size_t>(e.kind)];
            } else {
                ++report.kept[static_cast<std::size_t>(e.kind)];
            }
        }
    }
    impl->exec("BEGIN");
    for (const auto id : drop) {
        Statement del(impl->db, "DELETE FROM artefact WHERE id=?1");
        del.i64(1, id);
        if (del) sqlite3_step(del.s);
    }
    impl->exec("COMMIT");
    SetLastSeen(current);
    return report;
}

bool Store::Put(Kind kind, const std::string& name, const void* data, std::size_t size, const std::string& sourceHash,
                const StageVersions& versions, const std::string& meta, int zstdLevel) {
    if (zstdLevel == 0) zstdLevel = kind == Kind::Metallib || kind == Kind::PipelineArchive || kind == Kind::VkPipelineCache ? 6 : 12;
    std::vector<char> packed(ZSTD_compressBound(size));
    std::size_t packedSize = ZSTD_compress(packed.data(), packed.size(), data, size, zstdLevel);
    if (ZSTD_isError(packedSize)) return false;
    packed.resize(packedSize);
    const auto contentHash = Sha256Hex16(data, size);
    const unsigned mask = DependencyMask(kind);
    std::lock_guard lock(impl->mutex);
    Statement st(impl->db,
        "INSERT INTO artefact(kind,name,source_hash,content_hash,v_recompiler,v_mesa,v_macos,v_metalc,v_gpu,raw_bytes,stored_bytes,created,last_used,use_count,warm,meta,data)"
        " VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?12,0,0,?13,?14)"
        " ON CONFLICT(kind,name) DO UPDATE SET source_hash=?3,content_hash=?4,v_recompiler=?5,v_mesa=?6,v_macos=?7,v_metalc=?8,v_gpu=?9,"
        "raw_bytes=?10,stored_bytes=?11,last_used=?12,meta=?13,data=?14");
    if (!st) return false;
    st.i64(1, static_cast<int>(kind));
    st.text(2, name);
    st.text(3, sourceHash);
    st.text(4, contentHash);
    for (int s = 0; s < StageCount; ++s) st.text(5 + s, (mask & (1u << s)) ? versions.value[static_cast<std::size_t>(s)] : std::string());
    st.i64(10, static_cast<std::int64_t>(size));
    st.i64(11, static_cast<std::int64_t>(packedSize));
    st.i64(12, now());
    st.text(13, meta);
    sqlite3_bind_blob64(st.s, 14, packed.data(), packed.size(), SQLITE_TRANSIENT);
    return sqlite3_step(st.s) == SQLITE_DONE;
}

bool Store::Has(Kind kind, const std::string& name) const {
    std::lock_guard lock(impl->mutex);
    Statement st(impl->db, "SELECT 1 FROM artefact WHERE kind=?1 AND name=?2");
    if (!st) return false;
    st.i64(1, static_cast<int>(kind));
    st.text(2, name);
    return sqlite3_step(st.s) == SQLITE_ROW;
}

bool Store::Get(Kind kind, const std::string& name, std::vector<std::byte>& out, bool touch) {
    std::lock_guard lock(impl->mutex);
    bool corrupt = false;
    {
        Statement st(impl->db, "SELECT data,raw_bytes,content_hash FROM artefact WHERE kind=?1 AND name=?2");
        if (!st) return false;
        st.i64(1, static_cast<int>(kind));
        st.text(2, name);
        if (sqlite3_step(st.s) != SQLITE_ROW) return false;
        const auto packedSize = static_cast<std::size_t>(sqlite3_column_bytes(st.s, 0));
        const auto rawSize = static_cast<std::size_t>(st.num(1));
        out.resize(rawSize);
        const auto got = ZSTD_decompress(out.data(), rawSize, sqlite3_column_blob(st.s, 0), packedSize);
        // A truncated or bit-flipped blob can still decompress to the right length: the content hash decides.
        if (ZSTD_isError(got) || got != rawSize || Sha256Hex16(out.data(), out.size()) != st.col(2)) corrupt = true;
    }
    if (corrupt) {
        // Never hand out a damaged artefact; drop the row so the next run rebuilds it.
        out.clear();
        Statement del(impl->db, "DELETE FROM artefact WHERE kind=?1 AND name=?2");
        if (del) {
            del.i64(1, static_cast<int>(kind));
            del.text(2, name);
            sqlite3_step(del.s);
        }
        std::fprintf(stderr, "[gpu-cache] corrupt %s entry '%s' dropped\n", KindName(kind), name.c_str());
        return false;
    }
    if (touch) {
        Statement up(impl->db, "UPDATE artefact SET last_used=?3,use_count=use_count+1,warm=1 WHERE kind=?1 AND name=?2");
        up.i64(1, static_cast<int>(kind));
        up.text(2, name);
        up.i64(3, now());
        if (up) sqlite3_step(up.s);
    }
    return true;
}

bool Store::GetMeta(Kind kind, const std::string& name, std::string& meta) const {
    std::lock_guard lock(impl->mutex);
    Statement st(impl->db, "SELECT meta FROM artefact WHERE kind=?1 AND name=?2");
    if (!st) return false;
    st.i64(1, static_cast<int>(kind));
    st.text(2, name);
    if (sqlite3_step(st.s) != SQLITE_ROW) return false;
    meta = st.col(0);
    return true;
}

bool Store::Remove(Kind kind, const std::string& name) {
    std::lock_guard lock(impl->mutex);
    Statement st(impl->db, "DELETE FROM artefact WHERE kind=?1 AND name=?2");
    if (!st) return false;
    st.i64(1, static_cast<int>(kind));
    st.text(2, name);
    return sqlite3_step(st.s) == SQLITE_DONE && sqlite3_changes(impl->db) > 0;
}

std::vector<Entry> Store::List(int kindFilter, bool prewarmOrder) const {
    std::lock_guard lock(impl->mutex);
    std::string sql = std::string("SELECT ") + EntryColumns + " FROM artefact";
    if (kindFilter >= 0) sql += " WHERE kind=" + std::to_string(kindFilter);
    sql += prewarmOrder ? " ORDER BY warm DESC, last_used DESC, use_count DESC, id" : " ORDER BY kind, id";
    Statement st(impl->db, sql.c_str());
    std::vector<Entry> out;
    if (!st) return out;
    while (sqlite3_step(st.s) == SQLITE_ROW) out.push_back(readEntry(st));
    return out;
}

Stats Store::GetStats() const {
    std::lock_guard lock(impl->mutex);
    Stats stats;
    Statement st(impl->db, "SELECT kind,COUNT(*),SUM(raw_bytes),SUM(stored_bytes) FROM artefact GROUP BY kind");
    if (st) {
        while (sqlite3_step(st.s) == SQLITE_ROW) {
            const auto k = static_cast<std::size_t>(st.num(0));
            if (k >= KindCount) continue;
            stats.count[k] = st.num(1);
            stats.raw[k] = static_cast<std::uint64_t>(st.num(2));
            stats.stored[k] = static_cast<std::uint64_t>(st.num(3));
        }
    }
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (!ec) stats.fileBytes = size;
    return stats;
}

std::int64_t Store::Trim(std::uint64_t maxStoredBytes) {
    std::lock_guard lock(impl->mutex);
    std::uint64_t total = 0;
    for (auto bytes : GetStats().stored) total += bytes;
    if (total <= maxStoredBytes) return 0;
    // Coldest first: not warm, oldest use, fewest uses.
    Statement st(impl->db, "SELECT id,stored_bytes FROM artefact ORDER BY warm, last_used, use_count, id");
    if (!st) return 0;
    std::vector<std::int64_t> drop;
    while (total > maxStoredBytes && sqlite3_step(st.s) == SQLITE_ROW) {
        drop.push_back(st.num(0));
        total -= static_cast<std::uint64_t>(st.num(1));
    }
    impl->exec("BEGIN");
    for (const auto id : drop) {
        Statement del(impl->db, "DELETE FROM artefact WHERE id=?1");
        del.i64(1, id);
        if (del) sqlite3_step(del.s);
    }
    impl->exec("COMMIT");
    return static_cast<std::int64_t>(drop.size());
}

void Store::ClearWarm() {
    std::lock_guard lock(impl->mutex);
    impl->exec("UPDATE artefact SET warm=0");
}

void Store::MarkWarm(Kind kind, const std::string& name) {
    std::lock_guard lock(impl->mutex);
    Statement up(impl->db, "UPDATE artefact SET last_used=?3,use_count=use_count+1,warm=1 WHERE kind=?1 AND name=?2");
    up.i64(1, static_cast<int>(kind));
    up.text(2, name);
    up.i64(3, now());
    if (up) sqlite3_step(up.s);
}

StageVersions Store::LastSeen() const {
    std::lock_guard lock(impl->mutex);
    StageVersions v;
    for (int s = 0; s < StageCount; ++s) {
        Statement st(impl->db, "SELECT v FROM meta WHERE k=?1");
        st.text(1, std::string("stage.") + StageNames[s]);
        if (st && sqlite3_step(st.s) == SQLITE_ROW) v.value[static_cast<std::size_t>(s)] = st.col(0);
    }
    return v;
}

bool Store::SetLastSeen(const StageVersions& versions) {
    std::lock_guard lock(impl->mutex);
    for (int s = 0; s < StageCount; ++s) {
        if (versions.value[static_cast<std::size_t>(s)].empty()) continue;
        Statement st(impl->db, "INSERT OR REPLACE INTO meta(k,v) VALUES(?1,?2)");
        st.text(1, std::string("stage.") + StageNames[s]);
        st.text(2, versions.value[static_cast<std::size_t>(s)]);
        if (st) sqlite3_step(st.s);
    }
    return true;
}

std::size_t Store::Materialize(Kind kind, const std::filesystem::path& dir, const char* extension, std::uint64_t limitBytes, double* decompressMs) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::size_t written = 0;
    std::uint64_t bytes = 0;
    double ms = 0;
    for (const auto& entry : List(static_cast<int>(kind), true)) {
        if (limitBytes != 0 && bytes + entry.rawBytes > limitBytes) continue;
        const auto file = dir / (entry.name + extension);
        std::error_code sizeError;
        if (std::filesystem::file_size(file, sizeError) == entry.rawBytes && !sizeError) { bytes += entry.rawBytes; continue; }
        std::vector<std::byte> data;
        const auto t0 = std::chrono::steady_clock::now();
        if (!Get(kind, entry.name, data, false)) continue;
        ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        const auto tmp = file.string() + ".tmp";
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
            if (!out) { std::filesystem::remove(tmp, ec); continue; }
        }
        std::filesystem::rename(tmp, file, ec);
        if (ec) continue;
        ++written;
        bytes += entry.rawBytes;
    }
    if (decompressMs) *decompressMs = ms;
    return written;
}

std::int64_t Store::ExportBundle(const std::filesystem::path& bundle, const std::string& gpu, std::string* bundleId) {
    std::error_code ec;
    std::filesystem::remove(bundle, ec);
    std::string error;
    auto out = Open(bundle, &error);
    if (!out) return -1;
    std::int64_t count = 0;
    std::string ids;
    for (const auto& entry : List(-1, false)) {
        if (!entry.versions[Stage::Gpu].empty() && !gpu.empty() && entry.versions[Stage::Gpu] != gpu) continue;
        std::vector<std::byte> data;
        if (!Get(entry.kind, entry.name, data, false)) continue;
        if (!out->Put(entry.kind, entry.name, data.data(), data.size(), entry.sourceHash, entry.versions, entry.meta)) continue;
        ids += entry.contentHash;
        ++count;
    }
    out->SetLastSeen(LastSeen());
    out->Checkpoint();
    if (bundleId) *bundleId = Sha256Hex16(ids);
    return count;
}

std::int64_t Store::ImportBundle(const std::filesystem::path& bundle) {
    std::string error;
    auto in = Open(bundle, &error);
    if (!in) return -1;
    std::int64_t count = 0;
    std::set<std::string> known;
    for (const auto& e : List(-1, false)) known.insert(std::string(KindName(e.kind)) + ":" + e.name + ":" + e.contentHash);
    for (const auto& entry : in->List(-1, false)) {
        if (known.count(std::string(KindName(entry.kind)) + ":" + entry.name + ":" + entry.contentHash)) continue;
        std::vector<std::byte> data;
        if (!in->Get(entry.kind, entry.name, data, false)) continue;
        if (Put(entry.kind, entry.name, data.data(), data.size(), entry.sourceHash, entry.versions, entry.meta)) ++count;
    }
    return count;
}

void Store::Checkpoint() {
    std::lock_guard lock(impl->mutex);
    impl->exec("PRAGMA incremental_vacuum;PRAGMA wal_checkpoint(TRUNCATE);");
}

}
