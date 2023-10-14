#pragma once
#include "Graphics/Val/ZenVal.h"
#include "Graphics/Rendering/ResourceCache.h"
#include "Common/UniquePtr.h"

namespace zen
{
class RenderDevice
{
public:
    explicit RenderDevice(const val::Device& valDevice);

    val::RenderPass* RequestRenderPass(const std::vector<VkAttachmentDescription>& attachments,
                                       const val::SubpassInfo&                     subpassInfo);

    val::PipelineLayout* RequestPipelineLayout(
        const std::vector<val::ShaderModule*>& shaderModules);

    val::GraphicsPipeline* RequestGraphicsPipeline(const val::PipelineLayout& pipelineLayout,
                                                   val::PipelineState&        pipelineState);

    UniquePtr<val::Image> CreateImageUnique(const val::ImageCreateInfo& imageCI);

    UniquePtr<val::Buffer> CreateBufferUnique(const val::BufferCreateInfo& bufferCI);

    UniquePtr<val::Framebuffer> CreateFramebufferUnique(VkRenderPass renderPassHandle,
                                                        const std::vector<VkImageView>& attachments,
                                                        VkExtent3D                      extent3D);

    VkDescriptorSet RequestDescriptorSet(const val::DescriptorSetLayout& layout);

    void UpdateDescriptorSets(const std::vector<VkWriteDescriptorSet>& writes);

private:
    const val::Device&       m_valDevice;
    UniquePtr<ResourceCache> m_resourceCache;

    val::DescriptorPoolManager  m_descriptorPoolManager;
    val::DescriptorSetAllocator m_descriptorAllocator;
    // cache
    std::unordered_map<size_t, VkDescriptorSet> m_descriptorSetCache;
};
} // namespace zen