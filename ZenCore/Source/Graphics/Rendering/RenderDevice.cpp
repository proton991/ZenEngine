#include "Graphics/Rendering/RenderDevice.h"
#include "Graphics/Rendering/ResourceCache.h"

namespace zen
{
RenderDevice::RenderDevice(val::Device& valDevice) :
    m_valDevice(valDevice)
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

UniquePtr<val::Framebuffer> RenderDevice::CreateFramebufferUnique(VkRenderPass renderPassHandle, const std::vector<VkImageView>& attachments, VkExtent3D extent3D)
{
    return MakeUnique<val::Framebuffer>(m_valDevice, renderPassHandle, attachments, extent3D);
}

val::RenderPass* RenderDevice::RequestRenderPass(const std::vector<VkAttachmentDescription>& attachments, const val::SubpassInfo& subpassInfo)
{
    return m_resourceCache->RequestRenderPass(attachments, subpassInfo);
}

val::PipelineLayout* RenderDevice::RequestPipelineLayout(const std::vector<val::ShaderModule*>& shaderModules)
{
    return m_resourceCache->RequestPipelineLayout(shaderModules);
}

val::GraphicsPipeline* RenderDevice::RequestGraphicsPipeline(const val::PipelineLayout& pipelineLayout, val::PipelineState& pipelineState)
{
    return m_resourceCache->RequestGraphicsPipeline(pipelineLayout, pipelineState);
}
} // namespace zen