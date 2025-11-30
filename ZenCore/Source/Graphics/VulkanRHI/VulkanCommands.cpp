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
    // VulkanCommandListContext* context = m_device->GetImmediateCmdContext();
    // return RHICommandList::Create(GraphicsAPIType::eVulkan, context);
    return m_device->GetImmediateCommandList();
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
        VulkanBuffer* vulkanBuffer = TO_VK_BUFFER(bufferTransition.buffer);
        VkAccessFlags srcAccess =
            BufferUsageToAccessFlagBits(bufferTransition.oldUsage, bufferTransition.oldAccessMode);
        VkAccessFlags dstAccess =
            BufferUsageToAccessFlagBits(bufferTransition.newUsage, bufferTransition.newAccessMode);
        barrier.AddBufferBarrier(vulkanBuffer->GetVkBuffer(), bufferTransition.offset,
                                 bufferTransition.size, srcAccess, dstAccess);
    }

    for (const auto& textureTransition : textureTransitions)
    {
        VulkanTexture* vulkanTexture = TO_VK_TEXTURE(textureTransition.texture);

        VkAccessFlags srcAccess = ToVkAccessFlags(TextureUsageToAccessFlagBits(
            textureTransition.oldUsage, textureTransition.oldAccessMode));
        VkAccessFlags dstAccess = ToVkAccessFlags(TextureUsageToAccessFlagBits(
            textureTransition.newUsage, textureTransition.newAccessMode));
        // VkImageLayout oldLayout =
        //     ToVkImageLayout(TextureUsageToLayout(textureTransition.oldUsage));
        VkImageLayout oldLayout = m_vkRHI->GetImageCurrentLayout(vulkanTexture->GetVkImage());
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

        barrier.AddImageBarrier(vulkanTexture->GetVkImage(), oldLayout, newLayout, subresourceRange,
                                srcAccess, dstAccess);
        m_vkRHI->UpdateImageLayout(vulkanTexture->GetVkImage(), newLayout);
    }
    barrier.Execute(m_cmdBufferManager->GetActiveCommandBufferDirect(), srcStages, dstStages);
}

void VulkanCommandList::ClearBuffer(RHIBuffer* buffer, uint32_t offset, uint32_t size)
{
    vkCmdFillBuffer(m_cmdBuffer->GetVkHandle(), TO_VK_BUFFER(buffer)->GetVkBuffer(), offset, size,
                    0);
}

void VulkanCommandList::CopyBuffer(RHIBuffer* srcBuffer,
                                   RHIBuffer* dstBuffer,
                                   const BufferCopyRegion& region)
{
    VkBufferCopy bufferCopy;
    bufferCopy.srcOffset = region.srcOffset;
    bufferCopy.dstOffset = region.dstOffset;
    bufferCopy.size      = region.size;

    vkCmdCopyBuffer(m_cmdBuffer->GetVkHandle(), TO_VK_BUFFER(srcBuffer)->GetVkBuffer(),
                    TO_VK_BUFFER(dstBuffer)->GetVkBuffer(), 1, &bufferCopy);
}

void VulkanCommandList::ClearTexture(RHITexture* texture,
                                     const Color& color,
                                     const TextureSubResourceRange& range)
{
    VkImageSubresourceRange vkRange;
    ToVkImageSubresourceRange(range, &vkRange);
    VkClearColorValue colorValue;
    ToVkClearColor(color, &colorValue);
    vkCmdClearColorImage(m_cmdBuffer->GetVkHandle(), TO_VK_TEXTURE(texture)->GetVkImage(),
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &colorValue, 1, &vkRange);
}

void VulkanCommandList::CopyTexture(RHITexture* srcTexture,
                                    RHITexture* dstTexture,
                                    VectorView<TextureCopyRegion> regions)
{
    std::vector<VkImageCopy> copies(regions.size());
    for (uint32_t i = 0; i < regions.size(); i++)
    {
        ToVkImageCopy(regions[i], &copies[i]);
    }

    vkCmdCopyImage(m_cmdBuffer->GetVkHandle(), TO_VK_TEXTURE(srcTexture)->GetVkImage(),
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, TO_VK_TEXTURE(dstTexture)->GetVkImage(),
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, copies.size(), copies.data());
}

void VulkanCommandList::CopyTextureToBuffer(RHITexture* texture,
                                            RHIBuffer* buffer,
                                            VectorView<BufferTextureCopyRegion> regions)
{
    std::vector<VkBufferImageCopy> copies(regions.size());
    for (uint32_t i = 0; i < regions.size(); i++)
    {
        ToVkBufferImageCopy(regions[i], &copies[i]);
    }

    vkCmdCopyImageToBuffer(m_cmdBuffer->GetVkHandle(), TO_VK_TEXTURE(texture)->GetVkImage(),
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           TO_VK_BUFFER(buffer)->GetVkBuffer(), copies.size(), copies.data());
}

void VulkanCommandList::CopyBufferToTexture(RHIBuffer* buffer,
                                            RHITexture* texture,
                                            VectorView<BufferTextureCopyRegion> regions)
{
    std::vector<VkBufferImageCopy> copies(regions.size());
    for (uint32_t i = 0; i < regions.size(); i++)
    {
        ToVkBufferImageCopy(regions[i], &copies[i]);
    }


    vkCmdCopyBufferToImage(m_cmdBuffer->GetVkHandle(), TO_VK_BUFFER(buffer)->GetVkBuffer(),
                           TO_VK_TEXTURE(texture)->GetVkImage(),
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, copies.size(), copies.data());
}

void VulkanCommandList::ResolveTexture(RHITexture* srcTexture,
                                       RHITexture* dstTexture,
                                       uint32_t srcLayer,
                                       uint32_t srcMipmap,
                                       uint32_t dstLayer,
                                       uint32_t dstMipmap)
{
    VkImageResolve region{};
    region.srcSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    region.srcSubresource.mipLevel       = srcMipmap;
    region.srcSubresource.baseArrayLayer = srcLayer;
    region.srcSubresource.layerCount     = 1;
    region.dstSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    region.dstSubresource.mipLevel       = dstMipmap;
    region.dstSubresource.baseArrayLayer = dstLayer;
    region.dstSubresource.layerCount     = 1;
    region.extent.width  = std::max(1u, srcTexture->GetBaseInfo().width >> srcMipmap);
    region.extent.height = std::max(1u, srcTexture->GetBaseInfo().height >> srcMipmap);
    region.extent.depth  = std::max(1u, srcTexture->GetBaseInfo().depth >> srcMipmap);
    vkCmdResolveImage(m_cmdBuffer->GetVkHandle(), TO_VK_TEXTURE(srcTexture)->GetVkImage(),
                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, TO_VK_TEXTURE(dstTexture)->GetVkImage(),
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
}

void VulkanCommandList::BindIndexBuffer(RHIBuffer* buffer, DataFormat format, uint32_t offset)
{
    VkIndexType indexType =
        format == DataFormat::eR16UInt ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
    vkCmdBindIndexBuffer(m_cmdBuffer->GetVkHandle(), TO_VK_BUFFER(buffer)->GetVkBuffer(), offset,
                         indexType);
}

void VulkanCommandList::BindVertexBuffers(VectorView<RHIBuffer*> buffers, const uint64_t* offsets)
{
    std::vector<VkBuffer> bufferHandles;
    for (RHIBuffer* buffer : buffers)
    {
        bufferHandles.push_back(TO_VK_BUFFER(buffer)->GetVkBuffer());
    }
    vkCmdBindVertexBuffers(m_cmdBuffer->GetVkHandle(), 0, bufferHandles.size(),
                           bufferHandles.data(), offsets);
}

// void VulkanCommandList::BindGfxPipeline(PipelineHandle pipelineHandle)
// {
//     VulkanPipeline* vulkanPipeline = TO_VK_PIPELINE(pipelineHandle);
//     if (vulkanPipeline->descriptorSetCount > 0)
//     {
//         std::vector<VkDescriptorSet> descriptorSets;
//         descriptorSets.reserve(vulkanPipeline->descriptorSetCount);
//         for (uint32_t i = 0; i < vulkanPipeline->descriptorSetCount; i++)
//         {
//             descriptorSets.push_back(vulkanPipeline->descriptorSets[i]->descriptorSet);
//         }
//         vkCmdBindDescriptorSets(m_cmdBuffer->GetVkHandle(), VK_PIPELINE_BIND_POINT_GRAPHICS,
//                                 vulkanPipeline->pipelineLayout, 0,
//                                 vulkanPipeline->descriptorSetCount, descriptorSets.data(), 0,
//                                 nullptr);
//     }
//     vkCmdBindPipeline(m_cmdBuffer->GetVkHandle(), VK_PIPELINE_BIND_POINT_GRAPHICS,
//                       vulkanPipeline->pipeline);
// }

void VulkanCommandList::BindGfxPipeline(RHIPipeline* pipelineHandle,
                                        const std::vector<DescriptorSetHandle>& descriptorSets)
{
    VulkanPipeline* vulkanPipeline = TO_VK_PIPELINE(pipelineHandle);
    if (!descriptorSets.empty())
    {
        std::vector<VkDescriptorSet> vkDescriptorSets;
        vkDescriptorSets.reserve(descriptorSets.size());
        for (uint32_t i = 0; i < descriptorSets.size(); i++)
        {
            vkDescriptorSets.push_back(
                reinterpret_cast<VulkanDescriptorSet*>(descriptorSets[i].value)->descriptorSet);
        }
        vkCmdBindDescriptorSets(m_cmdBuffer->GetVkHandle(), VK_PIPELINE_BIND_POINT_GRAPHICS,
                                vulkanPipeline->GetVkPipelineLayout(), 0, vkDescriptorSets.size(),
                                vkDescriptorSets.data(), 0, nullptr);
    }
    vkCmdBindPipeline(m_cmdBuffer->GetVkHandle(), VK_PIPELINE_BIND_POINT_GRAPHICS,
                      vulkanPipeline->GetVkPipeline());
}

void VulkanCommandList::BindComputePipeline(RHIPipeline* pipelineHandle,
                                            const std::vector<DescriptorSetHandle>& descriptorSets)
{
    VulkanPipeline* vulkanPipeline = TO_VK_PIPELINE(pipelineHandle);
    if (!descriptorSets.empty())
    {
        std::vector<VkDescriptorSet> vkDescriptorSets;
        vkDescriptorSets.reserve(descriptorSets.size());
        for (uint32_t i = 0; i < descriptorSets.size(); i++)
        {
            VkDescriptorSet vkDescriptorSet =
                reinterpret_cast<VulkanDescriptorSet*>(descriptorSets[i].value)->descriptorSet;
            vkDescriptorSets.push_back(vkDescriptorSet);
        }
        vkCmdBindDescriptorSets(m_cmdBuffer->GetVkHandle(), VK_PIPELINE_BIND_POINT_COMPUTE,
                                vulkanPipeline->GetVkPipelineLayout(), 0, vkDescriptorSets.size(),
                                vkDescriptorSets.data(), 0, nullptr);
    }
    vkCmdBindPipeline(m_cmdBuffer->GetVkHandle(), VK_PIPELINE_BIND_POINT_COMPUTE,
                      vulkanPipeline->GetVkPipeline());
}


// void VulkanCommandList::BindComputePipeline(PipelineHandle pipelineHandle)
// {
//     VulkanPipeline* vulkanPipeline = TO_VK_PIPELINE(pipelineHandle);
//     if (vulkanPipeline->descriptorSetCount > 0)
//     {
//         std::vector<VkDescriptorSet> descriptorSets;
//         descriptorSets.reserve(vulkanPipeline->descriptorSetCount);
//         for (uint32_t i = 0; i < vulkanPipeline->descriptorSetCount; i++)
//         {
//             descriptorSets.push_back(vulkanPipeline->descriptorSets[i]->descriptorSet);
//         }
//         vkCmdBindDescriptorSets(m_cmdBuffer->GetVkHandle(), VK_PIPELINE_BIND_POINT_COMPUTE,
//                                 vulkanPipeline->pipelineLayout, 0,
//                                 vulkanPipeline->descriptorSetCount, descriptorSets.data(), 0,
//                                 nullptr);
//     }
//     vkCmdBindPipeline(m_cmdBuffer->GetVkHandle(), VK_PIPELINE_BIND_POINT_COMPUTE,
//                       vulkanPipeline->pipeline);
// }

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
        const RenderTarget& colorRT  = colorRTs[i];
        VulkanTexture* vulkanTexture = TO_VK_TEXTURE(colorRT.texture);
        VkRenderingAttachmentInfoKHR colorAttachment{};
        InitVkStruct(colorAttachment, VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR);
        colorAttachment.imageView        = vulkanTexture->GetVkImageView();
        colorAttachment.imageLayout      = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorAttachment.loadOp           = ToVkAttachmentLoadOp(colorRT.loadOp);
        colorAttachment.storeOp          = ToVkAttachmentStoreOp(colorRT.storeOp);
        colorAttachment.clearValue.color = ToVkClearColor(clearValues[i]);
        colorAttachments.emplace_back(colorAttachment);
    }
    renderingInfo.colorAttachmentCount = static_cast<uint32_t>(colorAttachments.size());
    renderingInfo.pColorAttachments    = colorAttachments.data();

    VkRenderingAttachmentInfoKHR depthStencilAttachment;
    InitVkStruct(depthStencilAttachment, VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR);
    if (rpLayout.HasDepthStencilRenderTarget())
    {
        const RenderTarget& depthStencilRT = rpLayout.GetDepthStencilRenderTarget();
        VulkanTexture* vulkanTexture       = TO_VK_TEXTURE(depthStencilRT.texture);
        depthStencilAttachment.imageView   = vulkanTexture->GetVkImageView();
        depthStencilAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depthStencilAttachment.loadOp      = ToVkAttachmentLoadOp(depthStencilRT.loadOp);
        depthStencilAttachment.storeOp     = ToVkAttachmentStoreOp(depthStencilRT.storeOp);
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
                                    int32_t vertexOffset,
                                    uint32_t firstInstance)
{
    vkCmdDrawIndexed(m_cmdBuffer->GetVkHandle(), indexCount, instanceCount, firstIndex,
                     vertexOffset, firstInstance);
}

void VulkanCommandList::DrawIndexedIndirect(RHIBuffer* indirectBuffer,
                                            uint32_t offset,
                                            uint32_t drawCount,
                                            uint32_t stride)
{
    VulkanBuffer* vulkanBuffer = TO_VK_BUFFER(indirectBuffer);
    vkCmdDrawIndexedIndirect(m_cmdBuffer->GetVkHandle(), vulkanBuffer->GetVkBuffer(), offset,
                             drawCount, stride);
}


void VulkanCommandList::Dispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ)
{
    vkCmdDispatch(m_cmdBuffer->GetVkHandle(), groupCountX, groupCountY, groupCountZ);
}

void VulkanCommandList::DispatchIndirect(RHIBuffer* indirectBuffer, uint32_t offset)
{
    VulkanBuffer* vulkanBuffer = TO_VK_BUFFER(indirectBuffer);
    vkCmdDispatchIndirect(m_cmdBuffer->GetVkHandle(), vulkanBuffer->GetVkBuffer(), offset);
}

void VulkanCommandList::SetPushConstants(RHIPipeline* pipelineHandle, VectorView<uint8_t> data)
{
    VulkanPipeline* pipeline = TO_VK_PIPELINE(pipelineHandle);
    vkCmdPushConstants(m_cmdBuffer->GetVkHandle(), pipeline->GetVkPipelineLayout(),
                       pipeline->GetPushConstantsStageFlags(), 0, data.size(), data.data());
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

void VulkanCommandList::GenerateTextureMipmaps(RHITexture* texture)
{
    VulkanTexture* vulkanTexture = TO_VK_TEXTURE(texture);
    VkImage vkImage              = vulkanTexture->GetVkImage();
    // Get texture attributes
    // const uint32_t mipLevels = vulkanTexture->getv.mipLevels;
    const uint32_t texWidth  = texture->GetBaseInfo().width;
    const uint32_t texHeight = texture->GetBaseInfo().height;

    // store image's original layout
    VkImageLayout originLayout = m_vkRHI->GetImageCurrentLayout(vkImage);
    // Transition first mip level to transfer source for read during blit
    ChangeImageLayout(vkImage, originLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      vulkanTexture->GetVkSubresourceRange());
    // Copy down mips from n-1 to n
    for (uint32_t i = 1; i < texture->GetBaseInfo().mipmaps; i++)
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
                      vulkanTexture->GetVkSubresourceRange());
}

void VulkanCommandList::ChangeTextureLayout(RHITexture* texture, TextureLayout newLayout)
{
    VulkanTexture* vkTexture = TO_VK_TEXTURE(texture);

    // const VkImageCreateInfo& imageCI = vk
    //
    //     uint32_t levelCount = vkTexture->imageCI.mipLevels;
    // uint32_t layerCount     = vkTexture->imageCI.arrayLayers;

    VkImageLayout srcLayout = m_vkRHI->GetImageCurrentLayout(vkTexture->GetVkImage());
    VkImageLayout dstLayout = ToVkImageLayout(newLayout);

    // VkImageAspectFlags aspectFlags = VK_IMAGE_ASPECT_COLOR_BIT;
    // if (vkTexture->GetVkImageUsage() & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)
    // {
    //     aspectFlags = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    // }
    //
    // VkImageSubresourceRange range =
    //     VulkanTexture::GetVkSubresourceRange(aspectFlags, 0, levelCount, 0, layerCount);
    ChangeImageLayout(vkTexture->GetVkImage(), srcLayout, dstLayout,
                      vkTexture->GetVkSubresourceRange());
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