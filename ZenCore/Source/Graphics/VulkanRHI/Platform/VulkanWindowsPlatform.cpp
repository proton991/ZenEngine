#include "Graphics/VulkanRHI/Platform/VulkanWindowsPlatform.h"
#include "Graphics/VulkanRHI/VulkanExtension.h"

namespace zen
{
void VulkanWindowsPlatform::AddInstanceExtensions(
    std::vector<UniquePtr<VulkanInstanceExtension>>& extensions)
{
    extensions.emplace_back(
        MakeUnique<VulkanInstanceExtension>(VK_KHR_WIN32_SURFACE_EXTENSION_NAME));
    extensions.emplace_back(
        MakeUnique<VulkanInstanceExtension>(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME));
}
} // namespace zen