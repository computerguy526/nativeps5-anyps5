#include "prx/libSceAgc/Command/include/RegisterDefaults.hpp"
#include "prx/libSceAgc/Command/include/Packet.hpp"
#include "prx/libSceAgc/include/GuestPtr.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestAddr.hpp"
#include <memory>
#include <new>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <vector>

namespace Agc::Command {

namespace {

struct TypeIndex {
    std::uint32_t key;
    std::uint32_t bankIndex;
};

static_assert(sizeof(TypeIndex) == 8);

struct RegisterDefaults {
    ShaderRegister** tables[4];
    std::uint32_t registerCounts[4];
    void* types;
    std::uint32_t count;
};

static_assert(offsetof(RegisterDefaults, types) == 0x30);
static_assert(offsetof(RegisterDefaults, count) == 0x38);

struct CompactBank {
    ShaderRegister* registers;
    std::uint32_t registerCount;
    const std::uint16_t* offsets;
    std::uint32_t pointerCount;
};

struct CompactRegisterDefaults {
    CompactBank banks[4];
    std::uint32_t* types;
    std::uint32_t count;
};

#include "prx/libSceAgc/Command/include/RegisterDefaultsData.hpp"

// The defaults tables are read by the title and their register banks are named by GPU-visible indirect register
// packets, so the whole structure (header, banks, pointer tables, types) is built in one driver scratch block that has a
// guest address; every pointer stored in it is a guest value. `defaults` is the host pointer of the header in the block.
struct Storage {
    // Never freed: the tables live for the process (and a destructor running during static teardown would touch the
    // scratch allocator after it is gone).
    AgcDriver::GuestAddr::Scratch* block = nullptr;
    RegisterDefaults* defaults = nullptr;
    bool initialized = false;
};

std::mutex defaultsMutex;
std::array<std::array<std::array<Storage, 14>, 2>, 2> storage;

struct AgcContextState {
    std::uint32_t data[512];
};

static_assert(sizeof(AgcContextState) == 2048);

std::mutex defaultStateMutex;
AgcContextState defaultState{};
bool defaultStateInitialized = false;

void InitDefaultCxState() {
    std::memset(&defaultState, 0, sizeof(defaultState));
    const auto* source = agcPublicRegDefaultsByVersion[8];
    for (std::size_t bank = 0; bank < 4; ++bank) {
        const auto& b = source->banks[bank];
        if (b.registers == nullptr) continue;
        for (std::uint32_t r = 0; r < b.registerCount; ++r) {
            const auto offset = b.registers[r].offset;
            const auto value = b.registers[r].value;
            if (offset < 512) {
                defaultState.data[offset] = value;
            }
        }
    }
    defaultStateInitialized = true;
}

}

void* GetRegisterDefaults(std::uint32_t version, bool internal, const char* function,
                          RegisterDefaultsTypeLayout typeLayout) {
    Require(version < 14, function, "unsupported register defaults version");
    const auto* source = internal ? agcInternalRegDefaultsByVersion[version] : agcPublicRegDefaultsByVersion[version];
    std::lock_guard lock(defaultsMutex);
    const auto layoutIndex = typeLayout == RegisterDefaultsTypeLayout::KeyBankPairs ? 1 : 0;
    auto& destination = storage[internal ? 1 : 0][layoutIndex][version];
    if (!destination.initialized) {
        const auto alignUp = [](std::size_t value) { return (value + 15u) & ~std::size_t{15}; };
        std::size_t bytes = alignUp(sizeof(RegisterDefaults));
        for (const auto& input : source->banks) {
            Require(input.pointerCount == 0 || (input.registers != nullptr && input.offsets != nullptr), function, "invalid register defaults bank");
            bytes += alignUp(std::size_t{input.registerCount} * sizeof(ShaderRegister)) + alignUp(std::size_t{input.pointerCount} * sizeof(ShaderRegister*));
        }
        Require(source->count == 0 || source->types != nullptr, function, "missing register defaults types");
        bytes += alignUp(std::size_t{source->count} * 3u * sizeof(std::uint32_t));
        auto* const block = new AgcDriver::GuestAddr::Scratch(bytes, 4096);
        auto* const base = static_cast<std::uint8_t*>(block->data());
        std::size_t cursor = 0;
        const auto take = [&](std::size_t size) {
            auto* const at = base + cursor;
            cursor += alignUp(size);
            return at;
        };
        auto* const defaults = new (take(sizeof(RegisterDefaults))) RegisterDefaults{};
        for (std::size_t bank = 0; bank < 4; ++bank) {
            const auto& input = source->banks[bank];
            auto* const registers = reinterpret_cast<ShaderRegister*>(take(std::size_t{input.registerCount} * sizeof(ShaderRegister)));
            if (input.registerCount != 0) std::memcpy(registers, input.registers, std::size_t{input.registerCount} * sizeof(ShaderRegister));
            auto* const table = reinterpret_cast<ShaderRegister**>(take(std::size_t{input.pointerCount} * sizeof(ShaderRegister*)));
            for (std::uint32_t index = 0; index < input.pointerCount; ++index) {
                Require(input.offsets[index] < input.registerCount, function, "register defaults pointer exceeds bank");
                table[index] = Agc::Guest::Guest(registers + input.offsets[index]);
            }
            defaults->tables[bank] = input.pointerCount == 0 ? nullptr : Agc::Guest::Guest(table);
            defaults->registerCounts[bank] = input.registerCount;
        }
        auto* const types = reinterpret_cast<std::uint32_t*>(take(std::size_t{source->count} * 3u * sizeof(std::uint32_t)));
        if (typeLayout == RegisterDefaultsTypeLayout::KeyBankPairs) {
            auto* const pairs = reinterpret_cast<TypeIndex*>(types);
            for (std::uint32_t entry = 0; entry < source->count; ++entry) {
                const std::uint32_t bankIndex = source->types[entry * 3 + 1] & 0x3ffu;
                Require((bankIndex >> 2) < source->banks[bankIndex & 3u].pointerCount, function, "register defaults type exceeds its bank");
                pairs[entry] = {source->types[entry * 3], bankIndex};
            }
        } else if (source->count != 0) {
            std::memcpy(types, source->types, std::size_t{source->count} * 3u * sizeof(std::uint32_t));
        }
        defaults->types = source->count == 0 ? nullptr : Agc::Guest::Guest(static_cast<void*>(types));
        defaults->count = source->count;
        destination.block = block;
        destination.defaults = defaults;
        destination.initialized = true;
    }
    return destination.defaults;
}

int GetDefaultCxStateFlat(void* destination, std::uint32_t size) {
    if (destination == nullptr) return static_cast<int>(0x80890001);
    std::lock_guard lock(defaultStateMutex);
    if (!defaultStateInitialized) {
        InitDefaultCxState();
    }
    const auto copyBytes = std::min(size, static_cast<std::uint32_t>(sizeof(AgcContextState)));
    std::memcpy(destination, defaultState.data, copyBytes);
    return 0;
}

}
