#include "Graphics/VulkanRHI/VulkanDebug.h"
#include "Graphics/VulkanRHI/VulkanDevice.h"
#include "Graphics/VulkanRHI/VulkanRHI.h"
#include "Graphics/VulkanRHI/VulkanTexture.h"
#include "Graphics/VulkanRHI/VulkanPipeline.h"

namespace zen::rhi
{
VulkanDebug::VulkanDebug(DynamicRHI* RHI) : RHIDebug(RHI) {}

void VulkanDebug::SetPipelineDebugName(PipelineHandle pipelineHandle, const std::string& debugName)
{
    VulkanRHI* vkRHI = dynamic_cast<VulkanRHI*>(m_RHI);

    VulkanPipeline* vulkanPipeline = TO_VK_PIPELINE(pipelineHandle);
    VkDebugUtilsObjectNameInfoEXT info{};
    info.sType        = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
    info.pNext        = nullptr;
    info.objectType   = VK_OBJECT_TYPE_PIPELINE;
    info.objectHandle = reinterpret_cast<uint64_t>(vulkanPipeline->pipeline);
    info.pObjectName  = debugName.data();

    CHECK_VK_ERROR(vkSetDebugUtilsObjectNameEXT(vkRHI->GetDevice()->GetVkHandle(), &info),
                   "Failed to set debug object name");
}

void VulkanDebug::SetTextureDebugName(TextureHandle textureHandle, const std::string& debugName)
{
    VulkanRHI* vkRHI = dynamic_cast<VulkanRHI*>(m_RHI);

    VulkanTexture* vulkanTexture = TO_VK_TEXTURE(textureHandle);
    VkDebugUtilsObjectNameInfoEXT info{};
    info.sType        = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
    info.pNext        = nullptr;
    info.objectType   = VK_OBJECT_TYPE_IMAGE;
    info.objectHandle = reinterpret_cast<uint64_t>(vulkanTexture->image);
    info.pObjectName  = debugName.data();

    CHECK_VK_ERROR(vkSetDebugUtilsObjectNameEXT(vkRHI->GetDevice()->GetVkHandle(), &info),
                   "Failed to set debug object name");
}

void VulkanDebug::SetRenderPassDebugName(RenderPassHandle renderPassHandle,
                                         const std::string& debugName)
{
    VulkanRHI* vkRHI = dynamic_cast<VulkanRHI*>(m_RHI);

    VkRenderPass renderPass = TO_VK_RENDER_PASS(renderPassHandle);
    VkDebugUtilsObjectNameInfoEXT info{};
    info.sType        = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
    info.pNext        = nullptr;
    info.objectType   = VK_OBJECT_TYPE_RENDER_PASS;
    info.objectHandle = reinterpret_cast<uint64_t>(renderPass);
    info.pObjectName  = debugName.data();

    CHECK_VK_ERROR(vkSetDebugUtilsObjectNameEXT(vkRHI->GetDevice()->GetVkHandle(), &info),
                   "Failed to set debug object name");
}

void VulkanDebug::SetDescriptorSetDebugName(DescriptorSetHandle descriptorSetHandle,
                                            const std::string& debugName)
{
    VulkanRHI* vkRHI = dynamic_cast<VulkanRHI*>(m_RHI);

    VulkanDescriptorSet* descriptorSet =
        reinterpret_cast<VulkanDescriptorSet*>(descriptorSetHandle.value);
    VkDebugUtilsObjectNameInfoEXT info{};
    info.sType        = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
    info.pNext        = nullptr;
    info.objectType   = VK_OBJECT_TYPE_DESCRIPTOR_SET;
    info.objectHandle = reinterpret_cast<uint64_t>(descriptorSet);
    info.pObjectName  = debugName.data();

    CHECK_VK_ERROR(vkSetDebugUtilsObjectNameEXT(vkRHI->GetDevice()->GetVkHandle(), &info),
                   "Failed to set debug object name");
}

} // namespace zen::rhi