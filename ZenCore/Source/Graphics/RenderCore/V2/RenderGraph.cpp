#include "Graphics/RenderCore/V2/RenderGraph.h"
#include "Graphics/RenderCore/V2/RenderResource.h"
#include "Graphics/RHI/RHICommandList.h"
#include "Graphics/RenderCore/V2/ShaderProgram.h"

#ifdef ZEN_WIN32
#    include <queue>
#endif
namespace zen::rc
{
RDGResourceTrackerPool RenderGraph::s_trackerPool;

namespace
{
bool RangesOverlap(uint32_t baseA, uint32_t countA, uint32_t baseB, uint32_t countB)
{
    const uint64_t endA = static_cast<uint64_t>(baseA) + countA;
    const uint64_t endB = static_cast<uint64_t>(baseB) + countB;
    return baseA < endB && baseB < endA;
}

bool TextureSubResourceRangesOverlap(const RHITextureSubResourceRange& a,
                                     const RHITextureSubResourceRange& b)
{
    if (a.aspect.IsEmpty() || b.aspect.IsEmpty())
    {
        return true;
    }

    if ((static_cast<int64_t>(a.aspect) & static_cast<int64_t>(b.aspect)) == 0)
    {
        return false;
    }

    return RangesOverlap(a.baseMipLevel, a.levelCount, b.baseMipLevel, b.levelCount) &&
        RangesOverlap(a.baseArrayLayer, a.layerCount, b.baseArrayLayer, b.layerCount);
}

bool ReadAccessRequiresOrdering(const RDGResource* pResource,
                                const RDGAccess& previousRead,
                                const RDGAccess& currentRead)
{
    if (pResource->type != RDGResourceType::eTexture)
    {
        return false;
    }

    if (previousRead.textureUsage == currentRead.textureUsage)
    {
        return false;
    }

    return TextureSubResourceRangesOverlap(previousRead.textureSubResourceRange,
                                           currentRead.textureSubResourceRange);
}

bool AccessNeedsBarrier(const RDGResource* pResource,
                        const RDGAccess& previousAccess,
                        const RDGAccess& currentAccess)
{
    if (previousAccess.accessMode == RHIAccessMode::eRead &&
        currentAccess.accessMode == RHIAccessMode::eRead)
    {
        return ReadAccessRequiresOrdering(pResource, previousAccess, currentAccess);
    }

    return true;
}
} // namespace

RDGResourceTrackerPool::RDGResourceTrackerPool() = default;

RDGResourceTrackerPool::~RDGResourceTrackerPool()
{
    for (auto& kv : m_trackerMap)
    {
        delete kv.second;
    }
}

RDGResourceTracker* RDGResourceTrackerPool::GetTracker(const RHIResource* pResource)
{
    if (!m_trackerMap.contains(pResource))
    {
        m_trackerMap[pResource] = new RDGResourceTracker();
    }
    return m_trackerMap[pResource];
}

void RDGResourceTrackerPool::UpdateTrackerState(const RHITexture* pTexture,
                                                RHIAccessMode accessMode,
                                                RHITextureUsage usage)
{
    if (m_trackerMap.contains(pTexture))
    {
        RDGResourceTracker* pTracker = m_trackerMap[pTexture];
        pTracker->accessMode         = accessMode;
        pTracker->textureUsage       = usage;
    }
}

void RDGResourceTrackerPool::UpdateTrackerState(const RHIBuffer* pBuffer,
                                                RHIAccessMode accessMode,
                                                RHIBufferUsage usage)
{
    if (m_trackerMap.contains(pBuffer))
    {
        RDGResourceTracker* pTracker = m_trackerMap[pBuffer];
        pTracker->accessMode         = accessMode;
        pTracker->bufferUsage        = usage;
    }
}

void RenderGraph::AddPassBindPipelineNode(RDGPassNode* pParent,
                                          RHIPipeline* pPipelineHandle,
                                          RHIPipelineType pipelineType)
{
    auto* pNode         = AllocPassChildNode<RDGBindPipelineNode>(pParent);
    pNode->pPipeline    = std::move(pPipelineHandle);
    pNode->pipelineType = pipelineType;
    pNode->type         = RDGPassCmdType::eBindPipeline;
}

RDGPassNode* RenderGraph::AddComputePassNode(const ComputePass* pComputePass, std::string tag)
{
    VERIFY_EXPR_MSG(!tag.empty(), "compute pass node tag should not be empty");

    auto* pNode         = AllocNode<RDGComputePassNode>();
    pNode->type         = RDGNodeType::eComputePass;
    pNode->tag          = std::move(tag);
    pNode->pComputePass = pComputePass;
    pNode->selfStages.SetFlag(RHIPipelineStageBits::eComputeShader);
    for (uint32_t i = 0; i < pComputePass->numDescriptorSets; i++)
    {
        auto& setTrackers = pComputePass->resourceTrackers[i];
        for (auto& kv : setTrackers)
        {
            const PassResourceTracker& tracker = kv.second;
            if (tracker.resourceType == PassResourceType::eTexture)
            {
                for (auto* pTexture : tracker.textures)
                {
                    DeclareTextureAccessForPass(pNode, pTexture, tracker.textureUsage,
                                                pTexture->GetSubResourceRange(),
                                                tracker.accessMode);
                }
                // DeclareTextureAccessForPass(node, tracker.textureHandle, tracker.textureUsage,
                //                             tracker.textureSubResRange, tracker.accessMode,
                //                             tracker.name);
            }
            else if (tracker.resourceType == PassResourceType::eBuffer)
            {
                DeclareBufferAccessForPass(pNode, tracker.pBuffer, tracker.bufferUsage,
                                           tracker.accessMode);
            }
        }
    }
    AddPassBindPipelineNode(pNode, pComputePass->pPipeline, RHIPipelineType::eCompute);
    return pNode;
}

void RenderGraph::AddComputePassDispatchNode(RDGPassNode* pParent,
                                             uint32_t groupCountX,
                                             uint32_t groupCountY,
                                             uint32_t groupCountZ)
{
    auto* pNode        = AllocPassChildNode<RDGDispatchNode>(pParent);
    pNode->type        = RDGPassCmdType::eDispatch;
    pNode->groupCountX = groupCountX;
    pNode->groupCountY = groupCountY;
    pNode->groupCountZ = groupCountZ;
}

void RenderGraph::AddComputePassDispatchIndirectNode(RDGPassNode* pParent,
                                                     RHIBuffer* pIndirectBuffer,
                                                     uint32_t offset)
{
    auto* pNode            = AllocPassChildNode<RDGDispatchIndirectNode>(pParent);
    pNode->type            = RDGPassCmdType::eDispatchIndirect;
    pNode->pIndirectBuffer = std::move(pIndirectBuffer);
    pNode->offset          = offset;

    DeclareBufferAccessForPass(pParent, pIndirectBuffer, RHIBufferUsage::eIndirectBuffer,
                               RHIAccessMode::eRead);
}

// RDGPassNode* RenderGraph::AddGraphicsPassNode(RenderPassHandle renderPassHandle,
//                                               FramebufferHandle framebufferHandle,
//                                               Rect2<int> area,
//                                               VectorView<RHIRenderPassClearValue> clearValues,
//                                               bool hasColorTarget,
//                                               bool hasDepthTarget)
// {
//     auto* node = AllocNode<RDGGraphicsPassNode>();
//     // node->renderPass  = std::move(renderPassHandle);
//     // node->framebuffer = std::move(framebufferHandle);
//     // node->renderArea  = area;
//     node->type = RDGNodeType::eGraphicsPass;
//     // node->numAttachments = clearValues.size();
//
//     // for (auto i = 0; i < clearValues.size(); i++)
//     // {
//     //     node->clearValues[i] = clearValues[i];
//     // }
//     if (hasColorTarget)
//     {
//         node->selfStages.SetFlag(RHIPipelineStageBits::eColorAttachmentOutput);
//     }
//     if (hasDepthTarget)
//     {
//         node->selfStages.SetFlag(RHIPipelineStageBits::eEarlyFragmentTests);
//         node->selfStages.SetFlag(RHIPipelineStageBits::eLateFragmentTests);
//     }
//
//     return node;
// }

RDGPassNode* RenderGraph::AddGraphicsPassNode(const GraphicsPass* pGfxPass, std::string tag)
{
    VERIFY_EXPR_MSG(!tag.empty(), "graphics pass node tag should not be empty");
    auto* pNode          = AllocNode<RDGGraphicsPassNode>();
    pNode->pGraphicsPass = pGfxPass;
    // node->renderPass       = std::move(pGfxPass->renderPass);
    // node->framebuffer      = std::move(pGfxPass->framebuffer);
    // node->renderArea       = area;
    pNode->type = RDGNodeType::eGraphicsPass;
    // node->numAttachments   = clearValues.size();
    // node->renderPassLayout = std::move(pGfxPass->renderPassLayout);
    // node->dynamic          = RHIOptions::GetInstance().UseDynamicRendering();
    pNode->tag = std::move(tag);

    const RHIRenderingLayout* pRenderingLayout = pNode->pGraphicsPass->pRenderingLayout;

    // for (auto i = 0; i < clearValues.size(); i++)
    // {
    //     node->clearValues[i] = clearValues[i];
    // }
    if (pRenderingLayout->numColorRenderTargets > 0)
    {
        pNode->selfStages.SetFlag(RHIPipelineStageBits::eColorAttachmentOutput);
        // const TextureHandle* handles = node->renderPassLayout.GetRenderTargetHandles();
        // const auto& rtSubresourceRanges   = node->renderPassLayout.GetRTSubResourceRanges();
        const auto& colorRTs = pRenderingLayout->colorRenderTargets;
        for (uint32_t i = 0; i < pRenderingLayout->numColorRenderTargets; i++)
        {
            const std::string textureTag = pNode->tag + "_color_rt_" + std::to_string(i);
            DeclareTextureAccessForPass(
                pNode, colorRTs[i].pTexture, RHITextureUsage::eColorAttachment,
                colorRTs[i].pTexture->GetSubResourceRange(), RHIAccessMode::eReadWrite);
        }
    }
    if (pRenderingLayout->hasDepthStencilRT)
    {
        const auto& depthStencilRT = pRenderingLayout->depthStencilRenderTarget;
        pNode->selfStages.SetFlag(RHIPipelineStageBits::eEarlyFragmentTests);
        pNode->selfStages.SetFlag(RHIPipelineStageBits::eLateFragmentTests);
        DeclareTextureAccessForPass(
            pNode, depthStencilRT.pTexture, RHITextureUsage::eDepthStencilAttachment,
            depthStencilRT.pTexture->GetSubResourceRange(), RHIAccessMode::eReadWrite);
    }
    AddPassBindPipelineNode(pNode, pGfxPass->pPipeline, RHIPipelineType::eGraphics);
    for (uint32_t i = 0; i < pGfxPass->numDescriptorSets; i++)
    {
        auto& setTrackers = pGfxPass->resourceTrackers[i];
        for (auto& kv : setTrackers)
        {
            const PassResourceTracker& tracker = kv.second;
            if (tracker.resourceType == PassResourceType::eTexture)
            {
                for (auto* pTexture : tracker.textures)
                {
                    DeclareTextureAccessForPass(pNode, pTexture, tracker.textureUsage,
                                                pTexture->GetSubResourceRange(),
                                                tracker.accessMode);
                }
                // DeclareTextureAccessForPass(node, tracker.textureHandle, tracker.textureUsage,
                //                             tracker.textureSubResRange, tracker.accessMode,
                //                             tracker.name);
            }
            else if (tracker.resourceType == PassResourceType::eBuffer)
            {
                DeclareBufferAccessForPass(pNode, tracker.pBuffer, tracker.bufferUsage,
                                           tracker.accessMode);
            }
        }
    }

    return pNode;
}

void RenderGraph::AddGraphicsPassBindIndexBufferNode(RDGPassNode* pParent,
                                                     RHIBuffer* pBuffer,
                                                     DataFormat format,
                                                     uint32_t offset)
{
    auto* pNode    = AllocPassChildNode<RDGBindIndexBufferNode>(pParent);
    pNode->pBuffer = std::move(pBuffer);
    pNode->format  = format;
    pNode->offset  = offset;
    pNode->type    = RDGPassCmdType::eBindIndexBuffer;
    pNode->pParent->selfStages.SetFlag(RHIPipelineStageBits::eVertexShader);
}

void RenderGraph::AddGraphicsPassBindVertexBufferNode(RDGPassNode* pParent,
                                                      VectorView<RHIBuffer*> vertexBuffers,
                                                      VectorView<uint64_t> offsets)
{
    const uint32_t numBuffers = vertexBuffers.size();
    //size_t nodeSize           = sizeof(RDGBindVertexBufferNode) + sizeof(RHIBuffer*) * numBuffers +
    //    sizeof(uint64_t) * numBuffers;
    //auto* node = AllocPassChildNode<RDGBindVertexBufferNode>(parent, nodeSize);

    //node->numBuffers = numBuffers;

    RHIBuffer** ppBuffers = static_cast<RHIBuffer**>(
        m_poolAlloc.Alloc(numBuffers * sizeof(RHIBuffer*), alignof(RHIBuffer*)));
    std::ranges::copy(vertexBuffers, ppBuffers);

    uint64_t* pOffsets = static_cast<uint64_t*>(m_poolAlloc.Alloc(numBuffers * sizeof(uint64_t)));
    std::ranges::copy(offsets, pOffsets);

    auto* pNode = AllocPassChildNode<RDGBindVertexBufferNode>(
        pParent, MakeVecView(ppBuffers, numBuffers), MakeVecView(pOffsets, numBuffers));

    pNode->numBuffers = numBuffers;
    pNode->type       = RDGPassCmdType::eBindVertexBuffer;
    pNode->pParent->selfStages.SetFlag(RHIPipelineStageBits::eVertexShader);


    //RHIBuffer** pVertexBuffers     = node->VertexBuffers();
    //uint64_t* pVertexBufferOffsets = node->VertexBufferOffsets();

    //for (auto i = 0; i < numBuffers; i++)
    //{
    //    pVertexBuffers[i]       = vertexBuffers[i];
    //    pVertexBufferOffsets[i] = offsets[i];
    //}

    //// node->offsets = pVertexBufferOffsets;
    //node->type = RDGPassCmdType::eBindVertexBuffer;
    //node->parent->selfStages.SetFlag(RHIPipelineStageBits::eVertexShader);
}

void RenderGraph::AddGraphicsPassSetPushConstants(RDGPassNode* pParent,
                                                  const void* pData,
                                                  uint32_t dataSize)
{
    auto* pCopiedData =
        static_cast<uint8_t*>(m_poolAlloc.Alloc(sizeof(uint8_t) * dataSize, alignof(uint8_t)));
    std::memcpy(pCopiedData, pData, dataSize);

    auto* pNode =
        AllocPassChildNode<RDGSetPushConstantsNode>(pParent, MakeVecView(pCopiedData, dataSize));

    //const size_t nodeSize = sizeof(RDGSetPushConstantsNode) + dataSize;
    //auto* node            = AllocPassChildNode<RDGSetPushConstantsNode>(parent, nodeSize);
    pNode->type = RDGPassCmdType::eSetPushConstant;
    //node->dataSize        = dataSize;
    // node->data.resize(dataSize);
    //std::memcpy(node->PCData(), data, dataSize);
}

void RenderGraph::AddComputePassSetPushConstants(RDGPassNode* pParent,
                                                 const void* pData,
                                                 uint32_t dataSize)
{
    auto* pCopiedData =
        static_cast<uint8_t*>(m_poolAlloc.Alloc(sizeof(uint8_t) * dataSize, alignof(uint8_t)));
    std::memcpy(pCopiedData, pData, dataSize);

    auto* pNode =
        AllocPassChildNode<RDGSetPushConstantsNode>(pParent, MakeVecView(pCopiedData, dataSize));

    //const size_t nodeSize = sizeof(RDGSetPushConstantsNode) + dataSize;
    //auto* node            = AllocPassChildNode<RDGSetPushConstantsNode>(parent, nodeSize);
    pNode->type = RDGPassCmdType::eSetPushConstant;
    //node->dataSize        = dataSize;
    // node->data.resize(dataSize);
    //std::memcpy(node->PCData(), data, dataSize);
}

void RenderGraph::AddGraphicsPassDrawNode(RDGPassNode* pParent,
                                          uint32_t vertexCount,
                                          uint32_t instanceCount)
{
    auto* pNode          = AllocPassChildNode<RDGDrawNode>(pParent);
    pNode->type          = RDGPassCmdType::eDraw;
    pNode->vertexCount   = vertexCount;
    pNode->instanceCount = instanceCount;
}

void RenderGraph::AddGraphicsPassDrawIndexedNode(RDGPassNode* pParent,
                                                 uint32_t indexCount,
                                                 uint32_t instanceCount,
                                                 uint32_t firstIndex,
                                                 int32_t vertexOffset,
                                                 uint32_t firstInstance)
{
    auto* pNode          = AllocPassChildNode<RDGDrawIndexedNode>(pParent);
    pNode->type          = RDGPassCmdType::eDrawIndexed;
    pNode->indexCount    = indexCount;
    pNode->firstIndex    = firstIndex;
    pNode->instanceCount = instanceCount;
    pNode->vertexOffset  = vertexOffset;
    pNode->firstInstance = firstInstance;
}

void RenderGraph::AddGraphicsPassDrawIndexedIndirectNode(RDGPassNode* pParent,
                                                         RHIBuffer* pIndirectBuffer,
                                                         uint32_t offset,
                                                         uint32_t drawCount,
                                                         uint32_t stride)
{
    auto* pNode            = AllocPassChildNode<RDGDrawIndexedIndirectNode>(pParent);
    pNode->type            = RDGPassCmdType::eDrawIndexedIndirect;
    pNode->pIndirectBuffer = pIndirectBuffer;
    pNode->offset          = offset;
    pNode->drawCount       = drawCount;
    pNode->stride          = stride;
    pNode->pParent->selfStages.SetFlags(RHIPipelineStageBits::eDrawIndirect);

    DeclareBufferAccessForPass(pParent, pIndirectBuffer, RHIBufferUsage::eIndirectBuffer,
                               RHIAccessMode::eRead);
}

void RenderGraph::AddGraphicsPassSetBlendConstantNode(RDGPassNode* pParent, const Color& color)
{
    auto* pNode  = AllocPassChildNode<RDGSetBlendConstantsNode>(pParent);
    pNode->color = color;
    pNode->type  = RDGPassCmdType::eSetBlendConstant;
}

void RenderGraph::AddGraphicsPassSetLineWidthNode(RDGPassNode* pParent, float width)
{
    auto* pNode  = AllocPassChildNode<RDGSetLineWidthNode>(pParent);
    pNode->width = width;
    pNode->type  = RDGPassCmdType::eSetLineWidth;
}

void RenderGraph::AddGraphicsPassSetScissorNode(RDGPassNode* pParent, const Rect2<int>& scissor)
{
    auto* pNode    = AllocPassChildNode<RDGSetScissorNode>(pParent);
    pNode->scissor = scissor;
    pNode->type    = RDGPassCmdType::eSetScissor;
}

void RenderGraph::AddGraphicsPassSetViewportNode(RDGPassNode* pParent, const Rect2<float>& viewport)
{
    auto* pNode     = AllocPassChildNode<RDGSetViewportNode>(pParent);
    pNode->viewport = viewport;
    pNode->type     = RDGPassCmdType::eSetViewport;
}

void RenderGraph::AddGraphicsPassSetDepthBiasNode(RDGPassNode* pParent,
                                                  float depthBiasConstantFactor,
                                                  float depthBiasClamp,
                                                  float depthBiasSlopeFactor)
{
    auto* pNode                    = AllocPassChildNode<RDGSetDepthBiasNode>(pParent);
    pNode->depthBiasConstantFactor = depthBiasConstantFactor;
    pNode->depthBiasClamp          = depthBiasClamp;
    pNode->depthBiasSlopeFactor    = depthBiasSlopeFactor;
    pNode->type                    = RDGPassCmdType::eSetDepthBias;
}

void RenderGraph::AddBufferClearNode(RHIBuffer* pBuffer, uint32_t offset, uint64_t size)
{
    auto* pNode    = AllocNode<RDGBufferClearNode>();
    pNode->pBuffer = std::move(pBuffer);
    pNode->offset  = offset;
    pNode->size    = size;
    pNode->type    = RDGNodeType::eClearBuffer;
    pNode->selfStages.SetFlag(RHIPipelineStageBits::eTransfer);

    RDGAccess access{};
    access.accessMode  = RHIAccessMode::eReadWrite;
    access.nodeId      = pNode->id;
    access.bufferUsage = RHIBufferUsage::eTransferDst;

    RDGResource* pResource = GetOrAllocResource(pBuffer, RDGResourceType::eBuffer);
    access.resourceId      = pResource->id;

    AddResourceAccess(pResource, access);
}

void RenderGraph::AddBufferCopyNode(RHIBuffer* pSrcBufferHandle,
                                    RHIBuffer* pDstBufferHandle,
                                    const RHIBufferCopyRegion& copyRegion)
{
    auto* pNode       = AllocNode<RDGBufferCopyNode>();
    pNode->pSrcBuffer = std::move(pSrcBufferHandle);
    pNode->pDstBuffer = std::move(pDstBufferHandle);
    pNode->region     = copyRegion;
    pNode->type       = RDGNodeType::eCopyBuffer;
    pNode->selfStages.SetFlag(RHIPipelineStageBits::eTransfer);
    // read access to src buffer
    {
        RDGAccess readAccess{};
        readAccess.accessMode  = RHIAccessMode::eRead;
        readAccess.bufferUsage = RHIBufferUsage::eTransferSrc;
        readAccess.nodeId      = pNode->id;

        RDGResource* pResource =
            GetOrAllocResource(pSrcBufferHandle, RDGResourceType::eBuffer);
        readAccess.resourceId = pResource->id;

        AddResourceAccess(pResource, readAccess);
    }
    // write access to dst buffer
    {
        RDGAccess writeAccess{};
        writeAccess.accessMode  = RHIAccessMode::eReadWrite;
        writeAccess.bufferUsage = RHIBufferUsage::eTransferDst;
        writeAccess.nodeId      = pNode->id;

        RDGResource* pResource =
            GetOrAllocResource(pDstBufferHandle, RDGResourceType::eBuffer);
        writeAccess.resourceId = pResource->id;

        AddResourceAccess(pResource, writeAccess);
    }
}

void RenderGraph::AddBufferUpdateNode(RHIBuffer* pDstBufferHandle,
                                      const VectorView<RHIBufferCopySource>& sources)
{
    auto* pNode = AllocNode<RDGBufferUpdateNode>();
    for (const auto& source : sources)
    {
        pNode->sources.push_back(source);
    }
    pNode->pDstBuffer = std::move(pDstBufferHandle);
    pNode->type       = RDGNodeType::eUpdateBuffer;
    pNode->selfStages.SetFlag(RHIPipelineStageBits::eTransfer);
    // only support update through staging buffer
    // staging buffer is neither owned nor managed by RDG
    // write access to dst buffer
    RDGAccess writeAccess{};
    writeAccess.accessMode  = RHIAccessMode::eReadWrite;
    writeAccess.bufferUsage = RHIBufferUsage::eTransferDst;
    writeAccess.nodeId      = pNode->id;

    RDGResource* pResource =
        GetOrAllocResource(pDstBufferHandle, RDGResourceType::eBuffer);
    writeAccess.resourceId = pResource->id;

    AddResourceAccess(pResource, writeAccess);
}

void RenderGraph::AddTextureClearNode(RHITexture* pTexture,
                                      const Color& color,
                                      const RHITextureSubResourceRange& range)
{
    auto* pNode  = AllocNode<RDGTextureClearNode>();
    pNode->color = color;
    // node->range   = range;
    pNode->pTexture = pTexture;
    pNode->type     = RDGNodeType::eClearTexture;
    pNode->selfStages.SetFlag(RHIPipelineStageBits::eTransfer);

    RDGAccess access{};
    access.accessMode              = RHIAccessMode::eReadWrite;
    access.nodeId                  = pNode->id;
    access.textureUsage            = RHITextureUsage::eTransferDst;
    access.textureSubResourceRange = range;

    RDGResource* pResource = GetOrAllocResource(pTexture, RDGResourceType::eTexture);
    access.resourceId      = pResource->id;

    AddResourceAccess(pResource, access);
}

void RenderGraph::AddTextureCopyNode(RHITexture* pSrcTexture,
                                     RHITexture* pDstTexture,
                                     const VectorView<RHITextureCopyRegion>& regions)
{
    const uint32_t numCopyRegions      = regions.size();
    RHITextureCopyRegion* pCopyRegions = static_cast<RHITextureCopyRegion*>(m_poolAlloc.Alloc(
        sizeof(RHITextureCopyRegion) * numCopyRegions, alignof(RHITextureCopyRegion)));
    std::ranges::copy(regions, pCopyRegions);

    auto* pNode = AllocNode<RDGTextureCopyNode>(pSrcTexture, pDstTexture,
                                                MakeVecView(pCopyRegions, numCopyRegions));

    //const size_t nodeSize =
    //    sizeof(RDGTextureCopyNode) + sizeof(RHITextureCopyRegion) * numCopyRegions;

    //auto* node = AllocNode<RDGTextureCopyNode>(nodeSize);
    //node->numCopyRegions = numCopyRegions;
    pNode->type = RDGNodeType::eCopyTexture;
    //node->pSrcTexture    = srcTexture;
    //node->pDstTexture    = dstTexture;
    pNode->tag = "texture_copy";
    pNode->selfStages.SetFlag(RHIPipelineStageBits::eTransfer);

    //RHITextureCopyRegion* pCopyRegions = node->TextureCopyRegions();

    //for (uint32_t i = 0; i < numCopyRegions; ++i)
    //{
    //    // node->copyRegions.push_back(regions[i]);
    //    pCopyRegions[i] = regions[i];
    //}

    // node->copyRegions = pCopyRegions;

    // read access to src texture
    RDGAccess readAccess{};
    readAccess.textureSubResourceRange = pSrcTexture->GetSubResourceRange();
    readAccess.accessMode              = RHIAccessMode::eRead;
    readAccess.textureUsage            = RHITextureUsage::eTransferSrc;
    readAccess.nodeId                  = pNode->id;

    RDGResource* pSrcResource =
        GetOrAllocResource(pSrcTexture, RDGResourceType::eTexture);
    pSrcResource->tag = pSrcTexture->GetResourceTag();
    // if (srcResource->tag.empty())
    // {
    //     srcResource->tag = "src_texture";
    // }
    readAccess.resourceId = pSrcResource->id;

    AddResourceAccess(pSrcResource, readAccess);

    // write access to dst texture
    RDGAccess writeAccess{};
    writeAccess.textureSubResourceRange = pDstTexture->GetSubResourceRange();
    writeAccess.accessMode              = RHIAccessMode::eReadWrite;
    writeAccess.textureUsage            = RHITextureUsage::eTransferDst;
    writeAccess.nodeId                  = pNode->id;

    RDGResource* pDstResource =
        GetOrAllocResource(pDstTexture, RDGResourceType::eTexture);
    pDstResource->tag = pDstTexture->GetResourceTag();
    // if (dstResource->tag.empty())
    // {
    //     dstResource->tag = "dst_texture";
    // }
    writeAccess.resourceId = pDstResource->id;

    AddResourceAccess(pDstResource, writeAccess);
}

void RenderGraph::AddTextureReadNode(RHITexture* pSrcTexture,
                                     RHIBuffer* pDstBuffer,
                                     const VectorView<RHIBufferTextureCopyRegion>& regions)
{
    const size_t numCopyRegions = regions.size();

    auto* pCopyRegions = static_cast<RHIBufferTextureCopyRegion*>(m_poolAlloc.Alloc(
        sizeof(RHIBufferTextureCopyRegion) * numCopyRegions, alignof(RHIBufferTextureCopyRegion)));
    std::ranges::copy(regions, pCopyRegions);

    auto* pNode = AllocNode<RDGTextureReadNode>(pSrcTexture, pDstBuffer,
                                                MakeVecView(pCopyRegions, numCopyRegions));
    pNode->tag  = "texture_read";
    pNode->type = RDGNodeType::eReadTexture;

    //const size_t nodeSize =
    //    sizeof(RDGTextureReadNode) + sizeof(RHIBufferTextureCopyRegion) * numCopyRegions;
    //auto* node           = AllocNode<RDGTextureReadNode>(nodeSize);
    //node->numCopyRegions = numCopyRegions;
    //node->srcTexture     = srcTexture;
    //node->dstBuffer      = dstBufferHandle;
    pNode->selfStages.SetFlag(RHIPipelineStageBits::eTransfer);

    //auto* pCopyRegions = node->BufferTextureCopyRegions();

    //// for (auto& region : regions)
    //for (uint32_t i = 0; i < numCopyRegions; ++i)
    //{
    //    pCopyRegions[i] = regions[i];
    //    // node->bufferTextureCopyRegions.push_back(region);
    //}
    // read access to src texture
    {
        RDGAccess readAccess{};
        readAccess.accessMode              = RHIAccessMode::eRead;
        readAccess.textureUsage            = RHITextureUsage::eTransferSrc;
        readAccess.textureSubResourceRange = pSrcTexture->GetSubResourceRange();
        readAccess.nodeId                  = pNode->id;

        RDGResource* pResource =
            GetOrAllocResource(pSrcTexture, RDGResourceType::eTexture);
        readAccess.resourceId = pResource->id;

        AddResourceAccess(pResource, readAccess);
    }
    // write access to dst buffer
    {
        RDGAccess writeAccess{};
        writeAccess.accessMode  = RHIAccessMode::eReadWrite;
        writeAccess.bufferUsage = RHIBufferUsage::eTransferDst;
        writeAccess.nodeId      = pNode->id;

        RDGResource* pResource =
            GetOrAllocResource(pDstBuffer, RDGResourceType::eBuffer);
        writeAccess.resourceId = pResource->id;

        AddResourceAccess(pResource, writeAccess);
    }
}

void RenderGraph::AddTextureUpdateNode(RHITexture* pDstTexture,
                                       const VectorView<RHIBufferTextureCopySource>& sources)
{
    const uint32_t numCopySources = sources.size();
    auto* pCopySources            = static_cast<RHIBufferTextureCopySource*>(m_poolAlloc.Alloc(
        sizeof(RHIBufferTextureCopySource) * numCopySources, alignof(RHIBufferTextureCopySource)));
    std::ranges::copy(sources, pCopySources);

    auto* pNode =
        AllocNode<RDGTextureUpdateNode>(pDstTexture, MakeVecView(pCopySources, numCopySources));

    //const size_t nodeSize =
    //    sizeof(RHIBufferTextureCopySource) + sizeof(RHIBufferTextureCopySource) * numCopySources;
    //auto* node         = AllocNode<RDGTextureUpdateNode>(nodeSize);
    //auto* pCopySources = node->TextureCopySources();
    //for (uint32_t i = 0; i < numCopySources; ++i)
    //{
    //    pCopySources[i] = sources[i];
    //}
    // for (const auto& source : sources)
    // {
    //     node->sources.push_back(source);
    // }
    //node->numCopySources = numCopySources;
    //node->dstTexture     = dstTexture;
    pNode->type = RDGNodeType::eUpdateTexture;
    pNode->selfStages.SetFlag(RHIPipelineStageBits::eTransfer);
    // only support update through staging buffer
    // staging buffer is neither owned nor managed by RDG
    // write access to dst texture
    RDGAccess writeAccess{};
    writeAccess.textureSubResourceRange = pDstTexture->GetSubResourceRange();
    writeAccess.accessMode              = RHIAccessMode::eReadWrite;
    writeAccess.textureUsage            = RHITextureUsage::eTransferDst;
    writeAccess.nodeId                  = pNode->id;

    RDGResource* pResource = GetOrAllocResource(pDstTexture, RDGResourceType::eTexture);
    pResource->tag         = pDstTexture->GetResourceTag();
    writeAccess.resourceId = pResource->id;

    AddResourceAccess(pResource, writeAccess);
}

void RenderGraph::AddTextureResolveNode(RHITexture* pSrcTexture,
                                        RHITexture* pDstTexture,
                                        uint32_t srcLayer,
                                        uint32_t srcMipmap,
                                        uint32_t dstLayer,
                                        uint32_t dstMipMap)
{
    auto* pNode        = AllocNode<RDGTextureResolveNode>();
    pNode->pSrcTexture = pSrcTexture;
    pNode->pDstTexture = pDstTexture;
    pNode->srcLayer    = srcLayer;
    pNode->srcMipmap   = srcMipmap;
    pNode->dstLayer    = dstLayer;
    pNode->dstMipmap   = dstMipMap;
    pNode->type        = RDGNodeType::eResolveTexture;
    pNode->selfStages.SetFlag(RHIPipelineStageBits::eTransfer);
    // read access to src texture
    {
        RDGAccess readAccess{};
        readAccess.accessMode              = RHIAccessMode::eRead;
        readAccess.textureUsage            = RHITextureUsage::eTransferSrc;
        readAccess.textureSubResourceRange = pSrcTexture->GetSubResourceRange();
        readAccess.nodeId                  = pNode->id;

        RDGResource* pResource =
            GetOrAllocResource(pSrcTexture, RDGResourceType::eTexture);
        readAccess.resourceId = pResource->id;

        AddResourceAccess(pResource, readAccess);
    }
    // write access to dst texture
    {
        RDGAccess writeAccess{};
        writeAccess.accessMode              = RHIAccessMode::eReadWrite;
        writeAccess.textureUsage            = RHITextureUsage::eTransferDst;
        writeAccess.textureSubResourceRange = pDstTexture->GetSubResourceRange();
        writeAccess.nodeId                  = pNode->id;

        RDGResource* pResource =
            GetOrAllocResource(pDstTexture, RDGResourceType::eTexture);
        writeAccess.resourceId = pResource->id;

        AddResourceAccess(pResource, writeAccess);
    }
}

void RenderGraph::AddTextureMipmapGenNode(RHITexture* pTexture)
{
    auto* pNode     = AllocNode<RDGTextureMipmapGenNode>();
    pNode->type     = RDGNodeType::eGenTextureMipmap;
    pNode->pTexture = pTexture;
    pNode->selfStages.SetFlag(RHIPipelineStageBits::eTransfer);

    RDGAccess writeAccess{};
    writeAccess.textureSubResourceRange = pTexture->GetSubResourceRange();
    writeAccess.accessMode              = RHIAccessMode::eReadWrite;
    writeAccess.textureUsage            = RHITextureUsage::eTransferDst;
    writeAccess.nodeId                  = pNode->id;

    RDGResource* pResource = GetOrAllocResource(pTexture, RDGResourceType::eTexture);
    pResource->tag         = pTexture->GetResourceTag();
    writeAccess.resourceId = pResource->id;

    AddResourceAccess(pResource, writeAccess);
}

void RenderGraph::DeclareTextureAccessForPass(const RDGPassNode* pPassNode,
                                              RHITexture* pTexture,
                                              RHITextureUsage usage,
                                              const RHITextureSubResourceRange& range,
                                              RHIAccessMode accessMode)
{
    RDGResource* pResource = GetOrAllocResource(pTexture, RDGResourceType::eTexture);

    pResource->tag = pTexture->GetResourceTag() + "_layer_" + std::to_string(range.baseArrayLayer) +
        "_mip_" + std::to_string(range.baseMipLevel);
    // if (pResource->tag.empty())
    //     pResource->tag = tag;

    RDGNodeBase* pBaseNode = GetNodeBaseById(pPassNode->id);

    RDGAccess access{};
    access.accessMode              = accessMode;
    access.resourceId              = pResource->id;
    access.nodeId                  = pPassNode->id;
    access.textureUsage            = usage;
    access.textureSubResourceRange = range;
    AddResourceAccess(pResource, access);

    if (usage == RHITextureUsage::eSampled)
    {
        pBaseNode->selfStages.SetFlags(RHIPipelineStageBits::eFragmentShader);
    }
    if (usage == RHITextureUsage::eDepthStencilAttachment)
    {
        pBaseNode->selfStages.SetFlags(RHIPipelineStageBits::eEarlyFragmentTests,
                                       RHIPipelineStageBits::eLateFragmentTests);
    }
    if (usage == RHITextureUsage::eStorage)
    {
        if (pPassNode->type == RDGNodeType::eGraphicsPass)
        {
            pBaseNode->selfStages.SetFlags(RHIPipelineStageBits::eFragmentShader);
        }
        if (pPassNode->type == RDGNodeType::eComputePass)
        {
            pBaseNode->selfStages.SetFlags(RHIPipelineStageBits::eComputeShader);
        }
    }
}

void RenderGraph::DeclareBufferAccessForPass(const RDGPassNode* pPassNode,
                                             RHIBuffer* pBuffer,
                                             RHIBufferUsage usage,
                                             RHIAccessMode accessMode)
{
    RDGResource* pResource = GetOrAllocResource(pBuffer, RDGResourceType::eBuffer);

    pResource->tag = pBuffer->GetResourceTag();
    // if (pResource->tag.empty())
    //     pResource->tag = tag;

    RDGAccess access{};
    access.accessMode  = accessMode;
    access.resourceId  = pResource->id;
    access.nodeId      = pPassNode->id;
    access.bufferUsage = usage;
    AddResourceAccess(pResource, access);

    RDGNodeBase* pBaseNode = GetNodeBaseById(pPassNode->id);

    if (pPassNode->type == RDGNodeType::eGraphicsPass)
    {
        if (usage == RHIBufferUsage::eIndirectBuffer)
        {
            pBaseNode->selfStages.SetFlags(RHIPipelineStageBits::eDrawIndirect);
        }
        pBaseNode->selfStages.SetFlags(RHIPipelineStageBits::eFragmentShader);
    }
    if (pPassNode->type == RDGNodeType::eComputePass)
    {
        pBaseNode->selfStages.SetFlags(RHIPipelineStageBits::eAllCommands);
    }
}

void RenderGraph::AddResourceAccess(RDGResource* pResource, const RDGAccess& access)
{
    pResource->accesses.push_back(access);
    m_nodeAccessMap[access.nodeId].push_back(access);
}

bool RenderGraph::AddNodeDepsForResource(RDGResource* pResource,
                                         HashMap<RDG_ID, HeapVector<RDG_ID>>& nodeDependencies,
                                         HashMap<uint64_t, bool>& dependencyEdges,
                                         const RDGAccess& srcAccess,
                                         const RDGAccess& dstAccess)
{
    const RDG_ID srcNodeId = srcAccess.nodeId;
    const RDG_ID dstNodeId = dstAccess.nodeId;
    if (srcNodeId == dstNodeId)
    {
        return false;
    }

#if defined(ZEN_DEBUG)
    const auto& srcNodeTag = GetNodeBaseById(srcNodeId)->tag;
    const auto& dstNodeTag = GetNodeBaseById(dstNodeId)->tag;
    if (!srcNodeTag.empty() && !dstNodeTag.empty())
    {
        LOGI("RDG Node dependency: {} -> {}, resource: {}", srcNodeTag, dstNodeTag, pResource->tag);
    }
#endif
    // Add the dependency src -> dst
    const uint64_t dependencyEdgeKey = CreateNodePairKey(srcNodeId, dstNodeId);
    const bool addInDegree           = !dependencyEdges.contains(dependencyEdgeKey);
    if (addInDegree)
    {
        nodeDependencies[srcNodeId].push_back(dstNodeId);
        dependencyEdges[dependencyEdgeKey] = true;
    }

    return addInDegree;
}

void RenderGraph::SortNodesV2()
{
    m_sortedNodes.clear();

    HashMap<RDG_ID, HeapVector<RDG_ID>> nodeDependencies;
    HashMap<uint64_t, bool> dependencyEdges;
    HeapVector<uint32_t> inDegrees(m_nodeCount);
    // resolve node dependencies
    for (auto* pResource : m_resources)
    {
        bool hasLastWriter = false;
        RDGAccess lastWriter{};
        bool hasReadFrontier = false;
        size_t readFrontierBegin = 0;
        RDGAccess lastRead{};

        for (size_t accessIndex = 0; accessIndex < pResource->accesses.size(); ++accessIndex)
        {
            const RDGAccess& access = pResource->accesses[accessIndex];
            if (access.accessMode == RHIAccessMode::eRead)
            {
                const bool orderAfterReadFrontier =
                    hasReadFrontier && ReadAccessRequiresOrdering(pResource, lastRead, access);
                if (orderAfterReadFrontier)
                {
                    for (size_t readerIndex = readFrontierBegin; readerIndex < accessIndex;
                         ++readerIndex)
                    {
                        const RDGAccess& reader = pResource->accesses[readerIndex];
                        if (reader.accessMode != RHIAccessMode::eRead)
                        {
                            continue;
                        }

                        if (AddNodeDepsForResource(pResource, nodeDependencies, dependencyEdges,
                                                   reader, access))
                        {
                            inDegrees[access.nodeId]++;
                        }
                    }
                }
                else if (hasLastWriter)
                {
                    if (AddNodeDepsForResource(pResource, nodeDependencies, dependencyEdges,
                                               lastWriter, access))
                    {
                        inDegrees[access.nodeId]++;
                    }
                }

                if (!hasReadFrontier || orderAfterReadFrontier)
                {
                    hasReadFrontier   = true;
                    readFrontierBegin = accessIndex;
                }

                lastRead = access;
            }
            else if (access.accessMode == RHIAccessMode::eReadWrite)
            {
                if (hasLastWriter)
                {
                    if (AddNodeDepsForResource(pResource, nodeDependencies, dependencyEdges,
                                               lastWriter, access))
                    {
                        inDegrees[access.nodeId]++;
                    }
                }

                if (hasReadFrontier)
                {
                    for (size_t readerIndex = readFrontierBegin; readerIndex < accessIndex;
                         ++readerIndex)
                    {
                        const RDGAccess& reader = pResource->accesses[readerIndex];
                        if (reader.accessMode != RHIAccessMode::eRead)
                        {
                            continue;
                        }

                        if (AddNodeDepsForResource(pResource, nodeDependencies, dependencyEdges,
                                                   reader, access))
                        {
                            inDegrees[access.nodeId]++;
                        }
                    }
                }

                lastWriter       = access;
                hasLastWriter    = true;
                hasReadFrontier  = false;
            }
        }
    }

    std::queue<RDG_ID> queue;
    for (auto id = 0; id < inDegrees.size(); id++)
    {
        if (inDegrees[id] == 0)
        {
            queue.emplace(id);
        }
    }
    uint32_t sortedCount = 0;
    while (!queue.empty())
    {
        HeapVector<RDG_ID> currentLevel;
        const auto levelSize = queue.size();
        for (auto i = 0; i < levelSize; i++)
        {
            RDG_ID nodeId = queue.front();
            queue.pop();
            currentLevel.push_back(nodeId);
            sortedCount++;
            auto dependencyIter = nodeDependencies.find(nodeId);
            if (dependencyIter == nodeDependencies.end())
            {
                continue;
            }

            for (auto& neighbour : dependencyIter->second)
            {
                VERIFY_EXPR_MSG(inDegrees[neighbour] > 0,
                                "RenderGraph dependency in-degree underflow");
                inDegrees[neighbour]--;
                if (inDegrees[neighbour] == 0)
                {
                    queue.push(neighbour);
                }
            }
        }
        m_sortedNodes.push_back(std::move(currentLevel));
    }

    if (sortedCount != m_nodeCount)
    {
        LOGE("Cycle detected in RenderGraph '{}': sorted {} of {} nodes", m_rdgTag, sortedCount,
             m_nodeCount);
    }
}

void RenderGraph::Destroy()
{
    ResetBuildState();
}

void RenderGraph::DestroyNode(RDGNodeBase* pNode)
{
    if (pNode == nullptr)
    {
        return;
    }

    switch (pNode->type)
    {
        case RDGNodeType::eClearBuffer:
            reinterpret_cast<RDGBufferClearNode*>(pNode)->~RDGBufferClearNode();
            break;
        case RDGNodeType::eCopyBuffer:
            reinterpret_cast<RDGBufferCopyNode*>(pNode)->~RDGBufferCopyNode();
            break;
        case RDGNodeType::eUpdateBuffer:
            reinterpret_cast<RDGBufferUpdateNode*>(pNode)->~RDGBufferUpdateNode();
            break;
        case RDGNodeType::eClearTexture:
            reinterpret_cast<RDGTextureClearNode*>(pNode)->~RDGTextureClearNode();
            break;
        case RDGNodeType::eCopyTexture:
            reinterpret_cast<RDGTextureCopyNode*>(pNode)->~RDGTextureCopyNode();
            break;
        case RDGNodeType::eReadTexture:
            reinterpret_cast<RDGTextureReadNode*>(pNode)->~RDGTextureReadNode();
            break;
        case RDGNodeType::eUpdateTexture:
            reinterpret_cast<RDGTextureUpdateNode*>(pNode)->~RDGTextureUpdateNode();
            break;
        case RDGNodeType::eResolveTexture:
            reinterpret_cast<RDGTextureResolveNode*>(pNode)->~RDGTextureResolveNode();
            break;
        case RDGNodeType::eGenTextureMipmap:
            reinterpret_cast<RDGTextureMipmapGenNode*>(pNode)->~RDGTextureMipmapGenNode();
            break;
        case RDGNodeType::eGraphicsPass:
            reinterpret_cast<RDGGraphicsPassNode*>(pNode)->~RDGGraphicsPassNode();
            break;
        case RDGNodeType::eComputePass:
            reinterpret_cast<RDGComputePassNode*>(pNode)->~RDGComputePassNode();
            break;
        case RDGNodeType::eNone:
        case RDGNodeType::eMax:
        default: pNode->~RDGNodeBase(); break;
    }
}

void RenderGraph::DestroyPassChildNode(RDGPassChildNode* pNode)
{
    if (pNode == nullptr)
    {
        return;
    }

    switch (pNode->type)
    {
        case RDGPassCmdType::eBindIndexBuffer:
            reinterpret_cast<RDGBindIndexBufferNode*>(pNode)->~RDGBindIndexBufferNode();
            break;
        case RDGPassCmdType::eBindVertexBuffer:
            reinterpret_cast<RDGBindVertexBufferNode*>(pNode)->~RDGBindVertexBufferNode();
            break;
        case RDGPassCmdType::eBindPipeline:
            reinterpret_cast<RDGBindPipelineNode*>(pNode)->~RDGBindPipelineNode();
            break;
        case RDGPassCmdType::eDraw: reinterpret_cast<RDGDrawNode*>(pNode)->~RDGDrawNode(); break;
        case RDGPassCmdType::eDrawIndexed:
            reinterpret_cast<RDGDrawIndexedNode*>(pNode)->~RDGDrawIndexedNode();
            break;
        case RDGPassCmdType::eDrawIndexedIndirect:
            reinterpret_cast<RDGDrawIndexedIndirectNode*>(pNode)->~RDGDrawIndexedIndirectNode();
            break;
        case RDGPassCmdType::eDispatch:
            reinterpret_cast<RDGDispatchNode*>(pNode)->~RDGDispatchNode();
            break;
        case RDGPassCmdType::eDispatchIndirect:
            reinterpret_cast<RDGDispatchIndirectNode*>(pNode)->~RDGDispatchIndirectNode();
            break;
        case RDGPassCmdType::eSetPushConstant:
            reinterpret_cast<RDGSetPushConstantsNode*>(pNode)->~RDGSetPushConstantsNode();
            break;
        case RDGPassCmdType::eSetLineWidth:
            reinterpret_cast<RDGSetLineWidthNode*>(pNode)->~RDGSetLineWidthNode();
            break;
        case RDGPassCmdType::eSetBlendConstant:
            reinterpret_cast<RDGSetBlendConstantsNode*>(pNode)->~RDGSetBlendConstantsNode();
            break;
        case RDGPassCmdType::eSetScissor:
            reinterpret_cast<RDGSetScissorNode*>(pNode)->~RDGSetScissorNode();
            break;
        case RDGPassCmdType::eSetViewport:
            reinterpret_cast<RDGSetViewportNode*>(pNode)->~RDGSetViewportNode();
            break;
        case RDGPassCmdType::eSetDepthBias:
            reinterpret_cast<RDGSetDepthBiasNode*>(pNode)->~RDGSetDepthBiasNode();
            break;
        case RDGPassCmdType::eNone:
        case RDGPassCmdType::eClearAttachment:
        case RDGPassCmdType::eExecuteCommands:
        case RDGPassCmdType::eNextSubpass:
        case RDGPassCmdType::eMax:
        default: pNode->~RDGPassChildNode(); break;
    }
}

void RenderGraph::ResetBuildState()
{
    for (auto& kv : m_passChildNodeMap)
    {
        for (RDGPassChildNode* pChildNode : kv.second)
        {
            DestroyPassChildNode(pChildNode);
        }
    }

    for (auto& kv : m_baseNodeMap)
    {
        DestroyNode(kv.second);
    }

    for (RDGResource* pResource : m_resources)
    {
        m_resourceAllocator.Free(pResource);
    }

    m_compiledNodes.clear();
    m_sortedNodes.clear();
    m_baseNodeMap.clear();
    m_passChildNodeMap.clear();
    m_resources.clear();
    m_nodeAccessMap.clear();
    m_resourceMap.clear();
    m_poolAlloc.Reset();
    m_pCmdList       = nullptr;
    m_nodeCount      = 0;
    m_compileStats   = {};
    m_executionState = RDGExecutionState::eIdle;
}

void RenderGraph::Begin()
{
    LOGI("==========Render Graph Begin==========");
    VERIFY_EXPR_MSG(m_executionState != RDGExecutionState::eBuilding,
                    "RenderGraph::Begin called while already building");
    ResetBuildState();
    m_executionState = RDGExecutionState::eBuilding;
}

void RenderGraph::End()
{
    VERIFY_EXPR_MSG(m_executionState == RDGExecutionState::eBuilding,
                    "RenderGraph::End called before Begin");
    Compile();
    LOGI("==========Render Graph End==========");
}

void RenderGraph::Compile()
{
    m_compiledNodes.clear();
    m_compileStats = {};

    SortNodesV2();
    BuildCompiledNodeList();
    AttachFirstUseBarriers();
    AttachIntraGraphBarriers();
    ValidateCompiledGraph();
    m_executionState = RDGExecutionState::eCompiled;
}

void RenderGraph::BuildCompiledNodeList()
{
    m_compiledNodes.reserve(m_nodeCount);
    for (auto i = 0; i < m_sortedNodes.size(); i++)
    {
        const auto& currLevel = m_sortedNodes[i];
        for (auto& nodeId : currLevel)
        {
            RDGNodeBase* pNode = GetNodeBaseById(nodeId);
#if defined(ZEN_DEBUG)
            if (!pNode->tag.empty())
            {
                LOGI("RDG NodeTag after sort: {}, level: {}", pNode->tag, i);
            }
#endif

            RDGCompiledNode& compiledNode = m_compiledNodes.emplace_back();
            compiledNode.nodeId           = nodeId;

            m_compileStats.nodeCount++;
            if (pNode->type == RDGNodeType::eGraphicsPass ||
                pNode->type == RDGNodeType::eComputePass)
            {
                m_compileStats.passCount++;
            }
        }
    }

    m_compileStats.resourceCount    = static_cast<uint32_t>(m_resources.size());
    m_compileStats.commandListCount = m_compileStats.nodeCount > 0 ? 1 : 0;
}

void RenderGraph::AttachFirstUseBarriers()
{
    HeapVector<uint8_t> resourceInitialized(m_resources.size());
    for (RDGCompiledNode& compiledNode : m_compiledNodes)
    {
        auto iter = m_nodeAccessMap.find(compiledNode.nodeId);
        if (iter == m_nodeAccessMap.end())
        {
            continue;
        }

        for (const RDGAccess& access : iter->second)
        {
            if (access.resourceId < 0 ||
                static_cast<size_t>(static_cast<int32_t>(access.resourceId)) >= m_resources.size())
            {
                LOGE("RDG initial transition has invalid resource id");
                continue;
            }

            const size_t resourceIndex = static_cast<size_t>(static_cast<int32_t>(access.resourceId));
            if (resourceInitialized[resourceIndex] != 0)
            {
                continue;
            }

            resourceInitialized[resourceIndex] = 1;
            compiledNode.initialResourceAccesses.push_back(access);
            compiledNode.prologueSrcStages.SetFlag(RHIPipelineStageBits::eAllCommands);
            compiledNode.prologueDstStages.SetFlag(GetNodeBaseById(compiledNode.nodeId)->selfStages);
            m_compileStats.barrierCount++;
        }
    }
}

void RenderGraph::AttachIntraGraphBarriers()
{
    HeapVector<uint8_t> resourceInitialized(m_resources.size());
    HeapVector<uint8_t> resourceHasReadGroup(m_resources.size());
    HeapVector<RDGAccess> resourceStates(m_resources.size());
    HeapVector<BitField<RHIPipelineStageBits>> resourceReadGroupStages(m_resources.size());

    for (RDGCompiledNode& compiledNode : m_compiledNodes)
    {
        auto iter = m_nodeAccessMap.find(compiledNode.nodeId);
        if (iter == m_nodeAccessMap.end())
        {
            continue;
        }

        for (const RDGAccess& access : iter->second)
        {
            if (access.resourceId < 0 ||
                static_cast<size_t>(static_cast<int32_t>(access.resourceId)) >= m_resources.size())
            {
                LOGE("RDG dependency transition has invalid resource id");
                continue;
            }

            const size_t resourceIndex = static_cast<size_t>(static_cast<int32_t>(access.resourceId));
            RDGResource* pResource     = m_resources[resourceIndex];
            if (resourceInitialized[resourceIndex] == 0)
            {
                resourceInitialized[resourceIndex] = 1;
                resourceStates[resourceIndex]      = access;
                if (access.accessMode == RHIAccessMode::eRead)
                {
                    resourceHasReadGroup[resourceIndex] = 1;
                    resourceReadGroupStages[resourceIndex].SetFlag(
                        GetNodeBaseById(access.nodeId)->selfStages);
                }
                continue;
            }

            RDGAccess& previousAccess = resourceStates[resourceIndex];
            const bool needsBarrier = AccessNeedsBarrier(pResource, previousAccess, access);
            if (needsBarrier)
            {
                if (resourceHasReadGroup[resourceIndex] != 0)
                {
                    compiledNode.prologueSrcStages.SetFlag(
                        resourceReadGroupStages[resourceIndex]);
                }
                else
                {
                    compiledNode.prologueSrcStages.SetFlag(
                        GetNodeBaseById(previousAccess.nodeId)->selfStages);
                }
                compiledNode.prologueDstStages.SetFlag(GetNodeBaseById(access.nodeId)->selfStages);

                if (pResource->type == RDGResourceType::eBuffer)
                {
                    RHIBufferTransition transition;
                    transition.pBuffer       = dynamic_cast<RHIBuffer*>(pResource->pPhysicalRes);
                    transition.oldAccessMode = previousAccess.accessMode;
                    transition.newAccessMode = access.accessMode;
                    transition.oldUsage      = previousAccess.bufferUsage;
                    transition.newUsage      = access.bufferUsage;
                    compiledNode.prologueBufferTransitions.push_back(transition);
                }
                else if (pResource->type == RDGResourceType::eTexture)
                {
                    RHITextureTransition transition;
                    transition.pTexture         = dynamic_cast<RHITexture*>(pResource->pPhysicalRes);
                    transition.oldAccessMode    = previousAccess.accessMode;
                    transition.newAccessMode    = access.accessMode;
                    transition.oldUsage         = previousAccess.textureUsage;
                    transition.newUsage         = access.textureUsage;
                    transition.subResourceRange = access.textureSubResourceRange;
                    compiledNode.prologueTextureTransitions.push_back(transition);
                }
                else
                {
                    LOGE("Invalid RDGResource type!");
                }
                m_compileStats.barrierCount++;
            }

            previousAccess = access;
            if (access.accessMode == RHIAccessMode::eRead)
            {
                if (needsBarrier)
                {
                    resourceReadGroupStages[resourceIndex].Clear();
                }

                resourceHasReadGroup[resourceIndex] = 1;
                resourceReadGroupStages[resourceIndex].SetFlag(
                    GetNodeBaseById(access.nodeId)->selfStages);
            }
            else
            {
                resourceHasReadGroup[resourceIndex] = 0;
                resourceReadGroupStages[resourceIndex].Clear();
            }
        }
    }
}

void RenderGraph::ValidateCompiledGraph() const
{
    if (m_compiledNodes.size() != m_nodeCount)
    {
        LOGE("RenderGraph '{}' compiled {} of {} nodes", m_rdgTag, m_compiledNodes.size(),
             m_nodeCount);
    }

    if (m_baseNodeMap.size() != m_nodeCount)
    {
        LOGE("RenderGraph '{}' owns {} base nodes but node count is {}", m_rdgTag,
             m_baseNodeMap.size(), m_nodeCount);
    }

    for (const auto& kv : m_nodeAccessMap)
    {
        if (!m_baseNodeMap.contains(kv.first))
        {
            LOGE("RenderGraph '{}' has resource access for missing node {}", m_rdgTag,
                 static_cast<int32_t>(kv.first));
        }

        for (const RDGAccess& access : kv.second)
        {
            if (access.resourceId < 0 ||
                static_cast<size_t>(static_cast<int32_t>(access.resourceId)) >= m_resources.size())
            {
                LOGE("RenderGraph '{}' has invalid resource access id {}", m_rdgTag,
                     static_cast<int32_t>(access.resourceId));
            }
        }
    }

#if defined(ZEN_DEBUG)
    LOGI("RenderGraph '{}' compiled: nodes={}, passes={}, resources={}, barriers={}, cmdLists={}",
         m_rdgTag, m_compileStats.nodeCount, m_compileStats.passCount, m_compileStats.resourceCount,
         m_compileStats.barrierCount, m_compileStats.commandListCount);
#endif
}

void RenderGraph::Execute(RHICommandList* pCmdList)
{
    VERIFY_EXPR_MSG(m_executionState == RDGExecutionState::eCompiled,
                    "RenderGraph::Execute called before graph is compiled");
    m_pCmdList       = pCmdList;
    m_executionState = RDGExecutionState::eExecuting;

    for (RDGCompiledNode& compiledNode : m_compiledNodes)
    {
        EmitCompiledNodeBarriers(compiledNode);
        RunNode(GetNodeBaseById(compiledNode.nodeId));
    }

    m_pCmdList       = nullptr;
    m_executionState = RDGExecutionState::eCompiled;
}

void RenderGraph::RunNode(RDGNodeBase* pBase)
{
    RDGNodeType type = pBase->type;
    switch (type)
    {
        case RDGNodeType::eClearBuffer:
        {
            RDGBufferClearNode* pNode = reinterpret_cast<RDGBufferClearNode*>(pBase);
            m_pCmdList->ClearBuffer(pNode->pBuffer, pNode->offset, pNode->size);
        }
        break;
        case RDGNodeType::eCopyBuffer:
        {
            RDGBufferCopyNode* pNode = reinterpret_cast<RDGBufferCopyNode*>(pBase);
            m_pCmdList->CopyBuffer(pNode->pSrcBuffer, pNode->pDstBuffer, pNode->region);
        }
        break;
        case RDGNodeType::eUpdateBuffer:
        {
            RDGBufferUpdateNode* pNode = reinterpret_cast<RDGBufferUpdateNode*>(pBase);
            for (auto& source : pNode->sources)
            {
                m_pCmdList->CopyBuffer(source.pBuffer, pNode->pDstBuffer, source.region);
            }
        }
        break;
        case RDGNodeType::eClearTexture:
        {
            RDGTextureClearNode* pNode = reinterpret_cast<RDGTextureClearNode*>(pBase);
            m_pCmdList->ClearTexture(pNode->pTexture, pNode->color,
                                     pNode->pTexture->GetSubResourceRange());
        }
        break;
        case RDGNodeType::eCopyTexture:
        {
            RDGTextureCopyNode* pNode = reinterpret_cast<RDGTextureCopyNode*>(pBase);
            m_pCmdList->CopyTexture(pNode->pSrcTexture, pNode->pDstTexture,
                                    pNode->textureCopyRegions);
        }
        break;
        case RDGNodeType::eReadTexture:
        {
            RDGTextureReadNode* pNode = reinterpret_cast<RDGTextureReadNode*>(pBase);
            m_pCmdList->CopyTextureToBuffer(pNode->pSrcTexture, pNode->pDstBuffer,
                                            pNode->bufferTextureCopyRegions);
        }
        break;
        case RDGNodeType::eUpdateTexture:
        {
            RDGTextureUpdateNode* pNode = reinterpret_cast<RDGTextureUpdateNode*>(pBase);
            // for (auto& source : node->sources)
            for (uint32_t i = 0; i < pNode->numCopySources; i++)
            {
                const RHIBufferTextureCopySource& source = pNode->copySources[i];
                m_pCmdList->CopyBufferToTexture(source.pBuffer, pNode->pDstTexture, source.region);
            }
        }
        break;
        case RDGNodeType::eResolveTexture:
        {
            RDGTextureResolveNode* pNode = reinterpret_cast<RDGTextureResolveNode*>(pBase);
            m_pCmdList->ResolveTexture(pNode->pSrcTexture, pNode->pDstTexture, pNode->srcLayer,
                                       pNode->srcMipmap, pNode->dstLayer, pNode->dstMipmap);
        }
        break;

        case RDGNodeType::eGenTextureMipmap:
        {
            RDGTextureMipmapGenNode* pNode = reinterpret_cast<RDGTextureMipmapGenNode*>(pBase);
            m_pCmdList->GenerateTextureMipmaps(pNode->pTexture);
        }
        break;

        case RDGNodeType::eComputePass:
        {
            RDGComputePassNode* pNode   = reinterpret_cast<RDGComputePassNode*>(pBase);
            RHIPipeline* pBoundPipeline = nullptr;
            // for (RDGPassChildNode* child : node->childNodes)
            for (RDGPassChildNode* pChild : m_passChildNodeMap[pNode->id])
            {
                switch (pChild->type)
                {
                    case RDGPassCmdType::eBindPipeline:
                    {
                        auto* pCmdNode = reinterpret_cast<RDGBindPipelineNode*>(pChild);
                        pBoundPipeline = pCmdNode->pPipeline;
                        m_pCmdList->BindPipeline(pCmdNode->pipelineType, pCmdNode->pPipeline,
                                                 pNode->pComputePass->numDescriptorSets,
                                                 pNode->pComputePass->pDescriptorSets);
                    }
                    break;
                    case RDGPassCmdType::eDispatch:
                    {
                        auto* pCmdNode = reinterpret_cast<RDGDispatchNode*>(pChild);
                        m_pCmdList->Dispatch(pCmdNode->groupCountX, pCmdNode->groupCountY,
                                             pCmdNode->groupCountZ);
                    }
                    break;
                    case RDGPassCmdType::eDispatchIndirect:
                    {
                        auto* pCmdNode = reinterpret_cast<RDGDispatchIndirectNode*>(pChild);
                        m_pCmdList->DispatchIndirect(pCmdNode->pIndirectBuffer, pCmdNode->offset);
                    }
                    break;
                    case RDGPassCmdType::eSetPushConstant:
                    {
                        auto* pCmdNode = reinterpret_cast<RDGSetPushConstantsNode*>(pChild);
                        VERIFY_EXPR(pBoundPipeline != nullptr);
                        m_pCmdList->SetPushConstants(pBoundPipeline, pCmdNode->pcData);
                    }
                    break;
                    default: break;
                }
            }
        }
        break;

        case RDGNodeType::eGraphicsPass:
        {
            RDGGraphicsPassNode* pNode   = reinterpret_cast<RDGGraphicsPassNode*>(pBase);
            RHIPipeline* pBoundPipeline  = nullptr;
            RHIBuffer* pBoundIndexBuffer = nullptr;
            DataFormat boundIndexFormat  = DataFormat::eUndefined;
            uint32_t boundIndexOffset    = 0;

            m_pCmdList->BeginRendering(pNode->pGraphicsPass->pRenderingLayout);

            // if (node->dynamic)
            // {
            //     m_cmdList->BeginRenderPassDynamic(
            //         node->renderPassLayout, node->renderArea,
            //         VectorView(node->clearValues, node->numAttachments));
            // }
            // else
            // {
            //     m_cmdList->BeginRenderPass(node->renderPass, node->framebuffer, node->renderArea,
            //                                VectorView(node->clearValues, node->numAttachments));
            // }

            // m_cmdList->BeginRenderPass(node->renderPass, node->framebuffer, node->renderArea,
            //                            VectorView(node->clearValues, node->numAttachments));
            for (RDGPassChildNode* pChild : m_passChildNodeMap[pNode->id])
            {
                switch (pChild->type)
                {
                    case RDGPassCmdType::eBindIndexBuffer:
                    {
                        auto* pCmdNode = reinterpret_cast<RDGBindIndexBufferNode*>(pChild);

                        pBoundIndexBuffer = pCmdNode->pBuffer;
                        boundIndexFormat  = pCmdNode->format;
                        boundIndexOffset  = pCmdNode->offset;
                        // m_cmdList->BindIndexBuffer(cmdNode->buffer, cmdNode->format,
                        //                            cmdNode->offset);
                    }
                    break;
                    case RDGPassCmdType::eBindVertexBuffer:
                    {
                        auto* pCmdNode = reinterpret_cast<RDGBindVertexBufferNode*>(pChild);
                        //m_cmdList->BindVertexBuffers(
                        //    VectorView(cmdNode->VertexBuffers(), cmdNode->numBuffers),
                        //    cmdNode->VertexBufferOffsets());
                        m_pCmdList->BindVertexBuffers(pCmdNode->vertexBuffers, pCmdNode->offsets);
                    }
                    break;
                    case RDGPassCmdType::eBindPipeline:
                    {
                        auto* pCmdNode = reinterpret_cast<RDGBindPipelineNode*>(pChild);
                        pBoundPipeline = pCmdNode->pPipeline;
                        m_pCmdList->BindPipeline(pCmdNode->pipelineType, pCmdNode->pPipeline,
                                                 pNode->pGraphicsPass->numDescriptorSets,
                                                 pNode->pGraphicsPass->pDescriptorSets);
                    }
                    break;
                    case RDGPassCmdType::eClearAttachment: break;
                    case RDGPassCmdType::eDraw:
                    {
                        auto* pCmdNode = reinterpret_cast<RDGDrawNode*>(pChild);
                        m_pCmdList->Draw(pCmdNode->vertexCount, pCmdNode->instanceCount, 0, 0);
                    }
                    break;
                    case RDGPassCmdType::eDrawIndexed:
                    {
                        auto* pCmdNode = reinterpret_cast<RDGDrawIndexedNode*>(pChild);
                        VERIFY_EXPR(pBoundIndexBuffer != nullptr);
                        RHICommandDrawIndexed::Param param{};
                        param.pIndexBuffer      = pBoundIndexBuffer;
                        param.indexFormat       = boundIndexFormat;
                        param.indexBufferOffset = boundIndexOffset;
                        param.indexCount        = pCmdNode->indexCount;
                        param.instanceCount     = pCmdNode->instanceCount;
                        param.firstIndex        = pCmdNode->firstIndex;
                        param.vertexOffset      = pCmdNode->vertexOffset;
                        param.firstInstance     = pCmdNode->firstInstance;

                        m_pCmdList->DrawIndexed(param);
                    }
                    break;
                    case RDGPassCmdType::eDrawIndexedIndirect:
                    {
                        auto* pCmdNode = reinterpret_cast<RDGDrawIndexedIndirectNode*>(pChild);
                        VERIFY_EXPR(pBoundIndexBuffer != nullptr);
                        RHICommandDrawIndexedIndirect::Param param{};
                        param.pIndirectBuffer   = pCmdNode->pIndirectBuffer;
                        param.pIndexBuffer      = pBoundIndexBuffer;
                        param.indexFormat       = boundIndexFormat;
                        param.indexBufferOffset = boundIndexOffset;
                        param.offset            = pCmdNode->offset;
                        param.drawCount         = pCmdNode->drawCount;
                        param.stride            = pCmdNode->stride;

                        m_pCmdList->DrawIndexedIndirect(param);
                    }
                    break;
                    case RDGPassCmdType::eExecuteCommands: break;
                    case RDGPassCmdType::eNextSubpass: break;
                    case RDGPassCmdType::eDispatch: break;
                    case RDGPassCmdType::eDispatchIndirect: break;
                    case RDGPassCmdType::eSetPushConstant:
                    {
                        auto* pCmdNode = reinterpret_cast<RDGSetPushConstantsNode*>(pChild);
                        VERIFY_EXPR(pBoundPipeline != nullptr);
                        m_pCmdList->SetPushConstants(pBoundPipeline, pCmdNode->pcData);
                    }
                    break;
                    case RDGPassCmdType::eSetLineWidth:
                    {
                        auto* pCmdNode = reinterpret_cast<RDGSetLineWidthNode*>(pChild);
                        m_pCmdList->SetLineWidth(pCmdNode->width);
                    }
                    break;
                    case RDGPassCmdType::eSetBlendConstant:
                    {
                        auto* pCmdNode = reinterpret_cast<RDGSetBlendConstantsNode*>(pChild);
                        m_pCmdList->SetBlendConstants(pCmdNode->color);
                    }
                    break;
                    case RDGPassCmdType::eSetScissor:
                    {
                        auto* pCmdNode = reinterpret_cast<RDGSetScissorNode*>(pChild);
                        m_pCmdList->SetScissor(pCmdNode->scissor.minX, pCmdNode->scissor.minY,
                                               pCmdNode->scissor.maxX, pCmdNode->scissor.maxY);
                    }
                    break;
                    case RDGPassCmdType::eSetViewport:
                    {
                        auto* pCmdNode = reinterpret_cast<RDGSetViewportNode*>(pChild);
                        m_pCmdList->SetViewport(static_cast<uint32_t>(pCmdNode->viewport.minX),
                                                static_cast<uint32_t>(pCmdNode->viewport.minY),
                                                static_cast<uint32_t>(pCmdNode->viewport.maxX),
                                                static_cast<uint32_t>(pCmdNode->viewport.maxY));
                    }
                    break;
                    case RDGPassCmdType::eSetDepthBias:
                    {
                        auto* pCmdNode = reinterpret_cast<RDGSetDepthBiasNode*>(pChild);
                        m_pCmdList->SetDepthBias(pCmdNode->depthBiasConstantFactor,
                                                 pCmdNode->depthBiasClamp,
                                                 pCmdNode->depthBiasSlopeFactor);
                    }
                    break;

                    case RDGPassCmdType::eNone:
                    case RDGPassCmdType::eMax: break;
                }
            }

            m_pCmdList->EndRendering();

            // if (node->dynamic)
            // {
            //     m_cmdList->EndRenderPassDynamic();
            // }
            // else
            // {
            //     m_cmdList->EndRenderPass();
            // }
        }
        break;
        case RDGNodeType::eNone:
        case RDGNodeType::eMax:
        default: break;
    }
}

void RenderGraph::EmitCompiledNodeBarriers(RDGCompiledNode& compiledNode)
{
    if (compiledNode.initialResourceAccesses.empty())
    {
        if (compiledNode.prologueTextureTransitions.empty() &&
            compiledNode.prologueBufferTransitions.empty())
        {
            return;
        }

        for (const RHIBufferTransition& transition : compiledNode.prologueBufferTransitions)
        {
            s_trackerPool.UpdateTrackerState(transition.pBuffer, transition.newAccessMode,
                                             transition.newUsage);
        }

        for (const RHITextureTransition& transition : compiledNode.prologueTextureTransitions)
        {
            s_trackerPool.UpdateTrackerState(transition.pTexture, transition.newAccessMode,
                                             transition.newUsage);
        }

        m_pCmdList->AddTransitions(compiledNode.prologueSrcStages, compiledNode.prologueDstStages,
                                   {}, MakeVecView(compiledNode.prologueBufferTransitions),
                                   MakeVecView(compiledNode.prologueTextureTransitions));
        return;
    }

    HeapVector<RHITextureTransition> textureTransitions;
    HeapVector<RHIBufferTransition> bufferTransitions;
    textureTransitions.reserve(compiledNode.initialResourceAccesses.size() +
                               compiledNode.prologueTextureTransitions.size());
    bufferTransitions.reserve(compiledNode.initialResourceAccesses.size() +
                              compiledNode.prologueBufferTransitions.size());

    for (const RDGAccess& access : compiledNode.initialResourceAccesses)
    {
        if (access.resourceId < 0 ||
            static_cast<size_t>(static_cast<int32_t>(access.resourceId)) >= m_resources.size())
        {
            LOGE("RDG compiled initial transition has invalid resource id");
            continue;
        }

        RDGResource* pResource = m_resources[access.resourceId];
        if (pResource->type == RDGResourceType::eTexture)
        {
            RDGResourceTracker* pTracker = s_trackerPool.GetTracker(pResource->pPhysicalRes);
            RHITextureTransition textureTransition;
            textureTransition.pTexture         = dynamic_cast<RHITexture*>(pResource->pPhysicalRes);
            textureTransition.oldUsage         = pTracker->textureUsage;
            textureTransition.newUsage         = access.textureUsage;
            textureTransition.oldAccessMode    = pTracker->accessMode;
            textureTransition.newAccessMode    = access.accessMode;
            textureTransition.subResourceRange = access.textureSubResourceRange;
            textureTransitions.push_back(textureTransition);
        }
        else if (pResource->type == RDGResourceType::eBuffer)
        {
            RDGResourceTracker* pTracker = s_trackerPool.GetTracker(pResource->pPhysicalRes);
            RHIBufferTransition bufferTransition;
            bufferTransition.pBuffer       = dynamic_cast<RHIBuffer*>(pResource->pPhysicalRes);
            bufferTransition.oldUsage      = pTracker->bufferUsage;
            bufferTransition.newUsage      = access.bufferUsage;
            bufferTransition.oldAccessMode = pTracker->accessMode;
            bufferTransition.newAccessMode = access.accessMode;
            bufferTransition.offset        = 0;
            bufferTransitions.push_back(bufferTransition);
        }
    }

    for (const RHIBufferTransition& transition : compiledNode.prologueBufferTransitions)
    {
        bufferTransitions.push_back(transition);
    }

    for (const RHITextureTransition& transition : compiledNode.prologueTextureTransitions)
    {
        textureTransitions.push_back(transition);
    }

    if (textureTransitions.empty() && bufferTransitions.empty())
    {
        return;
    }

    for (const RHIBufferTransition& transition : bufferTransitions)
    {
        s_trackerPool.UpdateTrackerState(transition.pBuffer, transition.newAccessMode,
                                         transition.newUsage);
    }

    for (const RHITextureTransition& transition : textureTransitions)
    {
        s_trackerPool.UpdateTrackerState(transition.pTexture, transition.newAccessMode,
                                         transition.newUsage);
    }

    m_pCmdList->AddTransitions(compiledNode.prologueSrcStages, compiledNode.prologueDstStages, {},
                               bufferTransitions, textureTransitions);
}
} // namespace zen::rc
