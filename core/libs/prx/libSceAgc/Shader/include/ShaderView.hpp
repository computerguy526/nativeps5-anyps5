#ifndef CORE_LIBS_PRX_LIBSCEAGC_SHADER_INCLUDE_SHADERVIEW_HPP
#define CORE_LIBS_PRX_LIBSCEAGC_SHADER_INCLUDE_SHADERVIEW_HPP

#include <cstdint>
#include "SceShaders.hpp"
#include "prx/libSceAgc/include/GuestPtr.hpp"

namespace Agc::Guest {

// A host-pointer view of a Shader header that lives in guest memory. The header's pointer fields (user_data, code,
// cx_registers, sh_registers, specials, input_semantics, output_semantics and the ShaderUserData tables) hold guest
// values; the view copies the header and rewrites those fields to host pointers so the library can read the shader
// through ordinary pointers. It is a snapshot: write through the original header, not the view. `guest()` is the value
// the driver expects for a Shader* (AgcDriver*Shader*, ShaderPreparationScope).
class ShaderView {
public:
    explicit ShaderView(const Shader* host) : original(host) {
        if (host == nullptr) return;
        copy = *host;
        copy.user_data = Host(copy.user_data);
        copy.code = Host(copy.code);
        copy.cx_registers = Host(copy.cx_registers);
        copy.sh_registers = Host(copy.sh_registers);
        copy.specials = Host(copy.specials);
        copy.input_semantics = Host(copy.input_semantics);
        copy.output_semantics = Host(copy.output_semantics);
        if (copy.user_data != nullptr) {
            userData = *copy.user_data;
            userData.direct_resource_offset = Host(userData.direct_resource_offset);
            for (auto& table : userData.sharp_resource_offset) table = Host(table);
            copy.user_data = &userData;
        }
    }
    ShaderView(const ShaderView&) = delete;
    ShaderView& operator=(const ShaderView&) = delete;

    // Null when the original was null.
    const Shader* get() const { return original == nullptr ? nullptr : &copy; }
    const Shader* operator->() const { return &copy; }
    // The driver-facing (guest-valued) Shader pointer.
    const Shader* guest() const { return Guest(original); }
    const Shader* host() const { return original; }

private:
    const Shader* original;
    Shader copy{};
    ShaderUserData userData{};
};

}

#endif
