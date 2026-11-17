#ifndef CORE_LIBS_PRX_LIBSCEAGC_INCLUDE_GUESTPTR_HPP
#define CORE_LIBS_PRX_LIBSCEAGC_INCLUDE_GUESTPTR_HPP

// Guest / host pointer conversion for the guest-side command-buffer builder (docs/nps5-gpu-addr.md).
//
// Convention in libSceAgc (the same split the driver uses, `host = guest + bias`, bias set by AgcSetGuestBias):
//   * a pointer *parameter* of an exported function is a host pointer (the host runtime's thunk translated it);
//     the library dereferences it directly;
//   * a pointer that the library *encodes into GPU-visible data* (PM4 words, Shader header fields, the cursors of a
//     CommandBuffer, a value stored through an out-parameter such as sceAgcCreateShader's `dst`) is a guest value:
//     the library converts with Guest()/Address();
//   * a pointer *read out of* GPU-visible data (CommandBuffer cursors, Shader header fields) is a guest value and
//     is dereferenced only after Host();
//   * Shader pointers handed to the driver (AgcDriver*Shader*, ShaderPreparationScope) are guest values.
// With bias 0 everything below is the identity.

#include <cstdint>

extern "C" std::int64_t AgcGetGuestBias();

namespace Agc::Guest {

// Guest value -> host pointer. NULL is preserved.
template <typename T>
inline T* Host(T* guest) {
    if (guest == nullptr) return nullptr;
    return reinterpret_cast<T*>(reinterpret_cast<std::uintptr_t>(guest) + static_cast<std::uintptr_t>(AgcGetGuestBias()));
}

// Host pointer -> guest value (typed). NULL is preserved.
template <typename T>
inline T* Guest(T* host) {
    if (host == nullptr) return nullptr;
    return reinterpret_cast<T*>(reinterpret_cast<std::uintptr_t>(host) - static_cast<std::uintptr_t>(AgcGetGuestBias()));
}

// Host pointer -> guest address, the form PM4 words carry.
inline std::uint64_t Address(const volatile void* host) {
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(Guest(host)));
}

}

#endif
