#include "prx/libSceAgc/Shader/include/PrimState.hpp"
#include "prx/libSceAgc/Shader/include/LinkShaders.hpp"
#include "prx/libSceAgc/Shader/include/InterpolantMapping.hpp"
#include "SceShaders.hpp"
#include "prx/libSceAgc/Shader/include/ShaderView.hpp"
#include "prx/libSceAgcDriver/Execution/include/ShaderPreparationScope.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libc/include/general/VabiMacros.hpp"
#include "prx/libSceAgc/Shader/include/ShaderConstants.hpp"
#include <array>
#include <algorithm>

extern "C" int APS5_VABI sceAgcLinkShaders(ShaderRegister* context, ShaderRegister* primitive,
    const void* reserved, const Shader* vertex, const Shader* pixel, std::uint32_t primitiveType) {
    using namespace ShaderRegs;
    // `vertex`/`pixel` are host pointers to headers whose fields are guest values (ShaderView); the driver takes the
    // guest-valued Shader pointers.
    const Agc::Guest::ShaderView vertexView(vertex), pixelView(pixel);
    if (!context || !primitive || reserved || !vertex ||
        vertexView->type != static_cast<std::uint8_t>(ShaderBinaryType::Gs) ||
        (vertexView->num_output_semantics && !vertexView->output_semantics) ||
        (pixel && (pixelView->type != static_cast<std::uint8_t>(ShaderBinaryType::Ps) ||
                   pixelView->num_input_semantics > 32 ||
                   (pixelView->num_input_semantics && !pixelView->input_semantics))))
        return GRAPHICS5_ERROR_INVALID_SHADER_PROGRAM;

    // The simple vertex/pixel pipeline combines two primitive context registers
    // and 32 interpolant registers. Publish only after both helpers succeed.
    AgcDriver::ShaderPreparationScope transaction({vertexView.guest(), pixelView.guest()});
    std::array<ShaderRegister, 34> contextValues{};
    std::array<ShaderRegister, 3> primitiveValues{};
    sceAgcCreatePrimState(contextValues.data(), primitiveValues.data(), nullptr, vertex, primitiveType);
    sceAgcCreateInterpolantMapping_0100(contextValues.data() + 2, vertex, pixel);
    if (pixel != nullptr) {
        const std::array<const Shader*, 2> stages{vertexView.guest(), pixelView.guest()};
        AgcDriverResolveGraphicsStagesAbi_nid_postfix(stages, contextValues, primitiveValues);
    }
    AgcDriverResolveGraphicsAbi_nid_postfix(vertexView.guest(), pixelView.guest(), primitiveType);
    transaction.Commit();
    std::copy(contextValues.begin(), contextValues.end(), context);
    std::copy(primitiveValues.begin(), primitiveValues.end(), primitive);
    return 0;
}
