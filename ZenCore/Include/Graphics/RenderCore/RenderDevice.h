#pragma once
#include "Graphics/Val/ZenVal.h"
#include "Graphics/RenderCore/ResourceCache.h"
#include "Common/UniquePtr.h"

namespace zen
{
class RenderDevice
{
public:
    explicit RenderDevice(const val::Device& valDevice);

    val::Framebuffer* RequestFramebuffer(VkRenderPass renderPassHandle,
                                         const std::vector<VkImageView>& attachments,
                                         VkExtent3D extent3D);

    val::RenderPass* RequestRenderPass(const std::vector<VkAttachmentDescription>& attachments,
                                       const val::SubpassInfo& subpassInfo);

    val::PipelineLayout* RequestPipelineLayout(
        const std::vector<val::ShaderModule*>& shaderModules);

    val::GraphicsPipeline* RequestGraphicsPipeline(const val::PipelineLayout& pipelineLayout,
                                                   val::PipelineState& pipelineState);

    UniquePtr<val::Image> CreateImageUnique(const val::ImageCreateInfo& imageCI);

    UniquePtr<val::Buffer> CreateBufferUnique(const val::BufferCreateInfo& bufferCI);

    UniquePtr<val::Framebuffer> CreateFramebufferUnique(VkRenderPass renderPassHandle,
                                                        const std::vector<VkImageView>& attachments,
                                                        VkExtent3D extent3D);

    VkDescriptorSet RequestDescriptorSet(const val::DescriptorSetLayout& layout);

    void UpdateDescriptorSets(const std::vector<VkWriteDescriptorSet>& writes);

    size_t PadUniformBufferSize(size_t originalSize);

private:
    const val::Device& m_valDevice;
    UniquePtr<ResourceCache> m_resourceCache;

    val::DescriptorPoolManager m_descriptorPoolManager;
    val::DescriptorSetAllocator m_descriptorAllocator;
    // cache
    HashMap<size_t, VkDescriptorSet> m_descriptorSetCache;
};
} // namespace zen