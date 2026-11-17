#include "prx/libSceAgcDriver/Graphics/include/PlaceholderTexel.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace AgcDriver::Graphics {
namespace {

PlaceholderPattern make(std::initializer_list<std::uint8_t> values) {
    PlaceholderPattern pattern;
    for (const auto value : values) pattern.bytes[pattern.size++] = value;
    return pattern;
}

PlaceholderPattern word(std::uint32_t value) {
    return make({static_cast<std::uint8_t>(value), static_cast<std::uint8_t>(value >> 8), static_cast<std::uint8_t>(value >> 16), static_cast<std::uint8_t>(value >> 24)});
}

void append(PlaceholderPattern& pattern, const PlaceholderPattern& more) {
    for (std::uint32_t i = 0; i < more.size; ++i) pattern.bytes[pattern.size++] = more.bytes[i];
}

const PlaceholderPattern Bc1 = make({0xEF, 0x7B, 0xEF, 0x7B, 0, 0, 0, 0});  // both endpoints R5G6B5 grey, every index 0
const PlaceholderPattern Bc4 = make({0x80, 0x80, 0, 0, 0, 0, 0, 0});
const PlaceholderPattern Bc3Alpha = make({0xFF, 0xFF, 0, 0, 0, 0, 0, 0});

PlaceholderPattern bc7() {
    // Mode 6: mode bits 0b1000000 (bit 6), R0 R1 G0 G1 B0 B1 A0 A1 (7 bits each), P0 P1, then 63 index bits (all 0: endpoint 0).
    unsigned __int128 block = 0;
    unsigned bit = 0;
    const auto put = [&](std::uint64_t value, unsigned width) {
        block |= static_cast<unsigned __int128>(value) << bit;
        bit += width;
    };
    put(1u << 6, 7);
    for (int i = 0; i < 6; ++i) put(0x40, 7);  // R0 R1 G0 G1 B0 B1: 0x40 with p-bit 1 -> 129
    put(0x7F, 7);
    put(0x7F, 7);  // A0 A1: 255 with p-bit 1
    put(1, 1);
    put(1, 1);
    PlaceholderPattern pattern;
    for (int i = 0; i < 16; ++i) pattern.bytes[pattern.size++] = static_cast<std::uint8_t>(block >> (8 * i));
    return pattern;
}

bool envOn(const char* name, bool fallback) {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') return fallback;
    return *value != '0';
}

}

PlaceholderPattern PlaceholderPatternFor(std::uint32_t format) {
    switch (format) {
        case 1: return make({0x80});
        case 128: return make({0xBC});
        case 7: return make({0x00, 0x80});
        case 13: return make({0x00, 0x38});
        case 14: return make({0x80, 0x80});
        case 129: return make({0xBC, 0xBC});
        case 22: return word(0x3F000000u);
        case 23: return word(0x80008000u);
        case 29: return word(0x38003800u);
        case 36: return word(0x380u | (0x380u << 11) | (0x1C0u << 22));
        case 50: return word(512u | (512u << 10) | (512u << 20) | (3u << 30));
        case 56: return make({0x80, 0x80, 0x80, 0xFF});
        case 130: return make({0xBC, 0xBC, 0xBC, 0xFF});
        case 64: { auto p = word(0x3F000000u); append(p, p); return p; }
        case 65: return make({0, 0x80, 0, 0x80, 0, 0x80, 0xFF, 0xFF});
        case 71: return make({0, 0x38, 0, 0x38, 0, 0x38, 0, 0x3C});
        case 74: { auto h = word(0x3F000000u); auto p = h; append(p, h); append(p, h); return p; }
        case 77: { auto h = word(0x3F000000u); auto p = h; append(p, h); append(p, h); append(p, word(0x3F800000u)); return p; }
        case 132: return word(256u | (256u << 9) | (256u << 18) | (15u << 27));
        case 133: return make({0xEF, 0x7B});
        case 134: return make({0xEF, 0xBD});
        case 136: return make({0x88, 0xF8});
        case 169: case 170: return Bc1;
        case 171: case 172: { auto p = make({0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}); append(p, Bc1); return p; }
        case 173: case 174: { auto p = Bc3Alpha; append(p, Bc1); return p; }
        case 175: return Bc4;
        case 177: { auto p = Bc4; append(p, Bc4); return p; }
        case 181: case 182: return bc7();
        default: return {};
    }
}

bool AllZero(std::span<const std::byte> bytes) {
    const auto* data = bytes.data();
    std::size_t i = 0;
    for (; i + 8 <= bytes.size(); i += 8) {
        std::uint64_t word;
        std::memcpy(&word, data + i, 8);
        if (word != 0) return false;
    }
    for (; i < bytes.size(); ++i) {
        if (data[i] != std::byte{0}) return false;
    }
    return true;
}

void FillPattern(std::span<std::byte> bytes, const PlaceholderPattern& pattern) {
    if (pattern.size == 0) return;
    for (std::size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<std::byte>(pattern.bytes[i % pattern.size]);
}

bool PlaceholderTexturesEnabled() {
    static const bool enabled = envOn("APS5_PLACEHOLDER_TEX", true);
    return enabled;
}

bool PlaceholderStatsEnabled() {
    static const bool enabled = envOn("APS5_PLACEHOLDER_STATS", false);
    return enabled;
}

std::uint64_t DelayTexturesPresents() {
    static const std::uint64_t presents = [] {
        const char* value = std::getenv("APS5_DELAY_TEXTURES");
        return value != nullptr ? static_cast<std::uint64_t>(std::strtoull(value, nullptr, 10)) : 0ull;
    }();
    return presents;
}

PlaceholderCounters& PlaceholderStats() {
    static PlaceholderCounters counters;
    return counters;
}

void PlaceholderNote(const char* what, std::uint64_t count, const char* detail) {
    if (!PlaceholderStatsEnabled() || count == 0 || (count & (count - 1)) != 0) return;
    const auto& c = PlaceholderStats();
    std::fprintf(stderr, "[placeholder] %s #%llu %s | created %llu zero-unwritten %llu zero-written %llu substituted %llu no-pattern %llu delayed %llu absent sampled %llu storage %llu, storage images made %llu\n",
                 what, static_cast<unsigned long long>(count), detail, static_cast<unsigned long long>(c.created.load()), static_cast<unsigned long long>(c.zeroUnwritten.load()),
                 static_cast<unsigned long long>(c.zeroWritten.load()), static_cast<unsigned long long>(c.substituted.load()), static_cast<unsigned long long>(c.noPattern.load()),
                 static_cast<unsigned long long>(c.delayed.load()), static_cast<unsigned long long>(c.absentSampled.load()), static_cast<unsigned long long>(c.absentStorage.load()),
                 static_cast<unsigned long long>(c.storageCreated.load()));
}

}
