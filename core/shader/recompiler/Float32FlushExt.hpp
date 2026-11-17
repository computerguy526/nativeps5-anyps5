#ifndef CORE_SHADER_RECOMPILER_FLOAT32FLUSHEXT_HPP
#define CORE_SHADER_RECOMPILER_FLOAT32FLUSHEXT_HPP

#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace ShaderRecompiler {

// NPS5_HW_F32_FLUSH_EXT (process-wide, default off) extends the hardware f32 denormal flush (float32FlushToZero) from
// add/sub/mul/fma to more instruction classes by dropping their software flush. "1" enables every class below; a comma
// list ("cmp,cvt,round") enables some. Only takes effect where the target flushes in hardware, which the device probe
// validates. See research/shader-quality.md ("Hardware flush extension") for the per-class semantics.
constexpr std::uint32_t Float32FlushExtCompare = 1u;  // v_cmp*_f32 / v_cmpx*_f32: the device compare treats a denormal as zero
constexpr std::uint32_t Float32FlushExtConvert = 2u;  // converts whose result is the same for a denormal and for a signed zero
constexpr std::uint32_t Float32FlushExtRound = 4u;    // v_fract_f32 / v_floor_f32 / v_ceil_f32: the device flushes the input

inline std::uint32_t Float32FlushExtMask() {
    static const std::uint32_t mask = [] {
        const char* value = std::getenv("NPS5_HW_F32_FLUSH_EXT");
        if (value == nullptr || value[0] == '\0' || std::strcmp(value, "0") == 0) return 0u;
        if (std::strcmp(value, "1") == 0) return Float32FlushExtCompare | Float32FlushExtConvert | Float32FlushExtRound;
        return (std::strstr(value, "cmp") != nullptr ? Float32FlushExtCompare : 0u) | (std::strstr(value, "cvt") != nullptr ? Float32FlushExtConvert : 0u) | (std::strstr(value, "round") != nullptr ? Float32FlushExtRound : 0u);
    }();
    return mask;
}

}

#endif
