#pragma once
#include "Graphics/Val/ZenVal.h"
#include "Graphics/Rendering/ResourceCache.h"
#include "Common/UniquePtr.h"

namespace zen
{
class RenderDevice
{
public:
    explicit RenderDevice(val::Device& valDevice);

    val::RenderPass* RequestRenderPass(const std::vector<VkAttachmentDescription>& attachments, const val::SubpassInfo& subpassInfo);

    val::PipelineLayout* RequestPipelineLayout(const std::vector<val::ShaderModule*>& shaderModules);

    val::GraphicsPipeline* RequestGraphicsPipeline(const val::PipelineLayout& pipelineLayout, val::PipelineState& pipelineState);

    UniquePtr<val::Image> CreateImageUnique(const val::ImageCreateInfo& imageCI);

    UniquePtr<val::Buffer> CreateBufferUnique(const val::BufferCreateInfo& bufferCI);

    UniquePtr<val::Framebuffer> CreateFramebufferUnique(VkRenderPass renderPassHandle, const std::vector<VkImageView>& attachments, VkExtent3D extent3D);

private:
    val::Device&             m_valDevice;
    UniquePtr<ResourceCache> m_resourceCache;
};
} // namespace zen