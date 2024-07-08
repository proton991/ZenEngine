#include "Graphics/RenderCore/RenderDevice.h"
#include "Graphics/RenderCore/ResourceCache.h"

namespace zen
{
RenderDevice::RenderDevice(const val::Device& valDevice) :
    m_valDevice(valDevice),
    m_descriptorPoolManager(valDevice, val::MainDescriptorPoolSizes(), false),
    m_descriptorAllocator(valDevice, m_descriptorPoolManager)
{
    m_resourceCache = MakeUnique<ResourceCache>(m_valDevice);
}

UniquePtr<val::Image> RenderDevice::CreateImageUnique(const val::ImageCreateInfo& imageCI)
{
    return val::Image::CreateUnique(m_valDevice, imageCI);
}

UniquePtr<val::Buffer> RenderDevice::CreateBufferUnique(const val::BufferCreateInfo& bufferCI)
{
    return val::Buffer::CreateUnique(m_valDevice, bufferCI);
}

UniquePtr<val::Framebuffer> RenderDevice::CreateFramebufferUnique(
    VkRenderPass renderPassHandle,
    const std::vector<VkImageView>& attachments,
    VkExtent3D extent3D)
{
    return MakeUnique<val::Framebuffer>(m_valDevice, renderPassHandle, attachments, extent3D);
}

val::Framebuffer* RenderDevice::RequestFramebuffer(VkRenderPass renderPassHandle,
                                                   const std::vector<VkImageView>& attachments,
                                                   VkExtent3D extent3D)
{
    return m_resourceCache->RequestFramebuffer(renderPassHandle, attachments, extent3D);
}

val::RenderPass* RenderDevice::RequestRenderPass(
    const std::vector<VkAttachmentDescription>& attachments,
    const val::SubpassInfo& subpassInfo)
{
    return m_resourceCache->RequestRenderPass(attachments, subpassInfo);
}

val::PipelineLayout* RenderDevice::RequestPipelineLayout(
    const std::vector<val::ShaderModule*>& shaderModules)
{
    return m_resourceCache->RequestPipelineLayout(shaderModules);
}

val::GraphicsPipeline* RenderDevice::RequestGraphicsPipeline(
    const val::PipelineLayout& pipelineLayout,
    val::PipelineState& pipelineState)
{
    return m_resourceCache->RequestGraphicsPipeline(pipelineLayout, pipelineState);
}

VkDescriptorSet RenderDevice::RequestDescriptorSet(const val::DescriptorSetLayout& layout)
{
    auto hash = layout.GetHash();
    auto it   = m_descriptorSetCache.find(hash);
    if (it != m_descriptorSetCache.end())
        return it->second;
    VkDescriptorSet descriptorSet;
    VkDescriptorSetLayout dsLayout = layout.GetHandle();
    m_descriptorAllocator.Allocate(&dsLayout, 1, &descriptorSet);
    m_descriptorSetCache.emplace(hash, descriptorSet);
    return descriptorSet;
}

void RenderDevice::UpdateDescriptorSets(const std::vector<VkWriteDescriptorSet>& writes)
{
    vkUpdateDescriptorSets(m_valDevice.GetHandle(), util::ToU32(writes.size()), writes.data(), 0,
                           nullptr);
}

size_t RenderDevice::PadUniformBufferSize(size_t originalSize)
{
    auto minUboAlignment = m_valDevice.GetGPUProperties().limits.minUniformBufferOffsetAlignment;
    size_t alignedSize   = (originalSize + minUboAlignment - 1) & ~(minUboAlignment - 1);
    return alignedSize;
}
} // namespace zen