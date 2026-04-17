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

    RDGResource* pResource = GetOrAllocResource(pBuffer, RDGResourceType::eBuffer, pNode->id);
    // resource->accessNodeMap[RHIAccessMode::eReadWrite].push_back(node->id);

    pResource->writtenByNodeIds.push_back(pNode->id);

    access.resourceId = pResource->id;

    m_nodeAccessMap[pNode->id].emplace_back(std::move(access));
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
            GetOrAllocResource(pSrcBufferHandle, RDGResourceType::eBuffer, pNode->id);
        // resource->accessNodeMap[RHIAccessMode::eRead].push_back(node->id);
        pResource->readByNodeIds.push_back(pNode->id);
        readAccess.resourceId = pResource->id;

        m_nodeAccessMap[pNode->id].emplace_back(std::move(readAccess));
    }
    // write access to dst buffer
    {
        RDGAccess writeAccess{};
        writeAccess.accessMode  = RHIAccessMode::eReadWrite;
        writeAccess.bufferUsage = RHIBufferUsage::eTransferDst;
        writeAccess.nodeId      = pNode->id;

        RDGResource* pResource =
            GetOrAllocResource(pDstBufferHandle, RDGResourceType::eBuffer, pNode->id);
        // resource->accessNodeMap[RHIAccessMode::eReadWrite].push_back(node->id);
        pResource->writtenByNodeIds.push_back(pNode->id);
        writeAccess.resourceId = pResource->id;

        m_nodeAccessMap[pNode->id].emplace_back(std::move(writeAccess));
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
    pNode->type       = RDGNodeType::eCopyBuffer;
    pNode->selfStages.SetFlag(RHIPipelineStageBits::eTransfer);
    // only support update through staging buffer
    // staging buffer is neither owned nor managed by RDG
    // write access to dst buffer
    RDGAccess writeAccess{};
    writeAccess.accessMode  = RHIAccessMode::eReadWrite;
    writeAccess.bufferUsage = RHIBufferUsage::eTransferDst;
    writeAccess.nodeId      = pNode->id;

    RDGResource* pResource =
        GetOrAllocResource(pDstBufferHandle, RDGResourceType::eBuffer, pNode->id);
    // resource->accessNodeMap[RHIAccessMode::eReadWrite].push_back(node->id);
    pResource->writtenByNodeIds.push_back(pNode->id);
    writeAccess.resourceId = pResource->id;

    m_nodeAccessMap[pNode->id].emplace_back(std::move(writeAccess));
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

    RDGResource* pResource = GetOrAllocResource(pTexture, RDGResourceType::eTexture, pNode->id);
    // resource->accessNodeMap[RHIAccessMode::eReadWrite].push_back(node->id);
    pResource->writtenByNodeIds.push_back(pNode->id);
    access.resourceId = pResource->id;

    m_nodeAccessMap[pNode->id].emplace_back(std::move(access));
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
        GetOrAllocResource(pSrcTexture, RDGResourceType::eTexture, pNode->id);
    pSrcResource->tag = pSrcTexture->GetResourceTag();
    // if (srcResource->tag.empty())
    // {
    //     srcResource->tag = "src_texture";
    // }
    // srcResource->accessNodeMap[RHIAccessMode::eRead].push_back(node->id);
    pSrcResource->readByNodeIds.push_back(pNode->id);
    readAccess.resourceId = pSrcResource->id;

    m_nodeAccessMap[pNode->id].emplace_back(std::move(readAccess));

    // write access to dst texture
    RDGAccess writeAccess{};
    writeAccess.textureSubResourceRange = pDstTexture->GetSubResourceRange();
    writeAccess.accessMode              = RHIAccessMode::eReadWrite;
    writeAccess.textureUsage            = RHITextureUsage::eTransferDst;
    writeAccess.nodeId                  = pNode->id;

    RDGResource* pDstResource =
        GetOrAllocResource(pDstTexture, RDGResourceType::eTexture, pNode->id);
    pDstResource->tag = pDstTexture->GetResourceTag();
    // if (dstResource->tag.empty())
    // {
    //     dstResource->tag = "dst_texture";
    // }
    // dstResource->accessNodeMap[RHIAccessMode::eReadWrite].push_back(node->id);
    pDstResource->writtenByNodeIds.push_back(pNode->id);
    writeAccess.resourceId = pDstResource->id;

    m_nodeAccessMap[pNode->id].emplace_back(std::move(writeAccess));
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
        readAccess.accessMode   = RHIAccessMode::eRead;
        readAccess.textureUsage = RHITextureUsage::eTransferSrc;
        readAccess.nodeId       = pNode->id;

        RDGResource* pResource =
            GetOrAllocResource(pSrcTexture, RDGResourceType::eTexture, pNode->id);
        // resource->accessNodeMap[RHIAccessMode::eRead].push_back(node->id);
        pResource->readByNodeIds.push_back(pNode->id);
        readAccess.resourceId = pResource->id;

        m_nodeAccessMap[pNode->id].emplace_back(std::move(readAccess));
    }
    // write access to dst buffer
    {
        RDGAccess writeAccess{};
        writeAccess.accessMode  = RHIAccessMode::eReadWrite;
        writeAccess.bufferUsage = RHIBufferUsage::eTransferDst;
        writeAccess.nodeId      = pNode->id;

        RDGResource* pResource =
            GetOrAllocResource(pDstBuffer, RDGResourceType::eBuffer, pNode->id);
        // resource->accessNodeMap[RHIAccessMode::eReadWrite].push_back(node->id);
        pResource->writtenByNodeIds.push_back(pNode->id);
        writeAccess.resourceId = pResource->id;

        m_nodeAccessMap[pNode->id].emplace_back(std::move(writeAccess));
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
    pNode->type = RDGNodeType::eCopyBuffer;
    pNode->selfStages.SetFlag(RHIPipelineStageBits::eTransfer);
    // only support update through staging buffer
    // staging buffer is neither owned nor managed by RDG
    // write access to dst texture
    RDGAccess writeAccess{};
    writeAccess.accessMode   = RHIAccessMode::eReadWrite;
    writeAccess.textureUsage = RHITextureUsage::eTransferDst;
    writeAccess.nodeId       = pNode->id;

    RDGResource* pResource = GetOrAllocResource(pDstTexture, RDGResourceType::eTexture, pNode->id);
    // resource->accessNodeMap[RHIAccessMode::eReadWrite].push_back(node->id);
    pResource->writtenByNodeIds.push_back(pNode->id);
    writeAccess.resourceId = pResource->id;

    m_nodeAccessMap[pNode->id].emplace_back(std::move(writeAccess));
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
    pNode->selfStages.SetFlag(RHIPipelineStageBits::eTransfer);
    // read access to src texture
    {
        RDGAccess readAccess{};
        readAccess.accessMode   = RHIAccessMode::eRead;
        readAccess.textureUsage = RHITextureUsage::eTransferSrc;
        readAccess.nodeId       = pNode->id;

        RDGResource* pResource =
            GetOrAllocResource(pSrcTexture, RDGResourceType::eTexture, pNode->id);
        // resource->accessNodeMap[RHIAccessMode::eRead].push_back(node->id);
        pResource->readByNodeIds.push_back(pNode->id);
        readAccess.resourceId = pResource->id;

        m_nodeAccessMap[pNode->id].emplace_back(std::move(readAccess));
    }
    // write access to dst texture
    {
        RDGAccess writeAccess{};
        writeAccess.accessMode   = RHIAccessMode::eReadWrite;
        writeAccess.textureUsage = RHITextureUsage::eTransferDst;
        writeAccess.nodeId       = pNode->id;

        RDGResource* pResource =
            GetOrAllocResource(pDstTexture, RDGResourceType::eTexture, pNode->id);
        // resource->accessNodeMap[RHIAccessMode::eReadWrite].push_back(node->id);
        pResource->writtenByNodeIds.push_back(pNode->id);
        writeAccess.resourceId = pResource->id;

        m_nodeAccessMap[pNode->id].emplace_back(std::move(writeAccess));
    }
}

void RenderGraph::AddTextureMipmapGenNode(RHITexture* pTexture)
{
    auto* pNode     = AllocNode<RDGTextureMipmapGenNode>();
    pNode->type     = RDGNodeType::eGenTextureMipmap;
    pNode->pTexture = pTexture;
    pNode->selfStages.SetFlag(RHIPipelineStageBits::eTransfer);
}

void RenderGraph::DeclareTextureAccessForPass(const RDGPassNode* pPassNode,
                                              RHITexture* pTexture,
                                              RHITextureUsage usage,
                                              const RHITextureSubResourceRange& range,
                                              RHIAccessMode accessMode)
{
    RDGResource* pResource = GetOrAllocResource(pTexture, RDGResourceType::eTexture, pPassNode->id);

    if (accessMode == RHIAccessMode::eReadWrite)
    {
        pResource->writtenByNodeIds.push_back(pPassNode->id);
    }
    else if (accessMode == RHIAccessMode::eRead)
    {
        pResource->readByNodeIds.push_back(pPassNode->id);
    }
    // resource->accessNodeMap[accessMode].push_back(passNode->id);

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
    m_nodeAccessMap[pPassNode->id].emplace_back(std::move(access));

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
    RDGResource* pResource = GetOrAllocResource(pBuffer, RDGResourceType::eBuffer, pPassNode->id);
    // resource->accessNodeMap[accessMode].push_back(passNode->id);
    if (accessMode == RHIAccessMode::eReadWrite)
    {
        pResource->writtenByNodeIds.push_back(pPassNode->id);
    }
    else if (accessMode == RHIAccessMode::eRead)
    {
        pResource->readByNodeIds.push_back(pPassNode->id);
    }

    pResource->tag = pBuffer->GetResourceTag();
    // if (pResource->tag.empty())
    //     pResource->tag = tag;

    RDGAccess access{};
    access.accessMode  = accessMode;
    access.resourceId  = pResource->id;
    access.nodeId      = pPassNode->id;
    access.bufferUsage = usage;
    m_nodeAccessMap[pPassNode->id].emplace_back(std::move(access));

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

bool RenderGraph::AddNodeDepsForResource(RDGResource* pResource,
                                         HashMap<RDG_ID, std::vector<RDG_ID>>& nodeDependencies,
                                         const RDG_ID& srcNodeId,
                                         const RDG_ID& dstNodeId)
{
    if (srcNodeId == dstNodeId)
        return false;
    auto srcNodeTag = GetNodeBaseById(srcNodeId)->tag;
    auto dstNodeTag = GetNodeBaseById(dstNodeId)->tag;
#if defined(ZEN_DEBUG)
    if (!srcNodeTag.empty() && !dstNodeTag.empty())
    {
        LOGI("RDG Node dependency: {} -> {}, resource: {}", srcNodeTag, dstNodeTag, pResource->tag);
    }
#endif
    // Add the dependency src -> dst
    nodeDependencies[srcNodeId].push_back(dstNodeId);

    uint32_t oldUsage = 0;
    uint32_t newUsage = 0;
    RHIAccessMode oldAccessMode;
    RHIAccessMode newAccessMode;
    RHITextureSubResourceRange textureSubResourceRange;

    bool isTexture = pResource->type == RDGResourceType::eTexture;
    for (const auto& access : m_nodeAccessMap[srcNodeId])
    {
        if (access.resourceId == pResource->id)
        {
            if (isTexture)
            {
                oldUsage                = ToUnderlying(access.textureUsage);
                textureSubResourceRange = access.textureSubResourceRange;
            }
            else
            {
                oldUsage = ToUnderlying(access.bufferUsage);
            }
            oldAccessMode = access.accessMode;
        }
    }
    for (const auto& access : m_nodeAccessMap[dstNodeId])
    {
        if (access.resourceId == pResource->id)
        {
            if (isTexture)
            {
                newUsage                = ToUnderlying(access.textureUsage);
                textureSubResourceRange = access.textureSubResourceRange;
            }
            else
            {
                newUsage = ToUnderlying(access.bufferUsage);
            }
            newAccessMode = access.accessMode;
        }
    }

    auto nodePairId  = CreateNodePairKey(srcNodeId, dstNodeId);
    bool addInDegree = true;

    // if (m_bufferTransitions.contains(nodePairId) || m_textureTransitions.contains(nodePairId))
    // {
    //     addInDegree = false;
    // }
    if (pResource->type == RDGResourceType::eBuffer)
    {
        RHIBufferTransition bufferTransition;
        bufferTransition.pBuffer       = dynamic_cast<RHIBuffer*>(pResource->pPhysicalRes);
        bufferTransition.oldAccessMode = oldAccessMode;
        bufferTransition.newAccessMode = newAccessMode;
        bufferTransition.oldUsage      = static_cast<RHIBufferUsage>(oldUsage);
        bufferTransition.newUsage      = static_cast<RHIBufferUsage>(newUsage);
        m_bufferTransitions[nodePairId].emplace_back(bufferTransition);
    }
    else if (pResource->type == RDGResourceType::eTexture)
    {
        RHITextureTransition textureTransition;
        textureTransition.pTexture         = dynamic_cast<RHITexture*>(pResource->pPhysicalRes);
        textureTransition.oldAccessMode    = oldAccessMode;
        textureTransition.newAccessMode    = newAccessMode;
        textureTransition.oldUsage         = static_cast<RHITextureUsage>(oldUsage);
        textureTransition.newUsage         = static_cast<RHITextureUsage>(newUsage);
        textureTransition.subResourceRange = textureSubResourceRange;
        m_textureTransitions[nodePairId].emplace_back(textureTransition);
    }
    else
    {
        LOGE("Invalid RDGResource type!");
    }

    return addInDegree;
}

// void RenderGraph::SortNodes()
// {
//     // resolve node dependencies
//     HashMap<RDG_ID, std::vector<RDG_ID>> nodeDpedencies;
//     std::vector<uint32_t> inDegrees(m_nodeCount, 0);
//     for (RDGResource* resource : m_resources)
//     {
//         if (resource->accessNodeMap.contains(RHIAccessMode::eReadWrite) &&
//             resource->accessNodeMap.contains(RHIAccessMode::eRead))
//         {
//             bool isTexture         = resource->type == RDGResourceType::eTexture;
//             auto& writtenByNodeIds = resource->accessNodeMap[RHIAccessMode::eReadWrite];
//             auto& readByNodeIDs    = resource->accessNodeMap[RHIAccessMode::eRead];
//             std::ranges::sort(writtenByNodeIds);
//             std::ranges::sort(readByNodeIDs);
//             // Combine write and read access nodes into a single vector while preserving the order
//             std::vector<RDG_ID> accessNodes = writtenByNodeIds;
//             accessNodes.insert(accessNodes.end(), readByNodeIDs.begin(), readByNodeIDs.end());
//
//             // Sort the combined access nodes by RDG_ID to maintain proper order
//             std::ranges::sort(accessNodes.begin(), accessNodes.end());
//
//             // Create dependencies between consecutive nodes
//             for (size_t i = 1; i < accessNodes.size(); ++i)
//             {
//                 RDG_ID srcNodeId = accessNodes[i - 1];
//                 RDG_ID dstNodeId = accessNodes[i];
//
//                 auto nodePairId = CreateNodePairKey(srcNodeId, dstNodeId);
//
//                 if (srcNodeId == dstNodeId)
//                     continue;
//                 auto srcNodeTag = GetNodeBaseById(srcNodeId)->tag;
//                 auto dstNodeTag = GetNodeBaseById(dstNodeId)->tag;
// #if defined(ZEN_DEBUG)
//
//                 if (!srcNodeTag.empty() && !dstNodeTag.empty())
//                 {
//                     LOGI("RDG Node dependency: {} -> {}, resource: {}", srcNodeTag, dstNodeTag,
//                          pResource->tag);
//                 }
// #endif
//                 // Add the dependency src -> dst
//                 nodeDpedencies[srcNodeId].push_back(dstNodeId);
//
//                 uint32_t oldUsage = 0;
//                 uint32_t newUsage = 0;
//                 RHIAccessMode oldAccessMode;
//                 RHIAccessMode newAccessMode;
//                 RHITextureSubResourceRange textureSubResourceRange;
//                 for (const auto& access : m_nodeAccessMap[srcNodeId])
//                 {
//                     if (access.resourceId == resource->id)
//                     {
//                         if (isTexture)
//                         {
//                             oldUsage                = ToUnderlying(access.textureUsage);
//                             textureSubResourceRange = access.textureSubResourceRange;
//                         }
//                         else
//                         {
//                             oldUsage = ToUnderlying(access.bufferUsage);
//                         }
//                         oldAccessMode = access.accessMode;
//                     }
//                 }
//                 for (const auto& access : m_nodeAccessMap[dstNodeId])
//                 {
//                     if (access.resourceId == resource->id)
//                     {
//                         if (isTexture)
//                         {
//                             newUsage                = ToUnderlying(access.textureUsage);
//                             textureSubResourceRange = access.textureSubResourceRange;
//
//                             // RDGResourceUsageTracker::GetInstance().TrackTextureUsage(
//                             //     resource->physicalHandle, access.textureUsage);
//                         }
//                         else
//                         {
//                             newUsage = ToUnderlying(access.bufferUsage);
//
//                             // RDGResourceUsageTracker::GetInstance().TrackBufferUsage(
//                             //     resource->physicalHandle, access.bufferUsage);
//                         }
//                         newAccessMode = access.accessMode;
//                     }
//                 }
//                 if (resource->type == RDGResourceType::eBuffer)
//                 {
//                     RHIBufferTransition bufferTransition;
//                     bufferTransition.buffer = dynamic_cast<RHIBuffer*>(resource->physicalRes);
//                     bufferTransition.oldAccessMode = oldAccessMode;
//                     bufferTransition.newAccessMode = newAccessMode;
//                     bufferTransition.oldUsage      = static_cast<RHIBufferUsage>(oldUsage);
//                     bufferTransition.newUsage      = static_cast<RHIBufferUsage>(newUsage);
//                     m_bufferTransitions[nodePairId].emplace_back(bufferTransition);
//                 }
//                 else if (resource->type == RDGResourceType::eTexture)
//                 {
//                     RHITextureTransition textureTransition;
//                     textureTransition.texture =
//                         dynamic_cast<RHITexture*>(resource->physicalRes);
//                     textureTransition.oldAccessMode    = oldAccessMode;
//                     textureTransition.newAccessMode    = newAccessMode;
//                     textureTransition.oldUsage         = static_cast<RHITextureUsage>(oldUsage);
//                     textureTransition.newUsage         = static_cast<RHITextureUsage>(newUsage);
//                     textureTransition.subResourceRange = textureSubResourceRange;
//                     m_textureTransitions[nodePairId].emplace_back(textureTransition);
//                 }
//                 else
//                 {
//                     LOGE("Invalid RDGResource type!");
//                 }
//                 inDegrees[dstNodeId]++;
//             }
//         }
//     }
//     std::queue<RDG_ID> queue;
//     for (auto id = 0; id < inDegrees.size(); id++)
//     {
//         if (inDegrees[id] == 0)
//         {
//             queue.emplace(id);
//         }
//     }
//     uint32_t sortedCount = 0;
//     while (!queue.empty())
//     {
//         std::vector<RDG_ID> currentLevel;
//         const auto levelSize = queue.size();
//         for (auto i = 0; i < levelSize; i++)
//         {
//             RDG_ID nodeId = queue.front();
//             queue.pop();
//             currentLevel.push_back(nodeId);
//             sortedCount++;
//             for (auto& neighbour : nodeDpedencies[nodeId])
//             {
//                 inDegrees[neighbour]--;
//                 if (inDegrees[neighbour] == 0)
//                 {
//                     queue.push(neighbour);
//                 }
//             }
//         }
//         m_sortedNodes.push_back(std::move(currentLevel));
//     }
//     if (sortedCount != m_nodeCount)
//     {
//         LOGE("Cycle detected in RenderGraph");
//     }
// }

void RenderGraph::SortNodesV2()
{
    HashMap<RDG_ID, std::vector<RDG_ID>> nodeDependencies;
    std::vector<uint32_t> inDegrees(m_nodeCount, 0);
    // resolve node dependencies
    for (auto* pResource : m_resources)
    {
        // auto& writers = resource->accessNodeMap[RHIAccessMode::eReadWrite];
        // auto& readers = resource->accessNodeMap[RHIAccessMode::eRead];

        // Merge writers and readers into one timeline
        std::vector<std::pair<RDG_ID, RHIAccessMode>> accesses;
        for (const auto& w : pResource->writtenByNodeIds)
            accesses.emplace_back(w, RHIAccessMode::eReadWrite);
        for (const auto& r : pResource->readByNodeIds)
            accesses.emplace_back(r, RHIAccessMode::eRead);

        // Sort by node ID to get a consistent order
        std::ranges::sort(accesses, [](auto& a, auto& b) { return a.first < b.first; });

        RDG_ID lastWriter = RDG_ID::UndefinedValue;
        RDG_ID lastReader = RDG_ID::UndefinedValue;

        for (const auto& [nodeId, mode] : accesses)
        {
            if (mode == RHIAccessMode::eReadWrite)
            {
                // WAW dependency: this writer depends on the last writer
                if (lastWriter.IsValid())
                {
                    if (AddNodeDepsForResource(pResource, nodeDependencies, lastWriter, nodeId))
                        inDegrees[nodeId]++;
                }
                // WAR dependency: make sure this writer happens after last reader
                if (lastReader.IsValid())
                {
                    if (AddNodeDepsForResource(pResource, nodeDependencies, lastReader, nodeId))
                        inDegrees[nodeId]++;
                }
                // Update last writer
                lastWriter = nodeId;
            }
            else if (mode == RHIAccessMode::eRead)
            {
                // RAW dependency: this reader depends on the last writer
                if (lastWriter.IsValid())
                {
                    if (AddNodeDepsForResource(pResource, nodeDependencies, lastWriter, nodeId))
                        inDegrees[nodeId]++;
                }
                lastReader = nodeId;
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
        std::vector<RDG_ID> currentLevel;
        const auto levelSize = queue.size();
        for (auto i = 0; i < levelSize; i++)
        {
            RDG_ID nodeId = queue.front();
            queue.pop();
            currentLevel.push_back(nodeId);
            sortedCount++;
            for (auto& neighbour : nodeDependencies[nodeId])
            {
                inDegrees[neighbour]--;
                if (inDegrees[neighbour] == 0)
                {
                    queue.push(neighbour);
                }
            }
        }
        m_sortedNodes.push_back(std::move(currentLevel));
    }
}

void RenderGraph::Destroy()
{
    // for (RDGPassChildNode* child : m_allChildNodes)
    // {
    //     ZEN_MEM_FREE(child);
    //     // delete child;
    // }
    // m_allChildNodes.clear();

    for (RDGResource* pResource : m_resources)
    {
        m_resourceAllocator.Free(pResource);
    }

    m_poolAlloc.Reset();

    // m_nodeData.clear();
    // m_nodeDataOffset.clear();
}

void RenderGraph::Begin()
{
    LOGI("==========Render Graph Begin==========");

    for (RDGResource* pResource : m_resources)
    {
        m_resourceAllocator.Free(pResource);
    }
    // m_nodeData.clear();
    // m_nodeDataOffset.clear();
    m_sortedNodes.clear();
    m_resources.clear();
    m_nodeAccessMap.clear();
    m_resourceMap.clear();
    m_bufferTransitions.clear();
    m_textureTransitions.clear();
    m_resourceFirstUseNodeMap.clear();
    m_nodeCount = 0;
}

void RenderGraph::End()
{
    // sort nodes
    SortNodesV2();
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
        }
#endif
    }
    LOGI("==========Render Graph End==========");
}

// void RenderGraph::Execute(RHICommandList* cmdList)
// {
//     m_cmdList = cmdList;
//     // execute node level by level
//     for (auto i = 0; i < m_sortedNodes.size(); i++)
//     {
//         const auto& currLevel = m_sortedNodes[i];
//         EmitInitializationBarriers(i);
//         for (auto& nodeId : currLevel)
//         {
//             RDGNodeBase* node = GetNodeBaseById(nodeId);
//             RunNode(node);
//         }
//         EmitTransitionBarriers(i);
//     }
// }

void RenderGraph::Execute(RHICommandList* pCmdList)
{
    m_pCmdList = pCmdList;
    // execute node level by level
    for (auto i = 0; i < m_sortedNodes.size(); i++)
    {
        const auto& currLevel = m_sortedNodes[i];
        EmitInitializationBarriers(i);
        for (auto& nodeId : currLevel)
        {
            RDGNodeBase* pNode = GetNodeBaseById(nodeId);
            RunNode(pNode);
        }
        EmitTransitionBarriers(i);
    }
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

void RenderGraph::EmitTransitionBarriers(uint32_t level)
{
    if (level == m_sortedNodes.size() - 1)
    {
        // last level, do not emit barriers
        return;
    }
    const auto& currLevel = m_sortedNodes[level];
    const auto& nextLevel = m_sortedNodes[level + 1];
    HeapVector<RHIBufferTransition> bufferTransitions;
    HeapVector<RHITextureTransition> textureTransitions;
    BitField<RHIPipelineStageBits> srcStages;
    BitField<RHIPipelineStageBits> dstStages;
    for (const auto& srcNodeId : currLevel)
    {
        for (const auto& dstNodeId : nextLevel)
        {
            auto nodePairKey = CreateNodePairKey(srcNodeId, dstNodeId);
            srcStages.SetFlag(GetNodeBaseById(srcNodeId)->selfStages);
            dstStages.SetFlag(GetNodeBaseById(dstNodeId)->selfStages);
            if (m_bufferTransitions.contains(nodePairKey))
            {
                auto& transitions = m_bufferTransitions[nodePairKey];

                // for (auto& transition : transitions)
                // {
                //     srcStages.SetFlag(RHIBufferUsageToPipelineStage(transition.oldUsage));
                //     dstStages.SetFlag(RHIBufferUsageToPipelineStage(transition.newUsage));
                // }
                for (auto& transition : transitions)
                {
                    // update tracker state
                    s_trackerPool.UpdateTrackerState(transition.pBuffer, transition.newAccessMode,
                                                     transition.newUsage);
                    bufferTransitions.emplace_back(transition);
                }
                // bufferTransitions.insert(bufferTransitions.end(), transitions.begin(),
                //                          transitions.end());
            }
            if (m_textureTransitions.contains(nodePairKey))
            {
                auto& transitions = m_textureTransitions[nodePairKey];
                // for (auto& transition : transitions)
                // {
                //     srcStages.SetFlag(RHITextureUsageToPipelineStage(transition.oldUsage));
                //     dstStages.SetFlag(RHITextureUsageToPipelineStage(transition.newUsage));
                // }
                for (auto& transition : transitions)
                {
                    // update tracker state
                    s_trackerPool.UpdateTrackerState(transition.pTexture, transition.newAccessMode,
                                                     transition.newUsage);
                    textureTransitions.emplace_back(transition);
                }

                // textureTransitions.insert(textureTransitions.end(), transitions.begin(),
                //                           transitions.end());
            }
        }
    }
    m_pCmdList->AddTransitions(srcStages, dstStages, {}, bufferTransitions, textureTransitions);
}

void RenderGraph::EmitInitializationBarriers(uint32_t level)
{
    HeapVector<RHITextureTransition> textureTransitions;
    HeapVector<RHIBufferTransition> bufferTransitions;
    BitField<RHIPipelineStageBits> srcStages;
    BitField<RHIPipelineStageBits> dstStages;
    srcStages.SetFlag(RHIPipelineStageBits::eAllCommands); // todo: is it correct?

    const auto& currLevel = m_sortedNodes[level];
    for (const auto& kv : m_resourceFirstUseNodeMap)
    {
        const auto& resourceId  = kv.first;
        const auto& firstNodeId = kv.second;

        for (const auto& nodeId : currLevel)
        {
            if (firstNodeId == nodeId)
            {
                dstStages.SetFlag(GetNodeBaseById(nodeId)->selfStages);
                RDGResource* pResource   = m_resources[resourceId];
                const auto& nodeAccesses = m_nodeAccessMap[nodeId];
                for (const auto& access : nodeAccesses)
                {
                    if (access.resourceId == resourceId)
                    {
                        if (pResource->type == RDGResourceType::eTexture)
                        {
                            RDGResourceTracker* pTracker =
                                s_trackerPool.GetTracker(pResource->pPhysicalRes);
                            RHITextureTransition textureTransition;
                            textureTransition.pTexture =
                                dynamic_cast<RHITexture*>(pResource->pPhysicalRes);
                            // set oldUsage and access based on RDGResourceTracker
                            textureTransition.oldUsage         = pTracker->textureUsage;
                            textureTransition.newUsage         = access.textureUsage;
                            textureTransition.oldAccessMode    = pTracker->accessMode;
                            textureTransition.newAccessMode    = access.accessMode;
                            textureTransition.subResourceRange = access.textureSubResourceRange;

                            textureTransitions.emplace_back(textureTransition);
                            // update tracker state
                            s_trackerPool.UpdateTrackerState(textureTransition.pTexture,
                                                             textureTransition.newAccessMode,
                                                             textureTransition.newUsage);
                            // dstStages.SetFlag(GetNodeBaseById(nodeId)->selfStages);
                            // dstStages.SetFlag(
                            //     RHITextureUsageToPipelineStage(textureTransition.newUsage));
                        }
                        if (pResource->type == RDGResourceType::eBuffer)
                        {
                            RDGResourceTracker* pTracker =
                                s_trackerPool.GetTracker(pResource->pPhysicalRes);
                            RHIBufferTransition bufferTransition;
                            bufferTransition.pBuffer =
                                dynamic_cast<RHIBuffer*>(pResource->pPhysicalRes);
                            bufferTransition.oldUsage      = pTracker->bufferUsage;
                            bufferTransition.newUsage      = access.bufferUsage;
                            bufferTransition.oldAccessMode = pTracker->accessMode;
                            bufferTransition.newAccessMode = access.accessMode;
                            bufferTransition.offset        = 0;

                            bufferTransitions.emplace_back(bufferTransition);
                            // update tracker state
                            s_trackerPool.UpdateTrackerState(bufferTransition.pBuffer,
                                                             bufferTransition.newAccessMode,
                                                             bufferTransition.newUsage);
                            // dstStages.SetFlag(GetNodeBaseById(nodeId)->selfStages);
                            // dstStages.SetFlag(
                            //     RHIBufferUsageToPipelineStage(bufferTransition.newUsage));
                        }
                    }
                }
            }
        }
    }

    if (!textureTransitions.empty() || !bufferTransitions.empty())
    {
        m_pCmdList->AddTransitions(srcStages, dstStages, {}, bufferTransitions, textureTransitions);
    }
}
} // namespace zen::rc
