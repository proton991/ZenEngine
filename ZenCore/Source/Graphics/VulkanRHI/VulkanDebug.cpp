#include "Graphics/VulkanRHI/VulkanDebug.h"
#include "Graphics/VulkanRHI/VulkanDevice.h"
#include "Graphics/VulkanRHI/VulkanRHI.h"
#include "Graphics/VulkanRHI/VulkanTexture.h"
#include "Graphics/VulkanRHI/VulkanPipeline.h"

namespace zen
{
VulkanDebug::VulkanDebug() {}

void VulkanDebug::SetPipelineDebugName(RHIPipeline* pipelineHandle, const std::string& debugName)
{
    // VulkanRHI* GVulkanRHI = dynamic_cast<VulkanRHI*>(m_RHI);

    VulkanPipeline* vulkanPipeline = TO_VK_PIPELINE(pipelineHandle);
    VkDebugUtilsObjectNameInfoEXT info{};
    info.sType        = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
    info.pNext        = nullptr;
    info.objectType   = VK_OBJECT_TYPE_PIPELINE;
    info.objectHandle = reinterpret_cast<uint64_t>(vulkanPipeline->GetVkPipeline());
    info.pObjectName  = debugName.data();

    CHECK_VK_ERROR(vkSetDebugUtilsObjectNameEXT(GVulkanRHI->GetVkDevice(), &info),
                   "Failed to set debug object name");
}

void VulkanDebug::SetTextureDebugName(RHITexture* texture, const std::string& debugName)
{
    // VulkanRHI* GVulkanRHI = dynamic_cast<VulkanRHI*>(m_RHI);

    VulkanTexture* vulkanTexture = TO_VK_TEXTURE(texture);
    VkDebugUtilsObjectNameInfoEXT info{};
    info.sType        = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
    info.pNext        = nullptr;
    info.objectType   = VK_OBJECT_TYPE_IMAGE;
    info.objectHandle = reinterpret_cast<uint64_t>(vulkanTexture->GetVkImage());
    info.pObjectName  = debugName.data();

    CHECK_VK_ERROR(vkSetDebugUtilsObjectNameEXT(GVulkanRHI->GetVkDevice(), &info),
                   "Failed to set debug object name");
}

void VulkanDebug::SetRenderPassDebugName(RenderPassHandle renderPassHandle,
                                         const std::string& debugName)
{
    // VulkanRHI* GVulkanRHI = dynamic_cast<VulkanRHI*>(m_RHI);

    VkRenderPass renderPass = TO_VK_RENDER_PASS(renderPassHandle);
    VkDebugUtilsObjectNameInfoEXT info{};
    info.sType        = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
    info.pNext        = nullptr;
    info.objectType   = VK_OBJECT_TYPE_RENDER_PASS;
    info.objectHandle = reinterpret_cast<uint64_t>(renderPass);
    info.pObjectName  = debugName.data();

    CHECK_VK_ERROR(vkSetDebugUtilsObjectNameEXT(GVulkanRHI->GetVkDevice(), &info),
                   "Failed to set debug object name");
}

void VulkanDebug::SetDescriptorSetDebugName(RHIDescriptorSet* descriptorSetHandle,
                                            const std::string& debugName)
{
    // VulkanRHI* GVulkanRHI = dynamic_cast<VulkanRHI*>(m_RHI);

    VulkanDescriptorSet* descriptorSet = TO_VK_DESCRIPTORSET(descriptorSetHandle);
    VkDebugUtilsObjectNameInfoEXT info{};
    info.sType        = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
    info.pNext        = nullptr;
    info.objectType   = VK_OBJECT_TYPE_DESCRIPTOR_SET;
    info.objectHandle = reinterpret_cast<uint64_t>(descriptorSet->GetVkDescriptorSet());
    info.pObjectName  = debugName.data();

    CHECK_VK_ERROR(vkSetDebugUtilsObjectNameEXT(GVulkanRHI->GetVkDevice(), &info),
                   "Failed to set debug object name");
}

} // namespace zen