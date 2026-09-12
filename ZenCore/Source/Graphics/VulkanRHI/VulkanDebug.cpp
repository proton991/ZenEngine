#include "Graphics/VulkanRHI/VulkanDebug.h"
#include "Graphics/VulkanRHI/VulkanDevice.h"
#include "Graphics/VulkanRHI/VulkanRHI.h"
#include "Graphics/VulkanRHI/VulkanTexture.h"
#include "Graphics/VulkanRHI/VulkanPipeline.h"

namespace zen
{
VulkanDebug::VulkanDebug() {}

void VulkanDebug::SetPipelineDebugName(RHIPipeline* pPipelineHandle, NameID debugName)
{
    VulkanPipeline* pVulkanPipeline = TO_VK_PIPELINE(pPipelineHandle);
    VkDebugUtilsObjectNameInfoEXT info{};
    info.sType        = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
    info.pNext        = nullptr;
    info.objectType   = VK_OBJECT_TYPE_PIPELINE;
    info.objectHandle = reinterpret_cast<uint64_t>(pVulkanPipeline->GetVkPipeline());
    info.pObjectName  = debugName.CStr();

    CHECK_VK_ERROR(vkSetDebugUtilsObjectNameEXT(GVulkanRHI->GetVkDevice(), &info),
                   "Failed to set debug object name");
}

void VulkanDebug::SetTextureDebugName(RHITexture* pTexture, NameID debugName)
{
    VulkanTexture* pVulkanTexture = TO_VK_TEXTURE(pTexture);
    VkDebugUtilsObjectNameInfoEXT info{};
    info.sType        = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
    info.pNext        = nullptr;
    info.objectType   = VK_OBJECT_TYPE_IMAGE;
    info.objectHandle = reinterpret_cast<uint64_t>(pVulkanTexture->GetVkImage());
    info.pObjectName  = debugName.CStr();

    CHECK_VK_ERROR(vkSetDebugUtilsObjectNameEXT(GVulkanRHI->GetVkDevice(), &info),
                   "Failed to set debug object name");
}

} // namespace zen
