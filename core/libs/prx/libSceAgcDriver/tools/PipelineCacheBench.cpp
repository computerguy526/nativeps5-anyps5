// nps5: creates real graphics pipelines from recompiled SPIR-V pairs on the Vulkan driver, through the driver's own
// PipelineCache (device + driver UUID + shader-source-version keyed file) and ConfigureDriverPipelineArchive
// (KosmicKrisp Metal archive), and prints per-pipeline times. Run it twice in separate processes: the first is
// the cold launch, the second the warm launch. No guest execution, no shader recompile.
//
//   ANYPS5_SHADER_CACHE_DIR=<dir> nps5_pipe_bench [--prewarm] <vs.spv> <fs.spv> [<vs.spv> <fs.spv> ...]
//
// --prewarm builds the pairs on a background thread while the main thread "renders" (a 1 ms loop); it reports the
// longest main-thread frame gap so a prewarm that blocks the render thread shows up as a stall.
#include <vulkan/vulkan.h>
#include "prx/libSceAgcDriver/Graphics/include/PipelineCache.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>
#include <atomic>
#include <memory>

namespace AgcDriver::Graphics {
std::uint64_t& DeviceProcLookups() {
    thread_local std::uint64_t count = 0;
    return count;
}
}

namespace {

using Clock = std::chrono::steady_clock;
double msSince(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

#define CK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { std::fprintf(stderr, "%s failed (%d) line %d\n", #x, static_cast<int>(r_), __LINE__); std::exit(1); } } while (0)

std::vector<char> readFile(const char* path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) { std::fprintf(stderr, "cannot read %s\n", path); std::exit(2); }
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

VkShaderModule makeModule(VkDevice device, const char* path) {
    const auto bytes = readFile(path);
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = bytes.size();
    info.pCode = reinterpret_cast<const std::uint32_t*>(bytes.data());
    VkShaderModule module;
    CK(vkCreateShaderModule(device, &info, nullptr, &module));
    return module;
}

}

int main(int argc, char** argv) {
    bool prewarm = false;
    std::vector<const char*> files;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--prewarm") == 0) prewarm = true;
        else files.push_back(argv[i]);
    }
    if (files.empty() || files.size() % 2 != 0) {
        std::fprintf(stderr, "usage: nps5_pipe_bench [--prewarm] <vs.spv> <fs.spv> ...\n");
        return 2;
    }
    const auto processStart = Clock::now();
    const char* instanceExtensions[] = {"VK_KHR_portability_enumeration"};
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "nps5_pipe_bench";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instanceInfo.flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    instanceInfo.pApplicationInfo = &app;
    instanceInfo.enabledExtensionCount = 1;
    instanceInfo.ppEnabledExtensionNames = instanceExtensions;
    VkInstance instance;
    CK(vkCreateInstance(&instanceInfo, nullptr, &instance));
    std::uint32_t count = 1;
    VkPhysicalDevice physical;
    vkEnumeratePhysicalDevices(instance, &count, &physical);
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(physical, &properties);
    VkPhysicalDeviceDriverProperties driver{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 properties2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &driver};
    vkGetPhysicalDeviceProperties2(physical, &properties2);
    const std::string driverInfo = std::string(driver.driverName) + " " + driver.driverInfo;

    // Same call VulkanDevice makes right before vkCreateDevice.
    AgcDriver::Graphics::ConfigureDriverPipelineArchive(properties, driverInfo.c_str());

    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    f12.bufferDeviceAddress = VK_TRUE; f12.storageBuffer8BitAccess = VK_TRUE; f12.shaderInt8 = VK_TRUE; f12.shaderFloat16 = VK_TRUE;
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, &f12};
    f13.dynamicRendering = VK_TRUE;
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f13};
    f2.features.shaderInt64 = f2.features.shaderClipDistance = f2.features.vertexPipelineStoresAndAtomics = f2.features.fragmentStoresAndAtomics = f2.features.shaderInt16 = VK_TRUE;
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;
    VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f2};
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    const auto deviceStart = Clock::now();
    VkDevice device;
    CK(vkCreateDevice(physical, &deviceInfo, nullptr, &device));
    const double deviceMs = msSince(deviceStart);

    AgcDriver::Graphics::Context context{};
    context.device = device;
    context.physical = physical;
    context.deviceProc = vkGetDeviceProcAddr;
    const auto cacheStart = Clock::now();
    auto cache = std::make_unique<AgcDriver::Graphics::PipelineCache>(context, properties, true);
    const double cacheMs = msSince(cacheStart);
    context.pipelineCache = cache->Handle();

    VkDescriptorSetLayoutBinding bindings[2]{{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}, {61, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    setInfo.bindingCount = 2;
    setInfo.pBindings = bindings;
    VkDescriptorSetLayout setLayout;
    CK(vkCreateDescriptorSetLayout(device, &setInfo, nullptr, &setLayout));
    VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 128};
    VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layoutInfo.setLayoutCount = 1; layoutInfo.pSetLayouts = &setLayout; layoutInfo.pushConstantRangeCount = 1; layoutInfo.pPushConstantRanges = &push;
    VkPipelineLayout layout;
    CK(vkCreatePipelineLayout(device, &layoutInfo, nullptr, &layout));

    const std::size_t pairs = files.size() / 2;
    std::vector<double> times(pairs, 0.0);
    const auto buildAll = [&] {
        for (std::size_t p = 0; p < pairs; ++p) {
            const auto start = Clock::now();
            VkShaderModule vs = makeModule(device, files[2 * p]), fs = makeModule(device, files[2 * p + 1]);
            VkPipelineShaderStageCreateInfo stages[2]{{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", nullptr}, {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", nullptr}};
            VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
            VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
            ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
            vp.viewportCount = 1; vp.scissorCount = 1;
            VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
            rs.lineWidth = 1.0f;
            VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
            ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
            VkPipelineColorBlendAttachmentState blend{};
            blend.blendEnable = VK_TRUE; blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA; blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA; blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE; blend.colorWriteMask = 15;
            VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
            cb.attachmentCount = 1; cb.pAttachments = &blend;
            VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
            const VkDynamicState dynamic[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
            VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
            dyn.dynamicStateCount = 2; dyn.pDynamicStates = dynamic;
            const VkFormat format = VK_FORMAT_B8G8R8A8_UNORM;
            VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
            rendering.colorAttachmentCount = 1; rendering.pColorAttachmentFormats = &format;
            VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &rendering};
            info.stageCount = 2; info.pStages = stages; info.pVertexInputState = &vi; info.pInputAssemblyState = &ia; info.pViewportState = &vp; info.pRasterizationState = &rs; info.pMultisampleState = &ms; info.pDepthStencilState = &ds; info.pColorBlendState = &cb; info.pDynamicState = &dyn; info.layout = layout;
            VkPipeline pipeline;
            {
                auto lock = context.LockPipelineCacheForUse();
                CK(vkCreateGraphicsPipelines(device, context.pipelineCache, 1, &info, nullptr, &pipeline));
            }
            times[p] = msSince(start);
            std::printf("[bench] pipeline %zu (%s + %s): %.2f ms\n", p, std::strrchr(files[2 * p], '/') ? std::strrchr(files[2 * p], '/') + 1 : files[2 * p], std::strrchr(files[2 * p + 1], '/') ? std::strrchr(files[2 * p + 1], '/') + 1 : files[2 * p + 1], times[p]);
            std::fflush(stdout);
            vkDestroyPipeline(device, pipeline, nullptr);
            vkDestroyShaderModule(device, vs, nullptr);
            vkDestroyShaderModule(device, fs, nullptr);
        }
    };
    const auto buildStart = Clock::now();
    if (prewarm) {
        std::thread worker(buildAll);
        // Main thread stands in for the render loop: a 1 ms tick whose longest gap is the stall the prewarm causes.
        double maxGap = 0;
        auto last = Clock::now();
        std::atomic<bool> done{false};
        std::thread watcher([&] { worker.join(); done = true; });
        while (!done) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            maxGap = std::max(maxGap, msSince(last));
            last = Clock::now();
        }
        watcher.join();
        std::printf("[bench] prewarm on background thread: render-thread longest tick gap %.2f ms\n", maxGap);
    } else {
        buildAll();
    }
    const double totalMs = msSince(buildStart);
    double sum = 0;
    for (double t : times) sum += t;
    std::printf("[bench] device=%s driver=%s | vkCreateDevice %.1f ms | PipelineCache load %.1f ms | %zu pipelines total %.1f ms (sum %.1f) | process to last pipeline %.1f ms\n", properties.deviceName, driverInfo.c_str(), deviceMs, cacheMs, pairs, totalMs, sum, msSince(processStart));
    const auto saveStart = Clock::now();
    cache.reset();
    std::printf("[bench] PipelineCache save/teardown %.1f ms\n", msSince(saveStart));
    vkDestroyPipelineLayout(device, layout, nullptr);
    vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);
    return 0;
}
