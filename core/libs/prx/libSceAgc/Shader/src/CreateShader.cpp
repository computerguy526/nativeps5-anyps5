#include "prx/libSceAgc/Shader/include/CreateShader.hpp"

#include <cstdio>
#include <stdexcept>
#include <prx/libc/include/General.hpp>

#include "SceShaders.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgc/include/GuestPtr.hpp"
#include "prx/libSceAgc/Shader/include/ShaderUtils.hpp"
#include "prx/libSceAgc/Shader/include/ShaderConstants.hpp"

extern "C" {

int APS5_VABI sceAgcCreateShader(Shader** dst, void* header, const volatile void* code) {
    constexpr auto fn = __func__;
    if (dst == nullptr) {
        throw std::runtime_error(std::string(fn) + ": dst is null");
    }
    if (header == nullptr) {
        throw std::runtime_error(std::string(fn) + ": header is null");
    }
    if (code == nullptr) {
        throw std::runtime_error(std::string(fn) + ": code is null");
    }

    auto* h = static_cast<Shader*>(header);

    if (h->file_header != ShaderRegs::SHADER_FILE_HEADER_MAGIC || h->version != ShaderRegs::SHADER_VERSION) {
        throw std::runtime_error(std::string(fn) + ": invalid shader header or version");
    }
    const auto base = Agc::Guest::Address(code);
    if ((base & ShaderRegs::SHADER_BASE_ALIGN_MASK) != 0 || h->shader_size == 0 || (h->shader_size & 3u) != 0) {
        throw std::runtime_error(std::string(fn) + ": invalid shader code address or size");
    }
    std::uint32_t programOffset = 0;
    (void)GetProgramAddressRegisterOffset(h->type, programOffset);

    ResolveRelativePtr(h->cx_registers);
    ResolveRelativePtr(h->sh_registers);
    ResolveRelativePtr(h->user_data);
    ResolveRelativePtr(h->specials);
    ResolveRelativePtr(h->input_semantics);
    ResolveRelativePtr(h->output_semantics);

    // The header's pointer fields are now guest values; reach the pointees through the host mapping.
    if (h->user_data != nullptr) {
        auto* const userData = Agc::Guest::Host(h->user_data);
        ResolveRelativePtr(userData->direct_resource_offset);
        ResolveRelativePtr(userData->sharp_resource_offset[0]);
        ResolveRelativePtr(userData->sharp_resource_offset[1]);
        ResolveRelativePtr(userData->sharp_resource_offset[2]);
        ResolveRelativePtr(userData->sharp_resource_offset[3]);
    }

    h->code = Agc::Guest::Guest(code);

    int result = PatchProgramAddressRegister(Agc::Guest::Host(h->sh_registers), h->num_sh_registers, h->type, base);
    if (result != 0) {
        return result;
    }

    // The driver and the title know the header by its guest address.
    AgcDriverRegisterShader_nid_postfix(Agc::Guest::Guest(h));
    *dst = Agc::Guest::Guest(h);
    return 0;
}

}
