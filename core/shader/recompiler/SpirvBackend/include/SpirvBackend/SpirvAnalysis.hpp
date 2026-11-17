#ifndef CORE_SHADER_RECOMPILIER_SPIRVBACKEND_INCLUDE_SPIRVBACKEND_SPIRVANALYSIS_HPP
#define CORE_SHADER_RECOMPILIER_SPIRVBACKEND_INCLUDE_SPIRVBACKEND_SPIRVANALYSIS_HPP

#include "IntermediateRepresentation/IrProgram.hpp"
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ShaderRecompiler {

struct SpirvRequirements {
    bool subgroupBallot = false;
    bool subgroupShuffle = false;
    bool subgroupLocalInvocationId = false;
    bool computeDerivatives = false;
    bool neighbourLaneReads = false;
    bool offsetBarycentricReads = false;
    bool imageGatherExtended = false;
    bool functionLds = false;
    bool ldsLock = false;
    bool ldsBufferInt64Atomics = false;
    std::uint32_t functionLdsDwords = 0;
    std::unordered_map<const IrValue*, std::uint32_t> functionLdsAddresses;
    bool functionScratch = false;
    bool pixelValidMask = false;
    bool bufferInt64Atomics = false;
    bool bufferInt64AtomicsExact = false;
    bool bufferInt64AtomicsSplit = false;
    // Every 64-bit buffer atomic of the program is a plain 64-bit load or store in disguise (IsPlainBufferAtomic64).
    bool bufferInt64AtomicsPlainOnly = false;
    // Set by the emitter on a device without shaderBufferInt64Atomics: those atomics compile to one aligned 64-bit load or store.
    bool bufferInt64AtomicsPlain = false;
    bool coherentBufferPairs = false;
    bool bufferU64View = false;
    bool imageInt64Atomics = false;
    bool gdsInt64Atomics = false;
    bool sharedInt64Atomics = false;
    bool float64 = false;
    bool float64NeedsDevice = false;
    bool float64Interpolation = false;
    bool coherentBuffers = false;
    bool coherentBufferAlias = false;
    bool readOnlyStorage = false;
    std::vector<std::uint32_t> capabilities;
    std::vector<std::string> extensions;
};

[[nodiscard]] bool IsSplittableBufferAtomic64(const IrValue* inst);
// A 64-bit buffer atomic that only reads the word (or/xor/add of a zero operand on every active lane) or only writes it
// (an exchange whose result no instruction reads).
[[nodiscard]] bool IsPlainLoadBufferAtomic64(const IrValue* inst);
[[nodiscard]] bool IsPlainStoreBufferAtomic64(const IrValue* inst);
[[nodiscard]] SpirvRequirements AnalyzeProgramRequirements(const IrProgram& program);
[[nodiscard]] bool IsIntegerFloat64Opcode(IrOpcode opcode);
inline constexpr std::uint32_t FunctionLdsDwordLimit = 8192u;
inline constexpr std::uint32_t PixelFunctionStorageDwords = 4096u;
inline constexpr std::uint32_t PixelFunctionLdsDescriptorSet = 2u;
inline constexpr std::uint32_t MeshWindowDescriptorSet = 3u;
inline constexpr std::uint32_t MeshWindowSlots = 256u;
inline constexpr std::uint32_t MeshWindowHeaderDwords = 4u + MeshWindowSlots;
inline constexpr std::uint32_t MeshWindowSlotDwords = 65536u;
[[nodiscard]] std::uint32_t FunctionLdsDwords(const IrProgram& program);
[[nodiscard]] std::unordered_map<const IrValue*, std::uint32_t> FunctionLdsLaneAddresses(const IrProgram& program, std::uint32_t dwords);
[[nodiscard]] std::unordered_set<const IrValue*> WaveUniformValues(const IrProgram& program);
[[nodiscard]] bool IsWaveMaskBranch(BranchCondition condition);

}

#endif
