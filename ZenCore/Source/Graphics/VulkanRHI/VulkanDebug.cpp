#include "Graphics/VulkanRHI/VulkanDebug.h"
#include "Graphics/VulkanRHI/VulkanDevice.h"
#include "Graphics/VulkanRHI/VulkanRHI.h"
#include "Graphics/VulkanRHI/VulkanTexture.h"
#include "Graphics/VulkanRHI/VulkanPipeline.h"

namespace zen
{
VulkanDebug::VulkanDebug() {}

void VulkanDebug::SetPipelineDebugName(RHIPipeline* pPipelineHandle, const std::string& debugName)
{
    // VulkanRHI* GVulkanRHI = dynamic_cast<VulkanRHI*>(m_RHI);

    VulkanPipeline* pVulkanPipeline = TO_VK_PIPELINE(pPipelineHandle);
    VkDebugUtilsObjectNameInfoEXT info{};
    info.sType        = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
    info.pNext        = nullptr;
    info.objectType   = VK_OBJECT_TYPE_PIPELINE;
    info.objectHandle = reinterpret_cast<uint64_t>(pVulkanPipeline->GetVkPipeline());
    info.pObjectName  = debugName.data();

    CHECK_VK_ERROR(vkSetDebugUtilsObjectNameEXT(GVulkanRHI->GetVkDevice(), &info),
                   "Failed to set debug object name");
}

void VulkanDebug::SetTextureDebugName(RHITexture* pTexture, const std::string& debugName)
{
    // VulkanRHI* GVulkanRHI = dynamic_cast<VulkanRHI*>(m_RHI);

    VulkanTexture* pVulkanTexture = TO_VK_TEXTURE(pTexture);
    VkDebugUtilsObjectNameInfoEXT info{};
    info.sType        = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
    info.pNext        = nullptr;
    info.objectType   = VK_OBJECT_TYPE_IMAGE;
    info.objectHandle = reinterpret_cast<uint64_t>(pVulkanTexture->GetVkImage());
    info.pObjectName  = debugName.data();

    CHECK_VK_ERROR(vkSetDebugUtilsObjectNameEXT(GVulkanRHI->GetVkDevice(), &info),
                   "Failed to set debug object name");
}

// void VulkanDebug::SetRenderPassDebugName(RenderPassHandle renderPassHandle,
//                                          const std::string& debugName)
// {
//     // VulkanRHI* GVulkanRHI = dynamic_cast<VulkanRHI*>(m_RHI);
//
//     VkRenderPass renderPass = TO_VK_RENDER_PASS(renderPassHandle);
//     VkDebugUtilsObjectNameInfoEXT info{};
//     info.sType        = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
//     info.pNext        = nullptr;
//     info.objectType   = VK_OBJECT_TYPE_RENDER_PASS;
//     info.objectHandle = reinterpret_cast<uint64_t>(renderPass);
//     info.pObjectName  = debugName.data();
//
//     CHECK_VK_ERROR(vkSetDebugUtilsObjectNameEXT(GVulkanRHI->GetVkDevice(), &info),
//                    "Failed to set debug object name");
// }

void VulkanDebug::SetDescriptorSetDebugName(RHIDescriptorSet* pDescriptorSetHandle,
                                            const std::string& debugName)
{
    // VulkanRHI* GVulkanRHI = dynamic_cast<VulkanRHI*>(m_RHI);

    VulkanDescriptorSet* pDescriptorSet = TO_VK_DESCRIPTORSET(pDescriptorSetHandle);
    VkDebugUtilsObjectNameInfoEXT info{};
    info.sType        = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
    info.pNext        = nullptr;
    info.objectType   = VK_OBJECT_TYPE_DESCRIPTOR_SET;
    info.objectHandle = reinterpret_cast<uint64_t>(pDescriptorSet->GetVkDescriptorSet());
    info.pObjectName  = debugName.data();

    CHECK_VK_ERROR(vkSetDebugUtilsObjectNameEXT(GVulkanRHI->GetVkDevice(), &info),
                   "Failed to set debug object name");
}

} // namespace zen
