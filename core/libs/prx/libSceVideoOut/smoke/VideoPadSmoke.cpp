// Host-side arm64 smoke test for libSceVideoOut (DisplayWindow, SDL+Vulkan),
// libScePad and libSceUserService. No guest code. Opens a window through the
// real DisplayWindow class, presents a cleared frame via the Vulkan loader
// (KosmicKrisp ICD via VK_DRIVER_FILES), polls the pad API once.
#include "prx/libSceVideoOut/include/DisplayWindow.hpp"
#include "prx/libScePad/include/Pad.hpp"
#include "prx/libSceUserService/UserService.hpp"
#include "SDL_vulkan.h"
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <cstdio>
#include <cstdlib>
#include <vector>

#define CHK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { std::printf("FAIL %s = %d\n", #x, (int)r_); return 1; } } while (0)

int main() {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0) { std::printf("SDL_Init: %s\n", SDL_GetError()); return 1; }
    const char* lib = std::getenv("SDL_VULKAN_LIBRARY");
    if (SDL_Vulkan_LoadLibrary(lib) != 0) { std::printf("SDL_Vulkan_LoadLibrary: %s\n", SDL_GetError()); return 1; }

    // user service + pad, host side
    int rc = sceUserServiceInitialize(nullptr);
    int user = -1;
    int rc2 = sceUserServiceGetInitialUser(&user);
    int prc = scePadInit_nid_postfix();
    int handle = scePadOpen_nid_postfix(user, 0, 0, nullptr);
    if (handle < 0) handle = scePadGetHandle(user, 0, 0);
    PadData pd{};
    int rrc = scePadReadState(handle, &pd);
    std::printf("user: init=%d initial=%d id=%d | pad: init=%d handle=%d read=%d connected=%d buttons=0x%x\n",
                rc, rc2, user, prc, handle, rrc, (int)pd.connected, (unsigned)pd.buttons);

    DisplayWindow win;
    win.Ensure(1920, 1080);
    SDL_Window* w = win.Handle();
    if (!w) { std::printf("no window\n"); return 1; }
    std::uint32_t dw = 0, dh = 0;
    win.DrawableSize(dw, dh);
    std::printf("window ok, drawable %ux%u\n", dw, dh);

    auto gipa = (PFN_vkGetInstanceProcAddr)SDL_Vulkan_GetVkGetInstanceProcAddr();
    unsigned n = 0;
    SDL_Vulkan_GetInstanceExtensions(w, &n, nullptr);
    std::vector<const char*> exts(n);
    SDL_Vulkan_GetInstanceExtensions(w, &n, exts.data());
    exts.push_back("VK_KHR_portability_enumeration");
    exts.push_back("VK_KHR_get_physical_device_properties2");
    VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr, "nps5-smoke", 1, "nps5", 1, VK_API_VERSION_1_1};
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    ici.pApplicationInfo = &ai; ici.enabledExtensionCount = (uint32_t)exts.size(); ici.ppEnabledExtensionNames = exts.data();
    VkInstance inst;
    auto createInst = (PFN_vkCreateInstance)gipa(nullptr, "vkCreateInstance");
    CHK(createInst(&ici, nullptr, &inst));
#define IP(n) auto n = (PFN_##n)gipa(inst, #n)
    IP(vkEnumeratePhysicalDevices); IP(vkGetPhysicalDeviceProperties); IP(vkGetPhysicalDeviceQueueFamilyProperties);
    IP(vkGetPhysicalDeviceSurfaceSupportKHR); IP(vkGetPhysicalDeviceSurfaceCapabilitiesKHR); IP(vkGetPhysicalDeviceSurfaceFormatsKHR);
    IP(vkCreateDevice); IP(vkGetDeviceProcAddr); IP(vkDestroyInstance); IP(vkDestroySurfaceKHR);
    VkSurfaceKHR surf;
    if (!SDL_Vulkan_CreateSurface(w, inst, &surf)) { std::printf("CreateSurface: %s\n", SDL_GetError()); return 1; }
    uint32_t pc = 1; VkPhysicalDevice pdv;
    vkEnumeratePhysicalDevices(inst, &pc, &pdv);
    VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(pdv, &props);
    std::printf("vulkan device: %s (api %u.%u)\n", props.deviceName, VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion));
    uint32_t qc = 0; vkGetPhysicalDeviceQueueFamilyProperties(pdv, &qc, nullptr);
    std::vector<VkQueueFamilyProperties> qf(qc); vkGetPhysicalDeviceQueueFamilyProperties(pdv, &qc, qf.data());
    uint32_t fam = ~0u;
    for (uint32_t i = 0; i < qc; i++) { VkBool32 s = 0; vkGetPhysicalDeviceSurfaceSupportKHR(pdv, i, surf, &s); if (s && (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) { fam = i; break; } }
    if (fam == ~0u) { std::printf("no present queue\n"); return 1; }
    float prio = 1; VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, nullptr, 0, fam, 1, &prio};
    const char* dext[] = {"VK_KHR_swapchain"};
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qi; dci.enabledExtensionCount = 1; dci.ppEnabledExtensionNames = dext;
    VkDevice dev; CHK(vkCreateDevice(pdv, &dci, nullptr, &dev));
#define DP(n) auto n = (PFN_##n)vkGetDeviceProcAddr(dev, #n)
    DP(vkGetDeviceQueue); DP(vkCreateSwapchainKHR); DP(vkGetSwapchainImagesKHR); DP(vkAcquireNextImageKHR); DP(vkQueuePresentKHR);
    DP(vkCreateCommandPool); DP(vkAllocateCommandBuffers); DP(vkBeginCommandBuffer); DP(vkEndCommandBuffer); DP(vkCmdPipelineBarrier);
    DP(vkCmdClearColorImage); DP(vkQueueSubmit); DP(vkCreateSemaphore); DP(vkCreateFence); DP(vkWaitForFences); DP(vkResetFences); DP(vkDeviceWaitIdle);
    DP(vkResetCommandBuffer);
    VkQueue q; vkGetDeviceQueue(dev, fam, 0, &q);
    VkSurfaceCapabilitiesKHR caps; vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pdv, surf, &caps);
    uint32_t fc = 0; vkGetPhysicalDeviceSurfaceFormatsKHR(pdv, surf, &fc, nullptr);
    std::vector<VkSurfaceFormatKHR> fmts(fc); vkGetPhysicalDeviceSurfaceFormatsKHR(pdv, surf, &fc, fmts.data());
    VkExtent2D ext = caps.currentExtent.width == 0xFFFFFFFFu ? VkExtent2D{dw, dh} : caps.currentExtent;
    std::printf("surface: %u formats, first fmt=%d, extent %ux%u, usage=0x%x\n", fc, (int)fmts[0].format, ext.width, ext.height, caps.supportedUsageFlags);
    VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    sci.surface = surf; sci.minImageCount = caps.minImageCount; sci.imageFormat = fmts[0].format; sci.imageColorSpace = fmts[0].colorSpace;
    sci.imageExtent = ext; sci.imageArrayLayers = 1; sci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT; sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform = caps.currentTransform; sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR; sci.presentMode = VK_PRESENT_MODE_FIFO_KHR; sci.clipped = VK_TRUE;
    VkSwapchainKHR sc; CHK(vkCreateSwapchainKHR(dev, &sci, nullptr, &sc));
    uint32_t ic = 0; vkGetSwapchainImagesKHR(dev, sc, &ic, nullptr); std::vector<VkImage> imgs(ic); vkGetSwapchainImagesKHR(dev, sc, &ic, imgs.data());
    VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, fam};
    VkCommandPool pool; CHK(vkCreateCommandPool(dev, &cpi, nullptr, &pool));
    VkCommandBufferAllocateInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkCommandBuffer cb; CHK(vkAllocateCommandBuffers(dev, &cbi, &cb));
    VkSemaphoreCreateInfo semi{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO}; VkSemaphore acq, rel; CHK(vkCreateSemaphore(dev, &semi, nullptr, &acq)); CHK(vkCreateSemaphore(dev, &semi, nullptr, &rel));
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VkFence fence; CHK(vkCreateFence(dev, &fi, nullptr, &fence));
    int presented = 0;
    for (int frame = 0; frame < 30; frame++) {
        SDL_Event e; while (SDL_PollEvent(&e)) {}
        uint32_t idx; CHK(vkAcquireNextImageKHR(dev, sc, UINT64_MAX, acq, VK_NULL_HANDLE, &idx));
        vkResetCommandBuffer(cb, 0);
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        vkBeginCommandBuffer(cb, &bi);
        VkImageSubresourceRange rg{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.image = imgs[idx]; b.subresourceRange = rg;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        VkClearColorValue cc{{0.1f, 0.4f, 0.8f, 1.f}}; vkCmdClearColorImage(cb, imgs[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cc, 1, &rg);
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask = 0; b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        vkEndCommandBuffer(cb);
        VkPipelineStageFlags ws = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.waitSemaphoreCount = 1; si.pWaitSemaphores = &acq; si.pWaitDstStageMask = &ws; si.commandBufferCount = 1; si.pCommandBuffers = &cb; si.signalSemaphoreCount = 1; si.pSignalSemaphores = &rel;
        CHK(vkQueueSubmit(q, 1, &si, fence));
        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR}; pi.waitSemaphoreCount = 1; pi.pWaitSemaphores = &rel; pi.swapchainCount = 1; pi.pSwapchains = &sc; pi.pImageIndices = &idx;
        CHK(vkQueuePresentKHR(q, &pi));
        vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX); vkResetFences(dev, 1, &fence);
        presented++;
        SDL_Delay(16);
    }
    vkDeviceWaitIdle(dev);
    std::printf("presented %d cleared frames\nSMOKE OK\n", presented);
    win.Destroy();
    SDL_Quit();
    return 0;
}
