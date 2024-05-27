#if defined(ZEN_MACOS)
#    include <dlfcn.h>
#    include "Graphics/VulkanRHI/Platform/VulkanMacOSPlatform.h"
#    include "Graphics/VulkanRHI/VulkanExtension.h"

namespace zen
{
void VulkanMacOSPlatform::AddInstanceExtensions(
    std::vector<UniquePtr<VulkanInstanceExtension>>& extensions)
{
    extensions.emplace_back(
        MakeUnique<VulkanInstanceExtension>(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME));
    extensions.emplace_back(MakeUnique<VulkanInstanceExtension>("VK_EXT_metal_surface"));
    extensions.emplace_back(
        MakeUnique<VulkanInstanceExtension>(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME));
}

bool VulkanMacOSPlatform::VolkInitialize()
{
    void* module = dlopen("/usr/local/lib/libvulkan.dylib", RTLD_NOW | RTLD_LOCAL);
    if (!module) { return false; }
    auto handle = (PFN_vkGetInstanceProcAddr)dlsym(module, "vkGetInstanceProcAddr");
    volkInitializeCustom(handle);
    return true;
}
} // namespace zen

#endif