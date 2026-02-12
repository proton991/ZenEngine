#include "Graphics/RHI/RHIOptions.h"
#include "Graphics/VulkanRHI/VulkanTexture.h"
#include "Graphics/VulkanRHI/VulkanBuffer.h"
#include "Graphics/VulkanRHI/VulkanPipeline.h"
#include "Graphics/VulkanRHI/VulkanCommandList.h"
#include "Graphics/VulkanRHI/VulkanCommon.h"
#include "Graphics/VulkanRHI/VulkanDevice.h"
#include "Graphics/VulkanRHI/VulkanQueue.h"
#include "Graphics/VulkanRHI/VulkanRHI.h"
#include "Graphics/VulkanRHI/VulkanSynchronization.h"
#include "Graphics/VulkanRHI/VulkanTypes.h"
#include "Platform/Timer.h"

namespace zen
{

void FVulkanCommandBuffer::Begin()
{
    if (m_state == State::eNeedReset)
    {
        vkResetCommandBuffer(m_vkHandle, VK_COMMAND_BUFFER_RESET_RELEASE_RESOURCES_BIT);
    }
    else
    {
        VERIFY_EXPR_MSG(m_state == State::eReadyForBegin,
                        "Incorrect command buffer state when beginning command buffer");
    }

    m_state = State::eIsInsideBegin;
    VkCommandBufferBeginInfo beginInfo;
    InitVkStruct(beginInfo, VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VKCHECK(vkBeginCommandBuffer(m_vkHandle, &beginInfo));
}

void FVulkanCommandBuffer::End()
{
    VERIFY_EXPR_MSG(IsOutsideRenderPass(), "Can't end command buffer inside a render pass");
    vkEndCommandBuffer(m_vkHandle);
    m_state = State::eHasEnded;
}

void FVulkanCommandBuffer::BeginRendering(const VkRenderingInfo* pRenderingInfo)
{
    vkCmdBeginRendering(m_vkHandle, pRenderingInfo);
    m_state              = State::eIsInsideRenderPass;
    m_lastRenderingFlags = pRenderingInfo->flags;
}

void FVulkanCommandBuffer::EndRendering()
{
    vkCmdEndRendering(m_vkHandle);
    m_state = State::eIsInsideBegin;
}

void FVulkanCommandBuffer::BeginRenderPass(const VkRenderPassBeginInfo* pBeginInfo)
{
    vkCmdBeginRenderPass(m_vkHandle, pBeginInfo, VK_SUBPASS_CONTENTS_INLINE);
    m_state = State::eIsInsideRenderPass;
}

void FVulkanCommandBuffer::EndRenderPass()
{
    vkCmdEndRenderPass(m_vkHandle);
    m_state = State::eIsInsideBegin;
}

void FVulkanCommandBuffer::SetSubmitted()
{
    LockAuto lock(m_pCmdBufferPool->GetMutex());
    m_state      = State::eSubmitted;
    m_submitTime = platform::Timer::Now<>();
}

VulkanCommandBufferType FVulkanCommandBuffer::GetCommandBufferType() const
{
    return m_pCmdBufferPool->GetCommandBufferType();
}

FVulkanCommandBuffer::FVulkanCommandBuffer(FVulkanCommandBufferPool* pPool) :
    m_pCmdBufferPool(pPool)
{
    AllocMemory();
}

FVulkanCommandBuffer::~FVulkanCommandBuffer()
{
    if (m_state != State::eNotAllocated)
    {
        FreeMemory();
    }
}

void FVulkanCommandBuffer::AllocMemory()
{
    VkCommandBufferAllocateInfo allocInfo;
    InitVkStruct(allocInfo, VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO);
    allocInfo.commandPool        = m_pCmdBufferPool->GetVkHandle();
    allocInfo.commandBufferCount = 1;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; // todo: support secondary commandbuffer

    VKCHECK(vkAllocateCommandBuffers(GVulkanRHI->GetVkDevice(), &allocInfo, &m_vkHandle))

    m_state = State::eReadyForBegin;
}

void FVulkanCommandBuffer::FreeMemory()
{
    vkFreeCommandBuffers(GVulkanRHI->GetVkDevice(), m_pCmdBufferPool->GetVkHandle(), 1,
                         &m_vkHandle);
    m_state    = State::eNotAllocated;
    m_vkHandle = VK_NULL_HANDLE;
}

FVulkanCommandBufferPool::FVulkanCommandBufferPool(VulkanQueue* pQueue,
                                                   VulkanCommandBufferType type) :
    m_pQueue(pQueue), m_type(type)
{
    VkCommandPoolCreateInfo cmdPoolCI;
    InitVkStruct(cmdPoolCI, VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO);
    cmdPoolCI.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; // reset cmd buffer
    cmdPoolCI.queueFamilyIndex = pQueue->GetFamilyIndex();
    VKCHECK(vkCreateCommandPool(GVulkanRHI->GetVkDevice(), &cmdPoolCI, nullptr, &m_vkHandle));
}

FVulkanCommandBufferPool::~FVulkanCommandBufferPool()
{
    for (uint32_t i = 0; i < m_cmdBuffersInUse.size(); i++)
    {
        FVulkanCommandBuffer* pCmdBuffer = m_cmdBuffersInUse[i];
        pCmdBuffer->~FVulkanCommandBuffer();
        ZEN_MEM_FREE(pCmdBuffer);
        //ZEN_DELETE(pCmdBuffer);
    }

    for (uint32_t i = 0; i < m_cmdBuffersFree.size(); i++)
    {
        FVulkanCommandBuffer* pCmdBuffer = m_cmdBuffersFree[i];
        pCmdBuffer->~FVulkanCommandBuffer();
        ZEN_MEM_FREE(pCmdBuffer);
        //ZEN_DELETE(pCmdBuffer);
    }

    vkDestroyCommandPool(GVulkanRHI->GetVkDevice(), m_vkHandle, nullptr);
}

void FVulkanCommandBufferPool::FreeUnusedCommandBuffers()
{
    LockAuto lock(&m_mutex);

    const double currentTime = platform::Timer::Now<>();

    auto it = m_cmdBuffersInUse.end();
    while (it != m_cmdBuffersInUse.begin())
    {
        --it;
        FVulkanCommandBuffer* pCmdBuffer = *it;
        if ((pCmdBuffer->m_state == FVulkanCommandBuffer::State::eReadyForBegin ||
             pCmdBuffer->m_state == FVulkanCommandBuffer::State::eNeedReset) &&
            (currentTime - pCmdBuffer->m_submitTime) > 10.0f)
        {
            pCmdBuffer->FreeMemory();
            it = m_cmdBuffersInUse.erase(it);
            m_cmdBuffersFree.push_back(pCmdBuffer);
        }
    }
}

FVulkanCommandBuffer* FVulkanCommandBufferPool::CreateCmdBuffer()
{
    if (!m_cmdBuffersFree.empty())
    {
        FVulkanCommandBuffer* pCmdBuffer = m_cmdBuffersFree[0];
        m_cmdBuffersFree.remove(0);
        pCmdBuffer->AllocMemory();
        m_cmdBuffersInUse.emplace_back(pCmdBuffer);
        return pCmdBuffer;
    }

    FVulkanCommandBuffer* pCmdBuffer =
        static_cast<FVulkanCommandBuffer*>(ZEN_MEM_ALLOC(sizeof(FVulkanCommandBuffer)));

    new (pCmdBuffer) FVulkanCommandBuffer(this);

    m_cmdBuffersInUse.emplace_back(pCmdBuffer);

    return pCmdBuffer;
}

VulkanWorkload::~VulkanWorkload()
{
    // release semaphores and fence
}

void VulkanCommandContextBase::SetupNewCommandBuffer()
{
    LockAuto lock(&m_pCmdBufferPool->m_mutex);

    FVulkanCommandBuffer* pCmdBuffer = nullptr;

    for (uint32_t i = 0; i < m_pCmdBufferPool->m_cmdBuffersInUse.size(); i++)
    {
        FVulkanCommandBuffer* pCurrent = m_pCmdBufferPool->m_cmdBuffersInUse[i];
        if (pCurrent->m_state == FVulkanCommandBuffer::State::eReadyForBegin ||
            pCurrent->m_state == FVulkanCommandBuffer::State::eNeedReset)
        {
            pCmdBuffer = pCurrent;
        }
        else
        {
            VERIFY_EXPR(pCurrent->IsSubmitted() || pCurrent->HasEnded());
        }
    }
    if (!pCmdBuffer)
    {
        pCmdBuffer = m_pCmdBufferPool->CreateCmdBuffer();
    }

    m_pCurrentWorkload->m_pCmdBuffer = pCmdBuffer;
    pCmdBuffer->Begin();
}

void VulkanCommandContextBase::StartWorkload()
{
    m_pCurrentWorkload = ZEN_NEW() VulkanWorkload(m_pQueue);
}

void VulkanCommandContextBase::EndWorkload()
{
    if (m_pCurrentWorkload != nullptr)
    {
        FVulkanCommandBuffer* pCommandBuffer = m_pCurrentWorkload->m_pCmdBuffer;
        if (pCommandBuffer != nullptr)
        {
            if (!pCommandBuffer->HasEnded() &&
                pCommandBuffer->GetCommandBufferType() == VulkanCommandBufferType::ePrimary)
            {
                if (RHIOptions::GetInstance().UseDynamicRendering())
                {
                    pCommandBuffer->EndRendering();
                }
                else
                {
                    pCommandBuffer->EndRenderPass();
                }
            }

            pCommandBuffer->End();
        }
    }
}

void VulkanGfxState::SetViewport(uint32_t minX, uint32_t minY, uint32_t maxX, uint32_t maxY)
{
    m_viewports.resize(1);
    m_viewports[0].x      = minX;
    m_viewports[0].y      = minY;
    m_viewports[0].width  = maxX - minX;
    m_viewports[0].height = maxY - minY;
}

void VulkanGfxState::SetScissor(uint32_t minX, uint32_t minY, uint32_t maxX, uint32_t maxY)
{
    m_scissors.resize(1);
    m_scissors[0].offset.x      = minX;
    m_scissors[0].offset.y      = minY;
    m_scissors[0].extent.width  = maxX - minX;
    m_scissors[0].extent.height = maxY - minY;
}

void VulkanGfxState::SetBlendConstants(float r, float g, float b, float a)
{
    m_blendConstants[0] = r;
    m_blendConstants[1] = g;
    m_blendConstants[2] = b;
    m_blendConstants[3] = a;
}

void VulkanGfxState::SetLineWidth(float lineWidth)
{
    m_rasterizationState.lineWidth = lineWidth;
}
void VulkanGfxState::SetDepthBias(float depthBiasConstantFactor,
                                  float depthBiasClamp,
                                  float depthBiasSlopeFactor)
{
    m_rasterizationState.depthBiasEnable         = true;
    m_rasterizationState.depthBiasConstantFactor = depthBiasConstantFactor;
    m_rasterizationState.depthBiasClamp          = depthBiasClamp;
    m_rasterizationState.depthBiasSlopeFactor    = depthBiasSlopeFactor;
}

void VulkanGfxState::SetVertexBuffers(uint32_t numVertexBuffers,
                                      RHIBuffer* const* ppVertexBuffers,
                                      const uint64_t* pOffsets)
{
    m_vertexBuffers.resize(numVertexBuffers);
    m_vertexBufferOffsets.resize(numVertexBuffers);

    for (uint32_t i = 0; i < numVertexBuffers; i++)
    {
        m_vertexBuffers[i]       = TO_VK_BUFFER(ppVertexBuffers[i]);
        m_vertexBufferOffsets[i] = pOffsets[i];
    }
}
void VulkanGfxState::SetPipelineState(RHIPipeline* pPipeline,
                                      uint32_t numDescriptorSets,
                                      RHIDescriptorSet* const* ppDescriptorSets)
{
    m_pCurrentPipeline = TO_VK_PIPELINE(pPipeline);
    m_descriptorSets.resize(numDescriptorSets);
    for (uint32_t i = 0; i < numDescriptorSets; i++)
    {
        m_descriptorSets[i] = TO_VK_DESCRIPTORSET(ppDescriptorSets[i]);
    }
}

void VulkanGfxState::PreDraw(FVulkanCommandListContext* pContext)
{
    FVulkanCommandBuffer* pCmdBuffer = pContext->GetCommandBuffer();
}

FVulkanCommandListContext::FVulkanCommandListContext(RHICommandContextType contextType,
                                                     VulkanDevice* pDevice) :
    VulkanCommandContextBase(pDevice->GetQueue(contextType), VulkanCommandBufferType::ePrimary),
    m_pDevice(pDevice)
{
    m_pGfxState = ZEN_NEW() VulkanGfxState();
}

FVulkanCommandListContext::~FVulkanCommandListContext()
{
    ZEN_DELETE(m_pGfxState);
    m_pGfxState = nullptr;
}

void FVulkanCommandListContext::RHIBeginRendering(const RHIRenderingLayout* pRenderingLayout)
{
    if (RHIOptions::GetInstance().UseDynamicRendering())
    {
        VkRenderingInfoKHR renderingInfo{};

        const Rect2<int>& area = pRenderingLayout->renderArea;

        InitVkStruct(renderingInfo, VK_STRUCTURE_TYPE_RENDERING_INFO_KHR);
        renderingInfo.layerCount               = 1;
        renderingInfo.viewMask                 = 0;
        renderingInfo.flags                    = 0;
        renderingInfo.renderArea.offset.x      = area.minX;
        renderingInfo.renderArea.offset.y      = area.minY;
        renderingInfo.renderArea.extent.width  = area.Width();
        renderingInfo.renderArea.extent.height = area.Height();

        HeapVector<VkRenderingAttachmentInfoKHR> colorAttachments;
        colorAttachments.reserve(pRenderingLayout->numColorRenderTargets);

        for (uint32_t i = 0; i < pRenderingLayout->numColorRenderTargets; i++)
        {
            const RHIRenderTarget& colorRT = pRenderingLayout->colorRenderTargets[i];
            VulkanTexture* vulkanTexture   = TO_VK_TEXTURE(colorRT.texture);
            VkRenderingAttachmentInfoKHR colorAttachment{};
            InitVkStruct(colorAttachment, VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR);
            colorAttachment.imageView        = vulkanTexture->GetVkImageView();
            colorAttachment.imageLayout      = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            colorAttachment.loadOp           = ToVkAttachmentLoadOp(colorRT.loadOp);
            colorAttachment.storeOp          = ToVkAttachmentStoreOp(colorRT.storeOp);
            colorAttachment.clearValue.color = ToVkClearColor(colorRT.clearValue);
            colorAttachments.emplace_back(colorAttachment);
        }
        renderingInfo.colorAttachmentCount = pRenderingLayout->numColorRenderTargets;
        renderingInfo.pColorAttachments    = colorAttachments.data();

        VkRenderingAttachmentInfoKHR depthStencilAttachment;
        InitVkStruct(depthStencilAttachment, VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR);
        if (pRenderingLayout->hasDepthStencilRT)
        {
            const RHIRenderTarget& depthStencilRT = pRenderingLayout->depthStencilRenderTarget;
            VulkanTexture* vulkanTexture          = TO_VK_TEXTURE(depthStencilRT.texture);
            depthStencilAttachment.imageView      = vulkanTexture->GetVkImageView();
            depthStencilAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            depthStencilAttachment.loadOp      = ToVkAttachmentLoadOp(depthStencilRT.loadOp);
            depthStencilAttachment.storeOp     = ToVkAttachmentStoreOp(depthStencilRT.storeOp);
            depthStencilAttachment.clearValue.depthStencil =
                ToVkClearDepthStencil(depthStencilRT.clearValue);

            renderingInfo.pDepthAttachment = &depthStencilAttachment;
        }
        GetCommandBuffer()->BeginRendering(&renderingInfo);
    }
    else
    {
        uint32_t numAttachments = pRenderingLayout->GetTotalNumRenderTarges();
        HeapVector<VkClearValue> clearValues;
        clearValues.resize(numAttachments);
        HeapVector<RHIRenderTargetClearValue> clearValuesRHI;
        clearValuesRHI.resize(numAttachments);
        pRenderingLayout->GetRHIRenderTargetClearValueData(clearValuesRHI.data());

        for (uint32_t i = 0; i < pRenderingLayout->numColorRenderTargets; i++)
        {
            clearValues[i].color = ToVkClearColor(clearValuesRHI[i]);
        }
        if (pRenderingLayout->hasDepthStencilRT)
        {
            clearValues[numAttachments - 1].depthStencil =
                ToVkClearDepthStencil(clearValuesRHI[numAttachments - 1]);
        }

        VkRenderPassBeginInfo rpBeginInfo;
        InitVkStruct(rpBeginInfo, VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO);
        rpBeginInfo.renderPass = GVulkanRHI->GetOrCreateRenderPass(pRenderingLayout);
        rpBeginInfo.framebuffer =
            GVulkanRHI->GetOrCreateFramebuffer(pRenderingLayout, rpBeginInfo.renderPass);
        rpBeginInfo.renderArea.offset.x      = pRenderingLayout->renderArea.minX;
        rpBeginInfo.renderArea.offset.y      = pRenderingLayout->renderArea.minY;
        rpBeginInfo.renderArea.extent.width  = pRenderingLayout->renderArea.Width();
        rpBeginInfo.renderArea.extent.height = pRenderingLayout->renderArea.Height();
        rpBeginInfo.clearValueCount          = numAttachments;
        rpBeginInfo.pClearValues             = clearValues.data();

        GetCommandBuffer()->BeginRenderPass(&rpBeginInfo);
    }
}

void FVulkanCommandListContext::RHIEndRendering()
{
    if (RHIOptions::GetInstance().UseDynamicRendering())
    {
        GetCommandBuffer()->EndRendering();
    }
    else
    {
        GetCommandBuffer()->EndRenderPass();
    }
}

void FVulkanCommandListContext::RHISetScissor(uint32_t minX,
                                              uint32_t minY,
                                              uint32_t maxX,
                                              uint32_t maxY)
{
    m_pGfxState->SetScissor(minX, minY, maxX, maxY);
}

void FVulkanCommandListContext::RHISetViewport(uint32_t minX,
                                               uint32_t minY,
                                               uint32_t maxX,
                                               uint32_t maxY)
{
    m_pGfxState->SetViewport(minX, minY, maxX, maxY);
}

void FVulkanCommandListContext::RHISetDepthBias(float depthBiasConstantFactor,
                                                float depthBiasClamp,
                                                float depthBiasSlopeFactor)
{
    m_pGfxState->SetDepthBias(depthBiasConstantFactor, depthBiasClamp, depthBiasSlopeFactor);
}

void FVulkanCommandListContext::RHISetLineWidth(float lineWidth)
{
    m_pGfxState->SetLineWidth(lineWidth);
}

void FVulkanCommandListContext::RHISetBlendConstants(const Color& blendConstants)
{
    m_pGfxState->SetBlendConstants(blendConstants.r, blendConstants.g, blendConstants.b,
                                   blendConstants.a);
}

void FVulkanCommandListContext::RHIBindPipeline(RHIPipeline* pPipeline,
                                                uint32_t numDescriptorSets,
                                                RHIDescriptorSet* const* ppDescriptorSets)
{
    m_pGfxState->SetPipelineState(pPipeline, numDescriptorSets, ppDescriptorSets);
}

void FVulkanCommandListContext::RHIBindVertexBuffer(RHIBuffer* pBuffer, uint64_t offset)
{
    m_pGfxState->SetVertexBuffers(1, &pBuffer, &offset);
}


void FVulkanCommandListContext::RHIDrawIndexed(RHIBuffer* pIndexBuffer,
                                               uint32_t indexCount,
                                               uint32_t instanceCount,
                                               uint32_t firstIndex,
                                               int32_t vertexOffset,
                                               uint32_t firstInstance)
{
    m_pGfxState->PreDraw(this);

    FVulkanCommandBuffer* pCmdBuffer = GetCommandBuffer();

    // todo: get offset and index type info from buffers
    vkCmdBindIndexBuffer(pCmdBuffer->GetVkHandle(), TO_VK_BUFFER(pIndexBuffer)->GetVkBuffer(), 0,
                         VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(pCmdBuffer->GetVkHandle(), indexCount, instanceCount, firstIndex, vertexOffset,
                     firstInstance);
}

void FVulkanCommandListContext::RHIDrawIndexedIndirect(RHIBuffer* pIndirectBuffer,
                                                       RHIBuffer* pIndexBuffer,
                                                       uint32_t offset,
                                                       uint32_t drawCount,
                                                       uint32_t stride)
{
    m_pGfxState->PreDraw(this);

    FVulkanCommandBuffer* pCmdBuffer = GetCommandBuffer();

    // todo: get offset and index type info from buffers
    vkCmdBindIndexBuffer(pCmdBuffer->GetVkHandle(), TO_VK_BUFFER(pIndexBuffer)->GetVkBuffer(), 0,
                         VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexedIndirect(pCmdBuffer->GetVkHandle(), TO_VK_BUFFER(pIndexBuffer)->GetVkBuffer(),
                             offset, drawCount, stride);
}

void FVulkanCommandListContext::RHIDispatch(uint32_t groupCountX,
                                            uint32_t groupCountY,
                                            uint32_t groupCountZ)
{
    vkCmdDispatch(GetCommandBuffer()->GetVkHandle(), groupCountX, groupCountY, groupCountZ);
}

void FVulkanCommandListContext::RHIDispatchIndirect(RHIBuffer* pIndirectBuffer, uint32_t offset)
{
    VulkanBuffer* pVkBuffer = TO_VK_BUFFER(pIndirectBuffer);

    vkCmdDispatchIndirect(GetCommandBuffer()->GetVkHandle(), pVkBuffer->GetVkBuffer(), offset);
}

void FVulkanCommandListContext::RHIAddTransitions(
    BitField<RHIPipelineStageBits> srcStages,
    BitField<RHIPipelineStageBits> dstStages,
    const HeapVector<RHIMemoryTransition>& memoryTransitions,
    const HeapVector<RHIBufferTransition>& bufferTransitions,
    const HeapVector<RHITextureTransition>& textureTransitions)
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
        VkAccessFlags srcAccess    = RHIBufferUsageToAccessFlagBits(bufferTransition.oldUsage,
                                                                    bufferTransition.oldAccessMode);
        VkAccessFlags dstAccess    = RHIBufferUsageToAccessFlagBits(bufferTransition.newUsage,
                                                                    bufferTransition.newAccessMode);
        barrier.AddBufferBarrier(vulkanBuffer->GetVkBuffer(), bufferTransition.offset,
                                 bufferTransition.size, srcAccess, dstAccess);
    }

    for (const auto& textureTransition : textureTransitions)
    {
        VulkanTexture* vulkanTexture = TO_VK_TEXTURE(textureTransition.texture);

        VkAccessFlags srcAccess = ToVkAccessFlags(RHITextureUsageToAccessFlagBits(
            textureTransition.oldUsage, textureTransition.oldAccessMode));
        VkAccessFlags dstAccess = ToVkAccessFlags(RHITextureUsageToAccessFlagBits(
            textureTransition.newUsage, textureTransition.newAccessMode));
        // VkImageLayout oldLayout =
        //     ToVkImageLayout(RHITextureUsageToLayout(textureTransition.oldUsage));
        VkImageLayout oldLayout = GVulkanRHI->GetImageCurrentLayout(vulkanTexture->GetVkImage());
        VkImageLayout newLayout =
            ToVkImageLayout(RHITextureUsageToLayout(textureTransition.newUsage));
        // filter
        if (oldLayout == newLayout)
        {
            continue;
        }
        VkImageSubresourceRange subresourceRange{};
        ToVkImageSubresourceRange(textureTransition.subResourceRange, &subresourceRange);
        // subresourceRange.aspectMask = ToVkAspectFlags(textureTransition.subResourceRange.aspect);
        // subresourceRange.layerCount = textureTransition.subResourceRange.layerCount;
        // subresourceRange.levelCount = textureTransition.subResourceRange.levelCount;
        // subresourceRange.baseArrayLayer = textureTransition.subResourceRange.baseArrayLayer;
        // subresourceRange.baseMipLevel   = textureTransition.subResourceRange.baseMipLevel;

        barrier.AddImageBarrier(vulkanTexture->GetVkImage(), oldLayout, newLayout, subresourceRange,
                                srcAccess, dstAccess);
        GVulkanRHI->UpdateImageLayout(vulkanTexture->GetVkImage(), newLayout);
    }
    barrier.Execute(GetCommandBuffer()->GetVkHandle(), srcStages, dstStages);
}

void FVulkanCommandListContext::RHICopyBufferToTexture(RHIBuffer* pSrcBuffer,
                                                       RHITexture* pDstTexture,
                                                       uint32_t numRegions,
                                                       RHIBufferTextureCopyRegion* pRegions)
{}

void FVulkanCommandListContext::RHIGenTextureMipmaps(RHITexture* pTexture) {}

void FVulkanCommandListContext::RHIAddTextureTransition(RHITexture* pTexture,
                                                        RHITextureLayout newLayout)
{}
} // namespace zen