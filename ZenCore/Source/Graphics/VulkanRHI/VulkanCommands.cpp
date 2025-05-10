#include "Graphics/VulkanRHI/VulkanCommands.h"
#include "Graphics/VulkanRHI/VulkanBuffer.h"
#include "Graphics/VulkanRHI/VulkanCommandBuffer.h"
#include "Graphics/VulkanRHI/VulkanCommon.h"
#include "Graphics/VulkanRHI/VulkanDevice.h"
#include "Graphics/VulkanRHI/VulkanPipeline.h"
#include "Graphics/VulkanRHI/VulkanRenderPass.h"
#include "Graphics/VulkanRHI/VulkanTexture.h"
#include "Graphics/VulkanRHI/VulkanTypes.h"

namespace zen::rhi
{
VulkanCommandList::VulkanCommandList(VulkanCommandListContext* context) :
    m_cmdBufferManager(context->GetCmdBufferManager())
{}

void VulkanRHI::WaitForCommandList(RHICommandList* cmdList)
{
    VulkanCommandList* vulkanCmdList = dynamic_cast<VulkanCommandList*>(cmdList);
    if (vulkanCmdList->m_cmdBuffer != nullptr && vulkanCmdList->m_cmdBuffer->IsSubmitted())
    {
        vulkanCmdList->m_cmdBufferManager->WaitForCmdBuffer(vulkanCmdList->m_cmdBuffer);
    }
}

RHICommandList* VulkanRHI::GetImmediateCommandList()
{
    VulkanCommandListContext* context = m_device->GetImmediateCmdContext();
    return RHICommandList::Create(GraphicsAPIType::eVulkan, context);
}

VulkanCommandList::~VulkanCommandList() {}

void VulkanCommandList::BeginRender()
{
    m_cmdBuffer = m_cmdBufferManager->GetActiveCommandBuffer();
}

void VulkanCommandList::EndRender()
{
    m_cmdBufferManager->SubmitActiveCmdBuffer();
    m_cmdBufferManager->SetupNewActiveCmdBuffer();
}

void VulkanCommandList::BeginUpload()
{
    m_cmdBuffer = m_cmdBufferManager->GetUploadCommandBuffer();
}

void VulkanCommandList::EndUpload()
{
    m_cmdBufferManager->SubmitUploadCmdBuffer();
}

void VulkanCommandList::AddPipelineBarrier(BitField<PipelineStageBits> srcStages,
                                           BitField<PipelineStageBits> dstStages,
                                           const std::vector<MemoryTransition>& memoryTransitions,
                                           const std::vector<BufferTransition>& bufferTransitions,
                                           const std::vector<TextureTransition>& textureTransitions)
{
    std::vector<VkMemoryBarrier> memoryBarriers;
    std::vector<VkBufferMemoryBarrier> bufferBarriers;
    std::vector<VkImageMemoryBarrier> imageBarriers;
    memoryBarriers.reserve(memoryTransitions.size());
    bufferBarriers.reserve(bufferTransitions.size());
    imageBarriers.reserve(textureTransitions.size());

    for (const auto& memoryTransition : memoryTransitions)
    {
        VkMemoryBarrier memoryBarrier;
        InitVkStruct(memoryBarrier, VK_STRUCTURE_TYPE_MEMORY_BARRIER);
        memoryBarrier.srcAccessMask = ToVkAccessFlags(memoryTransition.srcAccess);
        memoryBarrier.dstAccessMask = ToVkAccessFlags(memoryTransition.dstAccess);
        memoryBarriers.emplace_back(memoryBarrier);
    }

    for (const auto& bufferTransition : bufferTransitions)
    {
        VulkanBuffer* vulkanBuffer =
            reinterpret_cast<VulkanBuffer*>(bufferTransition.bufferHandle.value);
        VkBufferMemoryBarrier bufferBarrier;
        InitVkStruct(bufferBarrier, VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER);
        bufferBarrier.buffer = vulkanBuffer->buffer;
        bufferBarrier.offset = bufferTransition.offset;
        bufferBarrier.size   = bufferTransition.size;
        bufferBarrier.srcAccessMask =
            ToVkAccessFlags(BufferUsageToAccessFlagBits(bufferTransition.oldUsage));
        bufferBarrier.dstAccessMask =
            ToVkAccessFlags(BufferUsageToAccessFlagBits(bufferTransition.newUsage));
        bufferBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bufferBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bufferBarriers.emplace_back(bufferBarrier);
    }

    for (const auto& textureTransition : textureTransitions)
    {
        VulkanTexture* vulkanTexture =
            reinterpret_cast<VulkanTexture*>(textureTransition.textureHandle.value);
        VkAccessFlags srcAccess =
            ToVkAccessFlags(TextureUsageToAccessFlagBits(textureTransition.oldUsage));
        VkAccessFlags dstAccess =
            ToVkAccessFlags(TextureUsageToAccessFlagBits(textureTransition.newUsage));
        VkImageLayout oldLayout = ToVkImageLayout(TextureUsageToLayout(textureTransition.oldUsage));
        VkImageLayout newLayout = ToVkImageLayout(TextureUsageToLayout(textureTransition.newUsage));

        VkImageMemoryBarrier imageBarrier;
        InitVkStruct(imageBarrier, VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER);
        imageBarrier.image               = vulkanTexture->image;
        imageBarrier.oldLayout           = oldLayout;
        imageBarrier.newLayout           = newLayout;
        imageBarrier.srcAccessMask       = srcAccess;
        imageBarrier.dstAccessMask       = dstAccess;
        imageBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        imageBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        imageBarrier.subresourceRange.aspectMask =
            ToVkAspectFlags(textureTransition.subResourceRange.aspect);
        imageBarrier.subresourceRange.layerCount = textureTransition.subResourceRange.layerCount;
        imageBarrier.subresourceRange.levelCount = textureTransition.subResourceRange.levelCount;
        imageBarrier.subresourceRange.baseArrayLayer =
            textureTransition.subResourceRange.baseArrayLayer;
        imageBarrier.subresourceRange.baseMipLevel =
            textureTransition.subResourceRange.baseMipLevel;
        imageBarriers.emplace_back(imageBarrier);
    }

    vkCmdPipelineBarrier(m_cmdBufferManager->GetActiveCommandBufferDirect()->GetVkHandle(),
                         srcStages, dstStages, 0, memoryBarriers.size(), memoryBarriers.data(),
                         bufferBarriers.size(), bufferBarriers.data(), imageBarriers.size(),
                         imageBarriers.data());
}

void VulkanCommandList::ClearBuffer(BufferHandle bufferHandle, uint32_t offset, uint32_t size)
{
    VulkanBuffer* vulkanBuffer = reinterpret_cast<VulkanBuffer*>(bufferHandle.value);
    vkCmdFillBuffer(m_cmdBuffer->GetVkHandle(), vulkanBuffer->buffer, offset, size, 0);
}

void VulkanCommandList::CopyBuffer(BufferHandle srcBufferHandle,
                                   BufferHandle dstBufferHandle,
                                   const BufferCopyRegion& region)
{
    VulkanBuffer* srcBuffer = reinterpret_cast<VulkanBuffer*>(srcBufferHandle.value);
    VulkanBuffer* dstBuffer = reinterpret_cast<VulkanBuffer*>(dstBufferHandle.value);
    VkBufferCopy bufferCopy;
    bufferCopy.srcOffset = region.srcOffset;
    bufferCopy.dstOffset = region.dstOffset;
    bufferCopy.size      = region.size;
    vkCmdCopyBuffer(m_cmdBuffer->GetVkHandle(), srcBuffer->buffer, dstBuffer->buffer, 1,
                    &bufferCopy);
}

void VulkanCommandList::ClearTexture(TextureHandle textureHandle,
                                     const Color& color,
                                     const TextureSubResourceRange& range)
{
    VulkanTexture* texture = reinterpret_cast<VulkanTexture*>(textureHandle.value);
    VkImageSubresourceRange vkRange;
    ToVkImageSubresourceRange(range, &vkRange);
    VkClearColorValue colorValue;
    ToVkClearColor(color, &colorValue);
    vkCmdClearColorImage(m_cmdBuffer->GetVkHandle(), texture->image,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &colorValue, 1, &vkRange);
}

void VulkanCommandList::CopyTexture(TextureHandle srcTextureHandle,
                                    TextureHandle dstTextureHandle,
                                    VectorView<TextureCopyRegion> regions)
{
    std::vector<VkImageCopy> copies(regions.size());
    for (uint32_t i = 0; i < regions.size(); i++)
    {
        ToVkImageCopy(regions[i], &copies[i]);
    }
    VulkanTexture* srcTexture = reinterpret_cast<VulkanTexture*>(srcTextureHandle.value);
    VulkanTexture* dstTexture = reinterpret_cast<VulkanTexture*>(dstTextureHandle.value);
    vkCmdCopyImage(m_cmdBuffer->GetVkHandle(), srcTexture->image,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dstTexture->image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, copies.size(), copies.data());
}

void VulkanCommandList::CopyTextureToBuffer(TextureHandle textureHandle,
                                            BufferHandle bufferHandle,
                                            VectorView<BufferTextureCopyRegion> regions)
{
    std::vector<VkBufferImageCopy> copies(regions.size());
    for (uint32_t i = 0; i < regions.size(); i++)
    {
        ToVkBufferImageCopy(regions[i], &copies[i]);
    }
    VulkanTexture* srcTexture = reinterpret_cast<VulkanTexture*>(textureHandle.value);
    VulkanBuffer* dstBuffer   = reinterpret_cast<VulkanBuffer*>(bufferHandle.value);

    vkCmdCopyImageToBuffer(m_cmdBuffer->GetVkHandle(), srcTexture->image,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dstBuffer->buffer, copies.size(),
                           copies.data());
}

void VulkanCommandList::CopyBufferToTexture(BufferHandle bufferHandle,
                                            TextureHandle textureHandle,
                                            VectorView<BufferTextureCopyRegion> regions)
{
    std::vector<VkBufferImageCopy> copies(regions.size());
    for (uint32_t i = 0; i < regions.size(); i++)
    {
        ToVkBufferImageCopy(regions[i], &copies[i]);
    }
    VulkanBuffer* srcBuffer   = reinterpret_cast<VulkanBuffer*>(bufferHandle.value);
    VulkanTexture* dstTexture = reinterpret_cast<VulkanTexture*>(textureHandle.value);

    vkCmdCopyBufferToImage(m_cmdBuffer->GetVkHandle(), srcBuffer->buffer, dstTexture->image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, copies.size(), copies.data());
}

void VulkanCommandList::ResolveTexture(TextureHandle srcTextureHandle,
                                       TextureHandle dstTextureHandle,
                                       uint32_t srcLayer,
                                       uint32_t srcMipmap,
                                       uint32_t dstLayer,
                                       uint32_t dstMipmap)
{
    VulkanTexture* srcTexture = reinterpret_cast<VulkanTexture*>(srcTextureHandle.value);
    VulkanTexture* dstTexture = reinterpret_cast<VulkanTexture*>(dstTextureHandle.value);
    VkImageResolve region{};
    region.srcSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    region.srcSubresource.mipLevel       = srcMipmap;
    region.srcSubresource.baseArrayLayer = srcLayer;
    region.srcSubresource.layerCount     = 1;
    region.dstSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    region.dstSubresource.mipLevel       = dstMipmap;
    region.dstSubresource.baseArrayLayer = dstLayer;
    region.dstSubresource.layerCount     = 1;
    region.extent.width  = std::max(1u, srcTexture->imageCI.extent.width >> srcMipmap);
    region.extent.height = std::max(1u, srcTexture->imageCI.extent.height >> srcMipmap);
    region.extent.depth  = std::max(1u, srcTexture->imageCI.extent.depth >> srcMipmap);
    vkCmdResolveImage(m_cmdBuffer->GetVkHandle(), srcTexture->image,
                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dstTexture->image,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
}

void VulkanCommandList::BindIndexBuffer(BufferHandle bufferHandle,
                                        DataFormat format,
                                        uint32_t offset)
{
    VulkanBuffer* indexBuffer = reinterpret_cast<VulkanBuffer*>(bufferHandle.value);
    VkIndexType indexType =
        format == DataFormat::eR16UInt ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
    vkCmdBindIndexBuffer(m_cmdBuffer->GetVkHandle(), indexBuffer->buffer, offset, indexType);
}

void VulkanCommandList::BindVertexBuffers(VectorView<BufferHandle> bufferHandles,
                                          const uint64_t* offsets)
{
    std::vector<VkBuffer> buffers;
    for (BufferHandle handle : bufferHandles)
    {
        VulkanBuffer* vulkanBuffer = reinterpret_cast<VulkanBuffer*>(handle.value);
        buffers.push_back(vulkanBuffer->buffer);
    }
    vkCmdBindVertexBuffers(m_cmdBuffer->GetVkHandle(), 0, bufferHandles.size(), buffers.data(),
                           offsets);
}

void VulkanCommandList::BindGfxPipeline(PipelineHandle pipelineHandle)
{
    VulkanPipeline* vulkanPipeline = reinterpret_cast<VulkanPipeline*>(pipelineHandle.value);
    if (vulkanPipeline->descriptorSetCount > 0)
    {
        std::vector<VkDescriptorSet> descriptorSets;
        descriptorSets.reserve(vulkanPipeline->descriptorSetCount);
        for (uint32_t i = 0; i < vulkanPipeline->descriptorSetCount; i++)
        {
            descriptorSets.push_back(vulkanPipeline->descriptorSets[i]->descriptorSet);
        }
        vkCmdBindDescriptorSets(m_cmdBuffer->GetVkHandle(), VK_PIPELINE_BIND_POINT_GRAPHICS,
                                vulkanPipeline->pipelineLayout, 0,
                                vulkanPipeline->descriptorSetCount, descriptorSets.data(), 0,
                                nullptr);
    }
    vkCmdBindPipeline(m_cmdBuffer->GetVkHandle(), VK_PIPELINE_BIND_POINT_GRAPHICS,
                      vulkanPipeline->pipeline);
}

void VulkanCommandList::BindComputePipeline(PipelineHandle pipelineHandle)
{
    VulkanPipeline* vulkanPipeline = reinterpret_cast<VulkanPipeline*>(pipelineHandle.value);
    if (vulkanPipeline->descriptorSetCount > 0)
    {
        std::vector<VkDescriptorSet> descriptorSets;
        descriptorSets.reserve(vulkanPipeline->descriptorSetCount);
        for (uint32_t i = 0; i < vulkanPipeline->descriptorSetCount; i++)
        {
            descriptorSets.push_back(vulkanPipeline->descriptorSets[i]->descriptorSet);
        }
        //        for (VulkanDescriptorSet* ds : vulkanPipeline->descriptorSets)
        //        {
        //            descriptorSets.push_back(ds->descriptorSet);
        //        }
        vkCmdBindDescriptorSets(m_cmdBuffer->GetVkHandle(), VK_PIPELINE_BIND_POINT_COMPUTE,
                                vulkanPipeline->pipelineLayout, 0,
                                vulkanPipeline->descriptorSetCount, descriptorSets.data(), 0,
                                nullptr);
    }
    vkCmdBindPipeline(m_cmdBuffer->GetVkHandle(), VK_PIPELINE_BIND_POINT_COMPUTE,
                      vulkanPipeline->pipeline);
}

void VulkanCommandList::BeginRenderPass(RenderPassHandle renderPassHandle,
                                        FramebufferHandle framebufferHandle,
                                        const Rect2<int>& area,
                                        VectorView<RenderPassClearValue> clearValues)
{
    VkFramebuffer framebuffer =
        reinterpret_cast<VulkanFramebuffer*>(framebufferHandle.value)->GetVkHandle();
    VkRenderPassBeginInfo rpBeginInfo;
    InitVkStruct(rpBeginInfo, VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO);
    rpBeginInfo.renderPass               = reinterpret_cast<VkRenderPass>(renderPassHandle.value);
    rpBeginInfo.framebuffer              = framebuffer;
    rpBeginInfo.renderArea.offset.x      = area.minX;
    rpBeginInfo.renderArea.offset.y      = area.minY;
    rpBeginInfo.renderArea.extent.width  = area.Width();
    rpBeginInfo.renderArea.extent.height = area.Height();
    rpBeginInfo.clearValueCount          = clearValues.size();
    rpBeginInfo.pClearValues = reinterpret_cast<const VkClearValue*>(clearValues.data());

    vkCmdBeginRenderPass(m_cmdBuffer->GetVkHandle(), &rpBeginInfo, VK_SUBPASS_CONTENTS_INLINE);
}

void VulkanCommandList::EndRenderPass()
{
    vkCmdEndRenderPass(m_cmdBuffer->GetVkHandle());
}

void VulkanCommandList::BeginRenderPassDynamic(const RenderPassLayout& rpLayout,
                                               const Rect2<int>& area,
                                               VectorView<RenderPassClearValue> clearValues)
{
    const std::vector<RenderTarget>& colorRTs = rpLayout.GetColorRenderTargets();
    VkRenderingInfoKHR renderingInfo{};
    InitVkStruct(renderingInfo, VK_STRUCTURE_TYPE_RENDERING_INFO_KHR);
    renderingInfo.layerCount               = 1;
    renderingInfo.viewMask                 = 0;
    renderingInfo.flags                    = 0;
    renderingInfo.renderArea.offset.x      = area.minX;
    renderingInfo.renderArea.offset.y      = area.minY;
    renderingInfo.renderArea.extent.width  = area.Width();
    renderingInfo.renderArea.extent.height = area.Height();

    std::vector<VkRenderingAttachmentInfoKHR> colorAttachments;
    colorAttachments.reserve(colorRTs.size());

    for (uint32_t i = 0; i < colorRTs.size(); i++)
    {
        VulkanTexture* vulkanTexture =
            reinterpret_cast<VulkanTexture*>(rpLayout.GetRenderTargetHandles()[i].value);
        VkRenderingAttachmentInfoKHR colorAttachment{};
        InitVkStruct(colorAttachment, VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR);
        colorAttachment.imageView   = vulkanTexture->imageView;
        colorAttachment.imageLayout = ToVkImageLayout(TextureUsageToLayout(colorRTs[i].usage));
        colorAttachment.loadOp      = ToVkAttachmentLoadOp(rpLayout.GetColorRenderTargetLoadOp());
        colorAttachment.storeOp     = ToVkAttachmentStoreOp(rpLayout.GetColorRenderTargetStoreOp());
        colorAttachment.clearValue.color = ToVkClearColor(clearValues[i]);
        colorAttachments.emplace_back(colorAttachment);
    }
    renderingInfo.colorAttachmentCount = static_cast<uint32_t>(colorAttachments.size());
    renderingInfo.pColorAttachments    = colorAttachments.data();

    VkRenderingAttachmentInfoKHR depthStencilAttachment;
    InitVkStruct(depthStencilAttachment, VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR);
    if (rpLayout.HasDepthStencilRenderTarget())
    {
        VulkanTexture* vulkanTexture =
            reinterpret_cast<VulkanTexture*>(rpLayout.GetDepthStencilRenderTargetHandle()->value);
        depthStencilAttachment.imageView = vulkanTexture->imageView;
        depthStencilAttachment.imageLayout =
            ToVkImageLayout(TextureUsageToLayout(rpLayout.GetDepthStencilRenderTarget().usage));
        depthStencilAttachment.loadOp =
            ToVkAttachmentLoadOp(rpLayout.GetDepthStencilRenderTargetLoadOp());
        depthStencilAttachment.storeOp =
            ToVkAttachmentStoreOp(rpLayout.GetDepthStencilRenderTargetStoreOp());
        depthStencilAttachment.clearValue.depthStencil = ToVkClearDepthStencil(clearValues.back());

        renderingInfo.pDepthAttachment = &depthStencilAttachment;
    }

    vkCmdBeginRenderingKHR(m_cmdBuffer->GetVkHandle(), &renderingInfo);
}

void VulkanCommandList::EndRenderPassDynamic()
{
    vkCmdEndRenderingKHR(m_cmdBuffer->GetVkHandle());
}

void VulkanCommandList::Draw(uint32_t vertexCount,
                             uint32_t instanceCount,
                             uint32_t firstVertex,
                             uint32_t firstInstance)
{
    vkCmdDraw(m_cmdBuffer->GetVkHandle(), vertexCount, instanceCount, firstVertex, firstInstance);
}

void VulkanCommandList::DrawIndexed(uint32_t indexCount,
                                    uint32_t instanceCount,
                                    uint32_t firstIndex,
                                    uint32_t vertexOffset,
                                    uint32_t firstInstance)
{
    vkCmdDrawIndexed(m_cmdBuffer->GetVkHandle(), indexCount, instanceCount, firstIndex,
                     vertexOffset, firstInstance);
}

void VulkanCommandList::SetPushConstants(PipelineHandle pipelineHandle, VectorView<uint8_t> data)
{
    VulkanPipeline* pipeline = reinterpret_cast<VulkanPipeline*>(pipelineHandle.value);
    vkCmdPushConstants(m_cmdBuffer->GetVkHandle(), pipeline->pipelineLayout,
                       pipeline->pushConstantsStageFlags, 0, data.size(), data.data());
}

void VulkanCommandList::SetViewports(VectorView<Rect2<float>> viewports)
{
    std::vector<VkViewport> vkViewports;
    vkViewports.resize(viewports.size());
    for (uint32_t i = 0; i < viewports.size(); i++)
    {
        vkViewports[i]          = {};
        vkViewports[i].x        = viewports[i].minX;
        vkViewports[i].y        = viewports[i].minY;
        vkViewports[i].width    = viewports[i].Width();
        vkViewports[i].height   = viewports[i].Height();
        vkViewports[i].minDepth = 0.0f;
        vkViewports[i].maxDepth = 1.0f;
    }
    vkCmdSetViewport(m_cmdBuffer->GetVkHandle(), 0, viewports.size(), vkViewports.data());
}

void VulkanCommandList::SetScissors(VectorView<Rect2<int>> scissors)
{
    std::vector<VkRect2D> vkScissors;
    vkScissors.resize(scissors.size());
    for (uint32_t i = 0; i < scissors.size(); i++)
    {
        vkScissors[i]               = {};
        vkScissors[i].extent.width  = scissors[i].Width();
        vkScissors[i].extent.height = scissors[i].Height();
        vkScissors[i].offset        = {};
    }
    vkCmdSetScissor(m_cmdBuffer->GetVkHandle(), 0, vkScissors.size(), vkScissors.data());
}

void VulkanCommandList::SetDepthBias(float depthBiasConstantFactor,
                                     float depthBiasClamp,
                                     float depthBiasSlopeFactor)
{
    vkCmdSetDepthBias(m_cmdBuffer->GetVkHandle(), depthBiasConstantFactor, depthBiasClamp,
                      depthBiasSlopeFactor);
}

void VulkanCommandList::SetLineWidth(float width)
{
    vkCmdSetLineWidth(m_cmdBuffer->GetVkHandle(), width);
}

void VulkanCommandList::SetBlendConstants(const Color& color)
{
    float constants[4] = {color.r, color.g, color.b, color.a};
    vkCmdSetBlendConstants(m_cmdBuffer->GetVkHandle(), constants);
}
} // namespace zen::rhi