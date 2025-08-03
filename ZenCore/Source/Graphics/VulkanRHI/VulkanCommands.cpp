#include "Graphics/VulkanRHI/VulkanCommands.h"
#include "Graphics/VulkanRHI/VulkanBuffer.h"
#include "Graphics/VulkanRHI/VulkanCommandBuffer.h"
#include "Graphics/VulkanRHI/VulkanCommon.h"
#include "Graphics/VulkanRHI/VulkanDevice.h"
#include "Graphics/VulkanRHI/VulkanPipeline.h"
#include "Graphics/VulkanRHI/VulkanRenderPass.h"
#include "Graphics/VulkanRHI/VulkanSynchronization.h"
#include "Graphics/VulkanRHI/VulkanTexture.h"
#include "Graphics/VulkanRHI/VulkanTypes.h"

namespace zen::rhi
{
VulkanCommandList::VulkanCommandList(VulkanCommandListContext* context) :
    m_vkRHI(context->GetVkRHI()), m_cmdBufferManager(context->GetCmdBufferManager())
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
    VulkanPipelineBarrier barrier;

    for (const auto& memoryTransition : memoryTransitions)
    {
        barrier.AddMemoryBarrier(ToVkAccessFlags(memoryTransition.srcAccess),
                                 ToVkAccessFlags(memoryTransition.dstAccess));
    }

    for (const auto& bufferTransition : bufferTransitions)
    {
        VulkanBuffer* vulkanBuffer = TO_VK_BUFFER(bufferTransition.bufferHandle);
        VkAccessFlags srcAccess =
            BufferUsageToAccessFlagBits(bufferTransition.oldUsage, bufferTransition.oldAccessMode);
        VkAccessFlags dstAccess =
            BufferUsageToAccessFlagBits(bufferTransition.newUsage, bufferTransition.newAccessMode);
        barrier.AddBufferBarrier(vulkanBuffer->buffer, bufferTransition.offset,
                                 bufferTransition.size, srcAccess, dstAccess);
    }

    for (const auto& textureTransition : textureTransitions)
    {
        VulkanTexture* vulkanTexture = TO_VK_TEXTURE(textureTransition.textureHandle);

        VkAccessFlags srcAccess = ToVkAccessFlags(TextureUsageToAccessFlagBits(
            textureTransition.oldUsage, textureTransition.oldAccessMode));
        VkAccessFlags dstAccess = ToVkAccessFlags(TextureUsageToAccessFlagBits(
            textureTransition.newUsage, textureTransition.newAccessMode));
        // VkImageLayout oldLayout =
        //     ToVkImageLayout(TextureUsageToLayout(textureTransition.oldUsage));
        VkImageLayout oldLayout = m_vkRHI->GetImageCurrentLayout(vulkanTexture->image);
        VkImageLayout newLayout = ToVkImageLayout(TextureUsageToLayout(textureTransition.newUsage));
        // filter
        if (oldLayout == newLayout)
        {
            continue;
        }
        VkImageSubresourceRange subresourceRange{};
        subresourceRange.aspectMask = ToVkAspectFlags(textureTransition.subResourceRange.aspect);
        subresourceRange.layerCount = textureTransition.subResourceRange.layerCount;
        subresourceRange.levelCount = textureTransition.subResourceRange.levelCount;
        subresourceRange.baseArrayLayer = textureTransition.subResourceRange.baseArrayLayer;
        subresourceRange.baseMipLevel   = textureTransition.subResourceRange.baseMipLevel;

        barrier.AddImageBarrier(vulkanTexture->image, oldLayout, newLayout, subresourceRange,
                                srcAccess, dstAccess);
        m_vkRHI->UpdateImageLayout(vulkanTexture->image, newLayout);
    }
    barrier.Execute(m_cmdBufferManager->GetActiveCommandBufferDirect(), srcStages, dstStages);
}

void VulkanCommandList::ClearBuffer(BufferHandle bufferHandle, uint32_t offset, uint32_t size)
{
    VulkanBuffer* vulkanBuffer = TO_VK_BUFFER(bufferHandle);
    vkCmdFillBuffer(m_cmdBuffer->GetVkHandle(), vulkanBuffer->buffer, offset, size, 0);
}

void VulkanCommandList::CopyBuffer(BufferHandle srcBufferHandle,
                                   BufferHandle dstBufferHandle,
                                   const BufferCopyRegion& region)
{
    VulkanBuffer* srcBuffer = TO_VK_BUFFER(srcBufferHandle);
    VulkanBuffer* dstBuffer = TO_VK_BUFFER(dstBufferHandle);
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
    VulkanTexture* texture = TO_VK_TEXTURE(textureHandle);
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
    VulkanTexture* srcTexture = TO_VK_TEXTURE(srcTextureHandle);
    VulkanTexture* dstTexture = TO_VK_TEXTURE(dstTextureHandle);
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
    VulkanTexture* srcTexture = TO_VK_TEXTURE(textureHandle);
    VulkanBuffer* dstBuffer   = TO_VK_BUFFER(bufferHandle);

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
    VulkanBuffer* srcBuffer   = TO_VK_BUFFER(bufferHandle);
    VulkanTexture* dstTexture = TO_VK_TEXTURE(textureHandle);

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
    VulkanTexture* srcTexture = TO_VK_TEXTURE(srcTextureHandle);
    VulkanTexture* dstTexture = TO_VK_TEXTURE(dstTextureHandle);
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
    VulkanBuffer* indexBuffer = TO_VK_BUFFER(bufferHandle);
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
        VulkanBuffer* vulkanBuffer = TO_VK_BUFFER(handle);
        buffers.push_back(vulkanBuffer->buffer);
    }
    vkCmdBindVertexBuffers(m_cmdBuffer->GetVkHandle(), 0, bufferHandles.size(), buffers.data(),
                           offsets);
}

void VulkanCommandList::BindGfxPipeline(PipelineHandle pipelineHandle)
{
    VulkanPipeline* vulkanPipeline = TO_VK_PIPELINE(pipelineHandle);
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
    VulkanPipeline* vulkanPipeline = TO_VK_PIPELINE(pipelineHandle);
    if (vulkanPipeline->descriptorSetCount > 0)
    {
        std::vector<VkDescriptorSet> descriptorSets;
        descriptorSets.reserve(vulkanPipeline->descriptorSetCount);
        for (uint32_t i = 0; i < vulkanPipeline->descriptorSetCount; i++)
        {
            descriptorSets.push_back(vulkanPipeline->descriptorSets[i]->descriptorSet);
        }
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
    VkFramebuffer framebuffer = TO_VK_FRAMEBUFFER(framebufferHandle)->GetVkHandle();
    VkRenderPassBeginInfo rpBeginInfo;
    InitVkStruct(rpBeginInfo, VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO);
    rpBeginInfo.renderPass               = TO_VK_RENDER_PASS(renderPassHandle);
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
        VulkanTexture* vulkanTexture = TO_VK_TEXTURE(rpLayout.GetRenderTargetHandles()[i]);
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
        VulkanTexture* vulkanTexture = TO_VK_TEXTURE(rpLayout.GetDepthStencilRenderTargetHandle());
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

void VulkanCommandList::DrawIndexedIndirect(BufferHandle indirectBuffer,
                                            uint32_t offset,
                                            uint32_t drawCount,
                                            uint32_t stride)
{
    VulkanBuffer* vulkanBuffer = TO_VK_BUFFER(indirectBuffer);
    vkCmdDrawIndexedIndirect(m_cmdBuffer->GetVkHandle(), vulkanBuffer->buffer, offset, drawCount,
                             stride);
}


void VulkanCommandList::Dispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ)
{
    vkCmdDispatch(m_cmdBuffer->GetVkHandle(), groupCountX, groupCountY, groupCountZ);
}

void VulkanCommandList::DispatchIndirect(BufferHandle indirectBuffer, uint32_t offset)
{
    VulkanBuffer* vulkanBuffer = TO_VK_BUFFER(indirectBuffer);
    vkCmdDispatchIndirect(m_cmdBuffer->GetVkHandle(), vulkanBuffer->buffer, offset);
}

void VulkanCommandList::SetPushConstants(PipelineHandle pipelineHandle, VectorView<uint8_t> data)
{
    VulkanPipeline* pipeline = TO_VK_PIPELINE(pipelineHandle);
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

void VulkanCommandList::GenerateTextureMipmaps(TextureHandle textureHandle)
{
    VulkanTexture* texture = TO_VK_TEXTURE(textureHandle);
    VkImage vkImage        = texture->image;
    // Get texture attributes
    const uint32_t mipLevels = texture->imageCI.mipLevels;
    const uint32_t texWidth  = texture->imageCI.extent.width;
    const uint32_t texHeight = texture->imageCI.extent.height;

    // store image's original layout
    VkImageLayout originLayout = m_vkRHI->GetImageCurrentLayout(vkImage);
    // Transition first mip level to transfer source for read during blit
    ChangeImageLayout(vkImage, originLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      VulkanTexture::GetSubresourceRange(VK_IMAGE_ASPECT_COLOR_BIT, 0, mipLevels, 0,
                                                         texture->imageCI.arrayLayers));
    // Copy down mips from n-1 to n
    for (uint32_t i = 1; i < mipLevels; i++)
    {
        VkImageBlit imageBlit{};

        // Source
        imageBlit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        imageBlit.srcSubresource.layerCount = 1;
        imageBlit.srcSubresource.mipLevel   = i - 1;
        imageBlit.srcOffsets[1].x           = static_cast<int32_t>(texWidth >> (i - 1));
        imageBlit.srcOffsets[1].y           = static_cast<int32_t>(texHeight >> (i - 1));
        imageBlit.srcOffsets[1].z           = 1;

        // Destination
        imageBlit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        imageBlit.dstSubresource.layerCount = 1;
        imageBlit.dstSubresource.mipLevel   = i;
        imageBlit.dstOffsets[1].x           = static_cast<int32_t>(texWidth >> i);
        imageBlit.dstOffsets[1].y           = static_cast<int32_t>(texHeight >> i);
        imageBlit.dstOffsets[1].z           = 1;

        VkImageSubresourceRange mipSubRange = {};
        mipSubRange.aspectMask              = VK_IMAGE_ASPECT_COLOR_BIT;
        mipSubRange.baseMipLevel            = i;
        mipSubRange.levelCount              = 1;
        mipSubRange.layerCount              = 1;

        // Prepare current mip level as image blit destination
        ChangeImageLayout(vkImage, m_vkRHI->GetImageCurrentLayout(vkImage),
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, mipSubRange);

        // Blit from previous level
        vkCmdBlitImage(m_cmdBuffer->GetVkHandle(), vkImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       vkImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &imageBlit,
                       VK_FILTER_LINEAR);

        // Prepare the current mip level as image blit source for the next level
        ChangeImageLayout(vkImage, m_vkRHI->GetImageCurrentLayout(vkImage),
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, mipSubRange);
    }
    // After the loop, all mip layers are in TRANSFER_SRC layout,
    // need to restore its layout.
    ChangeImageLayout(vkImage, m_vkRHI->GetImageCurrentLayout(vkImage), originLayout,
                      VulkanTexture::GetSubresourceRange(VK_IMAGE_ASPECT_COLOR_BIT, 0, mipLevels, 0,
                                                         texture->imageCI.arrayLayers));
}

void VulkanCommandList::ChangeTextureLayout(TextureHandle textureHandle, TextureLayout newLayout)
{
    VulkanTexture* vkTexture = TO_VK_TEXTURE(textureHandle);
    uint32_t levelCount      = vkTexture->imageCI.mipLevels;
    uint32_t layerCount      = vkTexture->imageCI.arrayLayers;

    VkImageLayout srcLayout = m_vkRHI->GetImageCurrentLayout(vkTexture->image);
    VkImageLayout dstLayout = ToVkImageLayout(newLayout);

    VkImageAspectFlags aspectFlags = VK_IMAGE_ASPECT_COLOR_BIT;
    if (vkTexture->imageCI.usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)
    {
        aspectFlags = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    }

    VkImageSubresourceRange range =
        VulkanTexture::GetSubresourceRange(aspectFlags, 0, levelCount, 0, layerCount);
    ChangeImageLayout(vkTexture->image, srcLayout, dstLayout, range);
}

void VulkanCommandList::ChangeImageLayout(VkImage image,
                                          VkImageLayout srcLayout,
                                          VkImageLayout dstLayout,
                                          const VkImageSubresourceRange& range)
{
    VulkanPipelineBarrier barrier;
    barrier.AddImageBarrier(image, srcLayout, dstLayout, range);
    barrier.Execute(m_cmdBuffer);
    m_vkRHI->UpdateImageLayout(image, dstLayout);
}
} // namespace zen::rhi