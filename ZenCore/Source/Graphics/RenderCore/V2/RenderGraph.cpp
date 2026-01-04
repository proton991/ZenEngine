#include "Graphics/RenderCore/V2/RenderGraph.h"
#include "Graphics/RenderCore/V2/RenderResource.h"
#include "Graphics/RHI/RHIOptions.h"
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

RDGResourceTracker* RDGResourceTrackerPool::GetTracker(const RHIResource* resource)
{
    if (!m_trackerMap.contains(resource))
    {
        m_trackerMap[resource] = new RDGResourceTracker();
    }
    return m_trackerMap[resource];
}

void RDGResourceTrackerPool::UpdateTrackerState(const RHITexture* texture,
                                                RHIAccessMode accessMode,
                                                RHITextureUsage usage)
{
    if (m_trackerMap.contains(texture))
    {
        RDGResourceTracker* tracker = m_trackerMap[texture];
        tracker->accessMode         = accessMode;
        tracker->textureUsage       = usage;
    }
}

void RDGResourceTrackerPool::UpdateTrackerState(const RHIBuffer* buffer,
                                                RHIAccessMode accessMode,
                                                RHIBufferUsage usage)
{
    if (m_trackerMap.contains(buffer))
    {
        RDGResourceTracker* tracker = m_trackerMap[buffer];
        tracker->accessMode         = accessMode;
        tracker->bufferUsage        = usage;
    }
}

void RenderGraph::AddPassBindPipelineNode(RDGPassNode* parent,
                                          RHIPipeline* pipelineHandle,
                                          RHIPipelineType pipelineType)
{
    auto* node         = AllocPassChildNode<RDGBindPipelineNode>(parent);
    node->pipeline     = std::move(pipelineHandle);
    node->pipelineType = pipelineType;
    node->type         = RDGPassCmdType::eBindPipeline;
}

RDGPassNode* RenderGraph::AddComputePassNode(const ComputePass* pComputePass, std::string tag)
{
    VERIFY_EXPR_MSG(!tag.empty(), "compute pass node tag should not be empty");

    auto* node        = AllocNode<RDGComputePassNode>();
    node->type        = RDGNodeType::eComputePass;
    node->tag         = std::move(tag);
    node->computePass = pComputePass;
    node->selfStages.SetFlag(RHIPipelineStageBits::eComputeShader);
    for (uint32_t i = 0; i < pComputePass->numDescriptorSets; i++)
    {
        auto& setTrackers = pComputePass->resourceTrackers[i];
        for (auto& kv : setTrackers)
        {
            const PassResourceTracker& tracker = kv.second;
            if (tracker.resourceType == PassResourceType::eTexture)
            {
                for (auto* texture : tracker.textures)
                {
                    DeclareTextureAccessForPass(node, texture, tracker.textureUsage,
                                                texture->GetSubResourceRange(), tracker.accessMode);
                }
                // DeclareTextureAccessForPass(node, tracker.textureHandle, tracker.textureUsage,
                //                             tracker.textureSubResRange, tracker.accessMode,
                //                             tracker.name);
            }
            else if (tracker.resourceType == PassResourceType::eBuffer)
            {
                DeclareBufferAccessForPass(node, tracker.buffer, tracker.bufferUsage,
                                           tracker.accessMode);
            }
        }
    }
    AddPassBindPipelineNode(node, pComputePass->pipeline, RHIPipelineType::eCompute);
    return node;
}

void RenderGraph::AddComputePassDispatchNode(RDGPassNode* parent,
                                             uint32_t groupCountX,
                                             uint32_t groupCountY,
                                             uint32_t groupCountZ)
{
    auto* node        = AllocPassChildNode<RDGDispatchNode>(parent);
    node->type        = RDGPassCmdType::eDispatch;
    node->groupCountX = groupCountX;
    node->groupCountY = groupCountY;
    node->groupCountZ = groupCountZ;
}

void RenderGraph::AddComputePassDispatchIndirectNode(RDGPassNode* parent,
                                                     RHIBuffer* indirectBuffer,
                                                     uint32_t offset)
{
    auto* node           = AllocPassChildNode<RDGDispatchIndirectNode>(parent);
    node->type           = RDGPassCmdType::eDispatchIndirect;
    node->indirectBuffer = std::move(indirectBuffer);
    node->offset         = offset;

    DeclareBufferAccessForPass(parent, indirectBuffer, RHIBufferUsage::eIndirectBuffer,
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

RDGPassNode* RenderGraph::AddGraphicsPassNode(const GraphicsPass* pGfxPass,
                                              Rect2<int> area,
                                              VectorView<RHIRenderPassClearValue> clearValues,
                                              std::string tag)
{
    VERIFY_EXPR_MSG(!tag.empty(), "graphics pass node tag should not be empty");
    auto* node         = AllocNode<RDGGraphicsPassNode>();
    node->graphicsPass = pGfxPass;
    // node->renderPass       = std::move(pGfxPass->renderPass);
    // node->framebuffer      = std::move(pGfxPass->framebuffer);
    // node->renderArea       = area;
    node->type = RDGNodeType::eGraphicsPass;
    // node->numAttachments   = clearValues.size();
    // node->renderPassLayout = std::move(pGfxPass->renderPassLayout);
    // node->dynamic          = RHIOptions::GetInstance().UseDynamicRendering();
    node->tag = std::move(tag);

    const RHIRenderingLayout* pRenderingLayout = node->graphicsPass->pRenderingLayout;

    // for (auto i = 0; i < clearValues.size(); i++)
    // {
    //     node->clearValues[i] = clearValues[i];
    // }
    if (pRenderingLayout->numColorRenderTargets > 0)
    {
        node->selfStages.SetFlag(RHIPipelineStageBits::eColorAttachmentOutput);
        // const TextureHandle* handles = node->renderPassLayout.GetRenderTargetHandles();
        // const auto& rtSubresourceRanges   = node->renderPassLayout.GetRTSubResourceRanges();
        const auto& colorRTs = pRenderingLayout->colorRenderTargets;
        for (uint32_t i = 0; i < pRenderingLayout->numColorRenderTargets; i++)
        {
            const std::string textureTag = node->tag + "_color_rt_" + std::to_string(i);
            DeclareTextureAccessForPass(
                node, colorRTs[i].texture, RHITextureUsage::eColorAttachment,
                colorRTs[i].texture->GetSubResourceRange(), RHIAccessMode::eReadWrite);
        }
    }
    if (pRenderingLayout->hasDepthStencilRT)
    {
        const auto& depthStencilRT = pRenderingLayout->depthStencilRenderTarget;
        node->selfStages.SetFlag(RHIPipelineStageBits::eEarlyFragmentTests);
        node->selfStages.SetFlag(RHIPipelineStageBits::eLateFragmentTests);
        DeclareTextureAccessForPass(
            node, depthStencilRT.texture, RHITextureUsage::eDepthStencilAttachment,
            depthStencilRT.texture->GetSubResourceRange(), RHIAccessMode::eReadWrite);
    }
    AddPassBindPipelineNode(node, pGfxPass->pipeline, RHIPipelineType::eGraphics);
    for (uint32_t i = 0; i < pGfxPass->numDescriptorSets; i++)
    {
        auto& setTrackers = pGfxPass->resourceTrackers[i];
        for (auto& kv : setTrackers)
        {
            const PassResourceTracker& tracker = kv.second;
            if (tracker.resourceType == PassResourceType::eTexture)
            {
                for (auto* texture : tracker.textures)
                {
                    DeclareTextureAccessForPass(node, texture, tracker.textureUsage,
                                                texture->GetSubResourceRange(), tracker.accessMode);
                }
                // DeclareTextureAccessForPass(node, tracker.textureHandle, tracker.textureUsage,
                //                             tracker.textureSubResRange, tracker.accessMode,
                //                             tracker.name);
            }
            else if (tracker.resourceType == PassResourceType::eBuffer)
            {
                DeclareBufferAccessForPass(node, tracker.buffer, tracker.bufferUsage,
                                           tracker.accessMode);
            }
        }
    }

    return node;
}

void RenderGraph::AddGraphicsPassBindIndexBufferNode(RDGPassNode* parent,
                                                     RHIBuffer* buffer,
                                                     DataFormat format,
                                                     uint32_t offset)
{
    auto* node   = AllocPassChildNode<RDGBindIndexBufferNode>(parent);
    node->buffer = std::move(buffer);
    node->format = format;
    node->offset = offset;
    node->type   = RDGPassCmdType::eBindIndexBuffer;
    node->parent->selfStages.SetFlag(RHIPipelineStageBits::eVertexShader);
}

void RenderGraph::AddGraphicsPassBindVertexBufferNode(RDGPassNode* parent,
                                                      VectorView<RHIBuffer*> vertexBuffers,
                                                      VectorView<uint32_t> offsets)
{
    const uint32_t numBuffers = vertexBuffers.size();
    size_t nodeSize           = sizeof(RDGBindVertexBufferNode) + sizeof(RHIBuffer*) * numBuffers +
        sizeof(uint64_t) * numBuffers;
    auto* node = AllocPassChildNode<RDGBindVertexBufferNode>(parent, nodeSize);

    node->numBuffers = numBuffers;

    RHIBuffer** pVertexBuffers     = node->VertexBuffers();
    uint64_t* pVertexBufferOffsets = node->VertexBufferOffsets();

    for (auto i = 0; i < numBuffers; i++)
    {
        pVertexBuffers[i]       = vertexBuffers[i];
        pVertexBufferOffsets[i] = offsets[i];
    }

    // node->offsets = pVertexBufferOffsets;
    node->type = RDGPassCmdType::eBindVertexBuffer;
    node->parent->selfStages.SetFlag(RHIPipelineStageBits::eVertexShader);
}

void RenderGraph::AddGraphicsPassSetPushConstants(RDGPassNode* parent,
                                                  const void* data,
                                                  uint32_t dataSize)
{
    const size_t nodeSize = sizeof(RDGSetPushConstantsNode) + dataSize;
    auto* node            = AllocPassChildNode<RDGSetPushConstantsNode>(parent, nodeSize);
    node->type            = RDGPassCmdType::eSetPushConstant;
    node->dataSize        = dataSize;
    // node->data.resize(dataSize);
    std::memcpy(node->PCData(), data, dataSize);
}

void RenderGraph::AddComputePassSetPushConstants(RDGPassNode* parent,
                                                 const void* data,
                                                 uint32_t dataSize)
{
    const size_t nodeSize = sizeof(RDGSetPushConstantsNode) + dataSize;
    auto* node            = AllocPassChildNode<RDGSetPushConstantsNode>(parent, nodeSize);
    node->type            = RDGPassCmdType::eSetPushConstant;
    node->dataSize        = dataSize;
    // node->data.resize(dataSize);
    std::memcpy(node->PCData(), data, dataSize);
}

void RenderGraph::AddGraphicsPassDrawNode(RDGPassNode* parent,
                                          uint32_t vertexCount,
                                          uint32_t instanceCount)
{
    auto* node          = AllocPassChildNode<RDGDrawNode>(parent);
    node->type          = RDGPassCmdType::eDraw;
    node->vertexCount   = vertexCount;
    node->instanceCount = instanceCount;
}

void RenderGraph::AddGraphicsPassDrawIndexedNode(RDGPassNode* parent,
                                                 uint32_t indexCount,
                                                 uint32_t instanceCount,
                                                 uint32_t firstIndex,
                                                 int32_t vertexOffset,
                                                 uint32_t firstInstance)
{
    auto* node          = AllocPassChildNode<RDGDrawIndexedNode>(parent);
    node->type          = RDGPassCmdType::eDrawIndexed;
    node->indexCount    = indexCount;
    node->firstIndex    = firstIndex;
    node->instanceCount = instanceCount;
    node->vertexOffset  = vertexOffset;
    node->firstInstance = firstInstance;
}

void RenderGraph::AddGraphicsPassDrawIndexedIndirectNode(RDGPassNode* parent,
                                                         RHIBuffer* indirectBuffer,
                                                         uint32_t offset,
                                                         uint32_t drawCount,
                                                         uint32_t stride)
{
    auto* node           = AllocPassChildNode<RDGDrawIndexedIndirectNode>(parent);
    node->type           = RDGPassCmdType::eDrawIndexedIndirect;
    node->indirectBuffer = indirectBuffer;
    node->offset         = offset;
    node->drawCount      = drawCount;
    node->stride         = stride;
    node->parent->selfStages.SetFlags(RHIPipelineStageBits::eDrawIndirect);

    DeclareBufferAccessForPass(parent, indirectBuffer, RHIBufferUsage::eIndirectBuffer,
                               RHIAccessMode::eRead);
}

void RenderGraph::AddGraphicsPassSetBlendConstantNode(RDGPassNode* parent, const Color& color)
{
    auto* node  = AllocPassChildNode<RDGSetBlendConstantsNode>(parent);
    node->color = color;
    node->type  = RDGPassCmdType::eSetBlendConstant;
}

void RenderGraph::AddGraphicsPassSetLineWidthNode(RDGPassNode* parent, float width)
{
    auto* node  = AllocPassChildNode<RDGSetLineWidthNode>(parent);
    node->width = width;
    node->type  = RDGPassCmdType::eSetLineWidth;
}

void RenderGraph::AddGraphicsPassSetScissorNode(RDGPassNode* parent, const Rect2<int>& scissor)
{
    auto* node    = AllocPassChildNode<RDGSetScissorNode>(parent);
    node->scissor = scissor;
    node->type    = RDGPassCmdType::eSetScissor;
}

void RenderGraph::AddGraphicsPassSetViewportNode(RDGPassNode* parent, const Rect2<float>& viewport)
{
    auto* node     = AllocPassChildNode<RDGSetViewportNode>(parent);
    node->viewport = viewport;
    node->type     = RDGPassCmdType::eSetViewport;
}

void RenderGraph::AddGraphicsPassSetDepthBiasNode(RDGPassNode* parent,
                                                  float depthBiasConstantFactor,
                                                  float depthBiasClamp,
                                                  float depthBiasSlopeFactor)
{
    auto* node                    = AllocPassChildNode<RDGSetDepthBiasNode>(parent);
    node->depthBiasConstantFactor = depthBiasConstantFactor;
    node->depthBiasClamp          = depthBiasClamp;
    node->depthBiasSlopeFactor    = depthBiasSlopeFactor;
    node->type                    = RDGPassCmdType::eSetDepthBias;
}

void RenderGraph::AddBufferClearNode(RHIBuffer* buffer, uint32_t offset, uint64_t size)
{
    auto* node   = AllocNode<RDGBufferClearNode>();
    node->buffer = std::move(buffer);
    node->offset = offset;
    node->size   = size;
    node->type   = RDGNodeType::eClearBuffer;
    node->selfStages.SetFlag(RHIPipelineStageBits::eTransfer);

    RDGAccess access{};
    access.accessMode  = RHIAccessMode::eReadWrite;
    access.nodeId      = node->id;
    access.bufferUsage = RHIBufferUsage::eTransferDst;

    RDGResource* resource = GetOrAllocResource(buffer, RDGResourceType::eBuffer, node->id);
    // resource->accessNodeMap[RHIAccessMode::eReadWrite].push_back(node->id);

    resource->writtenByNodeIds.push_back(node->id);

    access.resourceId = resource->id;

    m_nodeAccessMap[node->id].emplace_back(std::move(access));
}

void RenderGraph::AddBufferCopyNode(RHIBuffer* srcBufferHandle,
                                    RHIBuffer* dstBufferHandle,
                                    const RHIBufferCopyRegion& copyRegion)
{
    auto* node      = AllocNode<RDGBufferCopyNode>();
    node->srcBuffer = std::move(srcBufferHandle);
    node->dstBuffer = std::move(dstBufferHandle);
    node->region    = copyRegion;
    node->type      = RDGNodeType::eCopyBuffer;
    node->selfStages.SetFlag(RHIPipelineStageBits::eTransfer);
    // read access to src buffer
    {
        RDGAccess readAccess{};
        readAccess.accessMode  = RHIAccessMode::eRead;
        readAccess.bufferUsage = RHIBufferUsage::eTransferSrc;
        readAccess.nodeId      = node->id;

        RDGResource* resource =
            GetOrAllocResource(srcBufferHandle, RDGResourceType::eBuffer, node->id);
        // resource->accessNodeMap[RHIAccessMode::eRead].push_back(node->id);
        resource->readByNodeIds.push_back(node->id);
        readAccess.resourceId = resource->id;

        m_nodeAccessMap[node->id].emplace_back(std::move(readAccess));
    }
    // write access to dst buffer
    {
        RDGAccess writeAccess{};
        writeAccess.accessMode  = RHIAccessMode::eReadWrite;
        writeAccess.bufferUsage = RHIBufferUsage::eTransferDst;
        writeAccess.nodeId      = node->id;

        RDGResource* resource =
            GetOrAllocResource(dstBufferHandle, RDGResourceType::eBuffer, node->id);
        // resource->accessNodeMap[RHIAccessMode::eReadWrite].push_back(node->id);
        resource->writtenByNodeIds.push_back(node->id);
        writeAccess.resourceId = resource->id;

        m_nodeAccessMap[node->id].emplace_back(std::move(writeAccess));
    }
}

void RenderGraph::AddBufferUpdateNode(RHIBuffer* dstBufferHandle,
                                      const VectorView<RHIBufferCopySource>& sources)
{
    auto* node = AllocNode<RDGBufferUpdateNode>();
    for (const auto& source : sources)
    {
        node->sources.push_back(source);
    }
    node->dstBuffer = std::move(dstBufferHandle);
    node->type      = RDGNodeType::eCopyBuffer;
    node->selfStages.SetFlag(RHIPipelineStageBits::eTransfer);
    // only support update through staging buffer
    // staging buffer is neither owned nor managed by RDG
    // write access to dst buffer
    RDGAccess writeAccess{};
    writeAccess.accessMode  = RHIAccessMode::eReadWrite;
    writeAccess.bufferUsage = RHIBufferUsage::eTransferDst;
    writeAccess.nodeId      = node->id;

    RDGResource* resource = GetOrAllocResource(dstBufferHandle, RDGResourceType::eBuffer, node->id);
    // resource->accessNodeMap[RHIAccessMode::eReadWrite].push_back(node->id);
    resource->writtenByNodeIds.push_back(node->id);
    writeAccess.resourceId = resource->id;

    m_nodeAccessMap[node->id].emplace_back(std::move(writeAccess));
}

void RenderGraph::AddTextureClearNode(RHITexture* texture,
                                      const Color& color,
                                      const RHITextureSubResourceRange& range)
{
    auto* node  = AllocNode<RDGTextureClearNode>();
    node->color = color;
    // node->range   = range;
    node->texture = texture;
    node->type    = RDGNodeType::eClearTexture;
    node->selfStages.SetFlag(RHIPipelineStageBits::eTransfer);

    RDGAccess access{};
    access.accessMode              = RHIAccessMode::eReadWrite;
    access.nodeId                  = node->id;
    access.textureUsage            = RHITextureUsage::eTransferDst;
    access.textureSubResourceRange = range;

    RDGResource* resource = GetOrAllocResource(texture, RDGResourceType::eTexture, node->id);
    // resource->accessNodeMap[RHIAccessMode::eReadWrite].push_back(node->id);
    resource->writtenByNodeIds.push_back(node->id);
    access.resourceId = resource->id;

    m_nodeAccessMap[node->id].emplace_back(std::move(access));
}

void RenderGraph::AddTextureCopyNode(RHITexture* srcTexture,
                                     RHITexture* dstTexture,
                                     const VectorView<RHITextureCopyRegion>& regions)
{
    const uint32_t numCopyRegions = regions.size();
    const size_t nodeSize =
        sizeof(RDGTextureCopyNode) + sizeof(RHITextureCopyRegion) * numCopyRegions;

    auto* node           = AllocNode<RDGTextureCopyNode>(nodeSize);
    node->numCopyRegions = numCopyRegions;
    node->type           = RDGNodeType::eCopyTexture;
    node->srcTexture     = srcTexture;
    node->dstTexture     = dstTexture;
    node->tag            = "texture_copy";
    node->selfStages.SetFlag(RHIPipelineStageBits::eTransfer);

    RHITextureCopyRegion* pCopyRegions = node->TextureCopyRegions();

    for (uint32_t i = 0; i < numCopyRegions; ++i)
    {
        // node->copyRegions.push_back(regions[i]);
        pCopyRegions[i] = regions[i];
    }

    // node->copyRegions = pCopyRegions;

    // read access to src texture
    RDGAccess readAccess{};
    readAccess.textureSubResourceRange = srcTexture->GetSubResourceRange();
    readAccess.accessMode              = RHIAccessMode::eRead;
    readAccess.textureUsage            = RHITextureUsage::eTransferSrc;
    readAccess.nodeId                  = node->id;

    RDGResource* srcResource = GetOrAllocResource(srcTexture, RDGResourceType::eTexture, node->id);
    srcResource->tag         = srcTexture->GetResourceTag();
    // if (srcResource->tag.empty())
    // {
    //     srcResource->tag = "src_texture";
    // }
    // srcResource->accessNodeMap[RHIAccessMode::eRead].push_back(node->id);
    srcResource->readByNodeIds.push_back(node->id);
    readAccess.resourceId = srcResource->id;

    m_nodeAccessMap[node->id].emplace_back(std::move(readAccess));

    // write access to dst texture
    RDGAccess writeAccess{};
    writeAccess.textureSubResourceRange = dstTexture->GetSubResourceRange();
    writeAccess.accessMode              = RHIAccessMode::eReadWrite;
    writeAccess.textureUsage            = RHITextureUsage::eTransferDst;
    writeAccess.nodeId                  = node->id;

    RDGResource* dstResource = GetOrAllocResource(dstTexture, RDGResourceType::eTexture, node->id);
    dstResource->tag         = dstTexture->GetResourceTag();
    // if (dstResource->tag.empty())
    // {
    //     dstResource->tag = "dst_texture";
    // }
    // dstResource->accessNodeMap[RHIAccessMode::eReadWrite].push_back(node->id);
    dstResource->writtenByNodeIds.push_back(node->id);
    writeAccess.resourceId = dstResource->id;

    m_nodeAccessMap[node->id].emplace_back(std::move(writeAccess));
}

void RenderGraph::AddTextureReadNode(RHITexture* srcTexture,
                                     RHIBuffer* dstBufferHandle,
                                     const VectorView<RHIBufferTextureCopyRegion>& regions)
{
    const size_t numCopyRegions = regions.size();
    const size_t nodeSize =
        sizeof(RDGTextureReadNode) + sizeof(RHIBufferTextureCopyRegion) * numCopyRegions;
    auto* node           = AllocNode<RDGTextureReadNode>(nodeSize);
    node->numCopyRegions = numCopyRegions;
    node->srcTexture     = srcTexture;
    node->dstBuffer      = dstBufferHandle;
    node->selfStages.SetFlag(RHIPipelineStageBits::eTransfer);

    auto* pCopyRegions = node->BufferTextureCopyRegions();

    // for (auto& region : regions)
    for (uint32_t i = 0; i < numCopyRegions; ++i)
    {
        pCopyRegions[i] = regions[i];
        // node->bufferTextureCopyRegions.push_back(region);
    }
    // read access to src texture
    {
        RDGAccess readAccess{};
        readAccess.accessMode   = RHIAccessMode::eRead;
        readAccess.textureUsage = RHITextureUsage::eTransferSrc;
        readAccess.nodeId       = node->id;

        RDGResource* resource = GetOrAllocResource(srcTexture, RDGResourceType::eTexture, node->id);
        // resource->accessNodeMap[RHIAccessMode::eRead].push_back(node->id);
        resource->readByNodeIds.push_back(node->id);
        readAccess.resourceId = resource->id;

        m_nodeAccessMap[node->id].emplace_back(std::move(readAccess));
    }
    // write access to dst buffer
    {
        RDGAccess writeAccess{};
        writeAccess.accessMode  = RHIAccessMode::eReadWrite;
        writeAccess.bufferUsage = RHIBufferUsage::eTransferDst;
        writeAccess.nodeId      = node->id;

        RDGResource* resource =
            GetOrAllocResource(dstBufferHandle, RDGResourceType::eBuffer, node->id);
        // resource->accessNodeMap[RHIAccessMode::eReadWrite].push_back(node->id);
        resource->writtenByNodeIds.push_back(node->id);
        writeAccess.resourceId = resource->id;

        m_nodeAccessMap[node->id].emplace_back(std::move(writeAccess));
    }
}

void RenderGraph::AddTextureUpdateNode(RHITexture* dstTexture,
                                       const VectorView<RHIBufferTextureCopySource>& sources)
{
    const uint32_t numCopySources = sources.size();
    const size_t nodeSize =
        sizeof(RHIBufferTextureCopySource) + sizeof(RHIBufferTextureCopySource) * numCopySources;
    auto* node         = AllocNode<RDGTextureUpdateNode>(nodeSize);
    auto* pCopySources = node->TextureCopySources();
    for (uint32_t i = 0; i < numCopySources; ++i)
    {
        pCopySources[i] = sources[i];
    }
    // for (const auto& source : sources)
    // {
    //     node->sources.push_back(source);
    // }
    node->numCopySources = numCopySources;
    node->dstTexture     = dstTexture;
    node->type           = RDGNodeType::eCopyBuffer;
    node->selfStages.SetFlag(RHIPipelineStageBits::eTransfer);
    // only support update through staging buffer
    // staging buffer is neither owned nor managed by RDG
    // write access to dst texture
    RDGAccess writeAccess{};
    writeAccess.accessMode   = RHIAccessMode::eReadWrite;
    writeAccess.textureUsage = RHITextureUsage::eTransferDst;
    writeAccess.nodeId       = node->id;

    RDGResource* resource = GetOrAllocResource(dstTexture, RDGResourceType::eTexture, node->id);
    // resource->accessNodeMap[RHIAccessMode::eReadWrite].push_back(node->id);
    resource->writtenByNodeIds.push_back(node->id);
    writeAccess.resourceId = resource->id;

    m_nodeAccessMap[node->id].emplace_back(std::move(writeAccess));
}

void RenderGraph::AddTextureResolveNode(RHITexture* srcTexture,
                                        RHITexture* dstTexture,
                                        uint32_t srcLayer,
                                        uint32_t srcMipmap,
                                        uint32_t dstLayer,
                                        uint32_t dstMipMap)
{
    auto* node       = AllocNode<RDGTextureResolveNode>();
    node->srcTexture = srcTexture;
    node->dstTexture = dstTexture;
    node->srcLayer   = srcLayer;
    node->srcMipmap  = srcMipmap;
    node->dstLayer   = dstLayer;
    node->dstMipmap  = dstMipMap;
    node->selfStages.SetFlag(RHIPipelineStageBits::eTransfer);
    // read access to src texture
    {
        RDGAccess readAccess{};
        readAccess.accessMode   = RHIAccessMode::eRead;
        readAccess.textureUsage = RHITextureUsage::eTransferSrc;
        readAccess.nodeId       = node->id;

        RDGResource* resource = GetOrAllocResource(srcTexture, RDGResourceType::eTexture, node->id);
        // resource->accessNodeMap[RHIAccessMode::eRead].push_back(node->id);
        resource->readByNodeIds.push_back(node->id);
        readAccess.resourceId = resource->id;

        m_nodeAccessMap[node->id].emplace_back(std::move(readAccess));
    }
    // write access to dst texture
    {
        RDGAccess writeAccess{};
        writeAccess.accessMode   = RHIAccessMode::eReadWrite;
        writeAccess.textureUsage = RHITextureUsage::eTransferDst;
        writeAccess.nodeId       = node->id;

        RDGResource* resource = GetOrAllocResource(dstTexture, RDGResourceType::eTexture, node->id);
        // resource->accessNodeMap[RHIAccessMode::eReadWrite].push_back(node->id);
        resource->writtenByNodeIds.push_back(node->id);
        writeAccess.resourceId = resource->id;

        m_nodeAccessMap[node->id].emplace_back(std::move(writeAccess));
    }
}

void RenderGraph::AddTextureMipmapGenNode(RHITexture* texture)
{
    auto* node    = AllocNode<RDGTextureMipmapGenNode>();
    node->type    = RDGNodeType::eGenTextureMipmap;
    node->texture = texture;
    node->selfStages.SetFlag(RHIPipelineStageBits::eTransfer);
}

void RenderGraph::DeclareTextureAccessForPass(const RDGPassNode* passNode,
                                              RHITexture* texture,
                                              RHITextureUsage usage,
                                              const RHITextureSubResourceRange& range,
                                              RHIAccessMode accessMode)
{
    RDGResource* resource = GetOrAllocResource(texture, RDGResourceType::eTexture, passNode->id);

    if (accessMode == RHIAccessMode::eReadWrite)
    {
        resource->writtenByNodeIds.push_back(passNode->id);
    }
    else if (accessMode == RHIAccessMode::eRead)
    {
        resource->readByNodeIds.push_back(passNode->id);
    }
    // resource->accessNodeMap[accessMode].push_back(passNode->id);

    resource->tag = texture->GetResourceTag() + "_layer_" + std::to_string(range.baseArrayLayer) +
        "_mip_" + std::to_string(range.baseMipLevel);
    // if (resource->tag.empty())
    //     resource->tag = tag;

    RDGNodeBase* baseNode = GetNodeBaseById(passNode->id);

    RDGAccess access{};
    access.accessMode              = accessMode;
    access.resourceId              = resource->id;
    access.nodeId                  = passNode->id;
    access.textureUsage            = usage;
    access.textureSubResourceRange = range;
    m_nodeAccessMap[passNode->id].emplace_back(std::move(access));

    if (usage == RHITextureUsage::eSampled)
    {
        baseNode->selfStages.SetFlags(RHIPipelineStageBits::eFragmentShader);
    }
    if (usage == RHITextureUsage::eDepthStencilAttachment)
    {
        baseNode->selfStages.SetFlags(RHIPipelineStageBits::eEarlyFragmentTests,
                                      RHIPipelineStageBits::eLateFragmentTests);
    }
    if (usage == RHITextureUsage::eStorage)
    {
        if (passNode->type == RDGNodeType::eGraphicsPass)
        {
            baseNode->selfStages.SetFlags(RHIPipelineStageBits::eFragmentShader);
        }
        if (passNode->type == RDGNodeType::eComputePass)
        {
            baseNode->selfStages.SetFlags(RHIPipelineStageBits::eComputeShader);
        }
    }
}

void RenderGraph::DeclareBufferAccessForPass(const RDGPassNode* passNode,
                                             RHIBuffer* buffer,
                                             RHIBufferUsage usage,
                                             RHIAccessMode accessMode)
{
    RDGResource* resource = GetOrAllocResource(buffer, RDGResourceType::eBuffer, passNode->id);
    // resource->accessNodeMap[accessMode].push_back(passNode->id);
    if (accessMode == RHIAccessMode::eReadWrite)
    {
        resource->writtenByNodeIds.push_back(passNode->id);
    }
    else if (accessMode == RHIAccessMode::eRead)
    {
        resource->readByNodeIds.push_back(passNode->id);
    }

    resource->tag = buffer->GetResourceTag();
    // if (resource->tag.empty())
    //     resource->tag = tag;

    RDGAccess access{};
    access.accessMode  = accessMode;
    access.resourceId  = resource->id;
    access.nodeId      = passNode->id;
    access.bufferUsage = usage;
    m_nodeAccessMap[passNode->id].emplace_back(std::move(access));

    RDGNodeBase* baseNode = GetNodeBaseById(passNode->id);

    if (passNode->type == RDGNodeType::eGraphicsPass)
    {
        if (usage == RHIBufferUsage::eIndirectBuffer)
        {
            baseNode->selfStages.SetFlags(RHIPipelineStageBits::eDrawIndirect);
        }
        baseNode->selfStages.SetFlags(RHIPipelineStageBits::eFragmentShader);
    }
    if (passNode->type == RDGNodeType::eComputePass)
    {
        baseNode->selfStages.SetFlags(RHIPipelineStageBits::eAllCommands);
    }
}

bool RenderGraph::AddNodeDepsForResource(RDGResource* resource,
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
        LOGI("RDG Node dependency: {} -> {}, resource: {}", srcNodeTag, dstNodeTag, resource->tag);
    }
#endif
    // Add the dependency src -> dst
    nodeDependencies[srcNodeId].push_back(dstNodeId);

    uint32_t oldUsage = 0;
    uint32_t newUsage = 0;
    RHIAccessMode oldAccessMode;
    RHIAccessMode newAccessMode;
    RHITextureSubResourceRange textureSubResourceRange;

    bool isTexture = resource->type == RDGResourceType::eTexture;
    for (const auto& access : m_nodeAccessMap[srcNodeId])
    {
        if (access.resourceId == resource->id)
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
        if (access.resourceId == resource->id)
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
    if (resource->type == RDGResourceType::eBuffer)
    {
        RHIBufferTransition bufferTransition;
        bufferTransition.buffer        = dynamic_cast<RHIBuffer*>(resource->physicalRes);
        bufferTransition.oldAccessMode = oldAccessMode;
        bufferTransition.newAccessMode = newAccessMode;
        bufferTransition.oldUsage      = static_cast<RHIBufferUsage>(oldUsage);
        bufferTransition.newUsage      = static_cast<RHIBufferUsage>(newUsage);
        m_bufferTransitions[nodePairId].emplace_back(bufferTransition);
    }
    else if (resource->type == RDGResourceType::eTexture)
    {
        RHITextureTransition textureTransition;
        textureTransition.texture          = dynamic_cast<RHITexture*>(resource->physicalRes);
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
//                          resource->tag);
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
    for (auto* resource : m_resources)
    {
        // auto& writers = resource->accessNodeMap[RHIAccessMode::eReadWrite];
        // auto& readers = resource->accessNodeMap[RHIAccessMode::eRead];

        // Merge writers and readers into one timeline
        std::vector<std::pair<RDG_ID, RHIAccessMode>> accesses;
        for (const auto& w : resource->writtenByNodeIds)
            accesses.emplace_back(w, RHIAccessMode::eReadWrite);
        for (const auto& r : resource->readByNodeIds)
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
                    if (AddNodeDepsForResource(resource, nodeDependencies, lastWriter, nodeId))
                        inDegrees[nodeId]++;
                }
                // WAR dependency: make sure this writer happens after last reader
                if (lastReader.IsValid())
                {
                    if (AddNodeDepsForResource(resource, nodeDependencies, lastReader, nodeId))
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
                    if (AddNodeDepsForResource(resource, nodeDependencies, lastWriter, nodeId))
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

    for (RDGResource* resource : m_resources)
    {
        m_resourceAllocator.Free(resource);
    }

    m_poolAlloc.Reset();

    // m_nodeData.clear();
    // m_nodeDataOffset.clear();
}

void RenderGraph::Begin()
{
    LOGI("==========Render Graph Begin==========");

    for (RDGResource* resource : m_resources)
    {
        m_resourceAllocator.Free(resource);
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
            RDGNodeBase* node = GetNodeBaseById(nodeId);
#if defined(ZEN_DEBUG)
            if (!node->tag.empty())
            {
                LOGI("RDG NodeTag after sort: {}, level: {}", node->tag, i);
            }
        }
#endif
    }
    LOGI("==========Render Graph End==========");
}

void RenderGraph::Execute(RHICommandList* cmdList)
{
    m_cmdList = cmdList;
    // execute node level by level
    for (auto i = 0; i < m_sortedNodes.size(); i++)
    {
        const auto& currLevel = m_sortedNodes[i];
        EmitInitializationBarriers(i);
        for (auto& nodeId : currLevel)
        {
            RDGNodeBase* node = GetNodeBaseById(nodeId);
            RunNode(node);
        }
        EmitTransitionBarriers(i);
    }
}

void RenderGraph::RunNode(RDGNodeBase* base)
{

    RDGNodeType type = base->type;
    switch (type)
    {
        case RDGNodeType::eClearBuffer:
        {
            RDGBufferClearNode* node = reinterpret_cast<RDGBufferClearNode*>(base);
            m_cmdList->ClearBuffer(node->buffer, node->offset, node->size);
        }
        break;
        case RDGNodeType::eCopyBuffer:
        {
            RDGBufferCopyNode* node = reinterpret_cast<RDGBufferCopyNode*>(base);
            m_cmdList->CopyBuffer(node->srcBuffer, node->dstBuffer, node->region);
        }
        break;
        case RDGNodeType::eUpdateBuffer:
        {
            RDGBufferUpdateNode* node = reinterpret_cast<RDGBufferUpdateNode*>(base);
            for (auto& source : node->sources)
            {
                m_cmdList->CopyBuffer(source.buffer, node->dstBuffer, source.region);
            }
        }
        break;
        case RDGNodeType::eClearTexture:
        {
            RDGTextureClearNode* node = reinterpret_cast<RDGTextureClearNode*>(base);
            m_cmdList->ClearTexture(node->texture, node->color,
                                    node->texture->GetSubResourceRange());
        }
        break;
        case RDGNodeType::eCopyTexture:
        {
            RDGTextureCopyNode* node = reinterpret_cast<RDGTextureCopyNode*>(base);
            m_cmdList->CopyTexture(node->srcTexture, node->dstTexture,
                                   VectorView(node->TextureCopyRegions(), node->numCopyRegions));
        }
        break;
        case RDGNodeType::eReadTexture:
        {
            RDGTextureReadNode* node = reinterpret_cast<RDGTextureReadNode*>(base);
            m_cmdList->CopyTextureToBuffer(
                node->srcTexture, node->dstBuffer,
                VectorView(node->BufferTextureCopyRegions(), node->numCopyRegions));
        }
        break;
        case RDGNodeType::eUpdateTexture:
        {
            RDGTextureUpdateNode* node = reinterpret_cast<RDGTextureUpdateNode*>(base);
            // for (auto& source : node->sources)
            for (uint32_t i = 0; i < node->numCopySources; i++)
            {
                const RHIBufferTextureCopySource& source = node->TextureCopySources()[i];
                m_cmdList->CopyBufferToTexture(source.buffer, node->dstTexture, source.region);
            }
        }
        break;
        case RDGNodeType::eResolveTexture:
        {
            RDGTextureResolveNode* node = reinterpret_cast<RDGTextureResolveNode*>(base);
            m_cmdList->ResolveTexture(node->srcTexture, node->dstTexture, node->srcLayer,
                                      node->srcMipmap, node->dstLayer, node->dstMipmap);
        }
        break;

        case RDGNodeType::eGenTextureMipmap:
        {
            RDGTextureMipmapGenNode* node = reinterpret_cast<RDGTextureMipmapGenNode*>(base);
            m_cmdList->GenerateTextureMipmaps(node->texture);
        }
        break;

        case RDGNodeType::eComputePass:
        {
            RDGComputePassNode* node = reinterpret_cast<RDGComputePassNode*>(base);
            // for (RDGPassChildNode* child : node->childNodes)
            for (RDGPassChildNode* child : m_passChildNodeMap[node->id])
            {
                switch (child->type)
                {
                    case RDGPassCmdType::eBindPipeline:
                    {
                        auto* cmdNode = reinterpret_cast<RDGBindPipelineNode*>(child);
                        // m_cmdList->BindComputePipeline(cmdNode->pipeline);
                        m_cmdList->BindComputePipeline(cmdNode->pipeline,
                                                       node->computePass->numDescriptorSets,
                                                       node->computePass->descriptorSets);
                    }
                    break;
                    case RDGPassCmdType::eDispatch:
                    {
                        auto* cmdNode = reinterpret_cast<RDGDispatchNode*>(child);
                        m_cmdList->Dispatch(cmdNode->groupCountX, cmdNode->groupCountY,
                                            cmdNode->groupCountZ);
                    }
                    break;
                    case RDGPassCmdType::eDispatchIndirect:
                    {
                        auto* cmdNode = reinterpret_cast<RDGDispatchIndirectNode*>(child);
                        m_cmdList->DispatchIndirect(cmdNode->indirectBuffer, cmdNode->offset);
                    }
                    break;
                    case RDGPassCmdType::eSetPushConstant:
                    {
                        auto* cmdNode = reinterpret_cast<RDGSetPushConstantsNode*>(child);
                        RHIPipeline* pipelineHandle;
                        for (auto* sibling : m_passChildNodeMap[node->id])
                        {
                            if (sibling->type == RDGPassCmdType::eBindPipeline)
                            {
                                auto* casted   = reinterpret_cast<RDGBindPipelineNode*>(sibling);
                                pipelineHandle = casted->pipeline;
                            }
                        }
                        m_cmdList->SetPushConstants(
                            pipelineHandle, VectorView(cmdNode->PCData(), cmdNode->dataSize));
                    }
                    break;
                    default: break;
                }
            }
        }
        break;

        case RDGNodeType::eGraphicsPass:
        {
            RDGGraphicsPassNode* node = reinterpret_cast<RDGGraphicsPassNode*>(base);
            m_cmdList->BeginRendering(node->graphicsPass->pRenderingLayout);
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
            for (RDGPassChildNode* child : m_passChildNodeMap[node->id])
            {
                switch (child->type)
                {
                    case RDGPassCmdType::eBindIndexBuffer:
                    {
                        auto* cmdNode = reinterpret_cast<RDGBindIndexBufferNode*>(child);
                        m_cmdList->BindIndexBuffer(cmdNode->buffer, cmdNode->format,
                                                   cmdNode->offset);
                    }
                    break;
                    case RDGPassCmdType::eBindVertexBuffer:
                    {
                        auto* cmdNode = reinterpret_cast<RDGBindVertexBufferNode*>(child);
                        m_cmdList->BindVertexBuffers(
                            VectorView(cmdNode->VertexBuffers(), cmdNode->numBuffers),
                            cmdNode->VertexBufferOffsets());
                    }
                    break;
                    case RDGPassCmdType::eBindPipeline:
                    {
                        auto* cmdNode = reinterpret_cast<RDGBindPipelineNode*>(child);
                        // m_cmdList->BindGfxPipeline(cmdNode->pipeline);
                        m_cmdList->BindGfxPipeline(cmdNode->pipeline,
                                                   node->graphicsPass->numDescriptorSets,
                                                   node->graphicsPass->descriptorSets);
                    }
                    break;
                    case RDGPassCmdType::eClearAttachment: break;
                    case RDGPassCmdType::eDraw:
                    {
                        auto* cmdNode = reinterpret_cast<RDGDrawNode*>(child);
                        m_cmdList->Draw(cmdNode->vertexCount, cmdNode->instanceCount, 0, 0);
                    }
                    break;
                    case RDGPassCmdType::eDrawIndexed:
                    {
                        auto* cmdNode = reinterpret_cast<RDGDrawIndexedNode*>(child);
                        m_cmdList->DrawIndexed(cmdNode->indexCount, cmdNode->instanceCount,
                                               cmdNode->firstIndex, cmdNode->vertexOffset,
                                               cmdNode->firstInstance);
                    }
                    break;
                    case RDGPassCmdType::eDrawIndexedIndirect:
                    {
                        auto* cmdNode = reinterpret_cast<RDGDrawIndexedIndirectNode*>(child);
                        m_cmdList->DrawIndexedIndirect(cmdNode->indirectBuffer, cmdNode->offset,
                                                       cmdNode->drawCount, cmdNode->stride);
                    }
                    break;
                    case RDGPassCmdType::eExecuteCommands: break;
                    case RDGPassCmdType::eNextSubpass: break;
                    case RDGPassCmdType::eDispatch: break;
                    case RDGPassCmdType::eDispatchIndirect: break;
                    case RDGPassCmdType::eSetPushConstant:
                    {
                        auto* cmdNode = reinterpret_cast<RDGSetPushConstantsNode*>(child);
                        RHIPipeline* pipelineHandle;
                        for (auto* sibling : m_passChildNodeMap[node->id])
                        {
                            if (sibling->type == RDGPassCmdType::eBindPipeline)
                            {
                                auto* casted   = reinterpret_cast<RDGBindPipelineNode*>(sibling);
                                pipelineHandle = casted->pipeline;
                            }
                        }
                        m_cmdList->SetPushConstants(
                            pipelineHandle, VectorView(cmdNode->PCData(), cmdNode->dataSize));
                    }
                    break;
                    case RDGPassCmdType::eSetLineWidth:
                    {
                        auto* cmdNode = reinterpret_cast<RDGSetLineWidthNode*>(child);
                        m_cmdList->SetLineWidth(cmdNode->width);
                    }
                    break;
                    case RDGPassCmdType::eSetBlendConstant:
                    {
                        auto* cmdNode = reinterpret_cast<RDGSetBlendConstantsNode*>(child);
                        m_cmdList->SetBlendConstants(cmdNode->color);
                    }
                    break;
                    case RDGPassCmdType::eSetScissor:
                    {
                        auto* cmdNode = reinterpret_cast<RDGSetScissorNode*>(child);
                        m_cmdList->SetScissors(cmdNode->scissor);
                    }
                    break;
                    case RDGPassCmdType::eSetViewport:
                    {
                        auto* cmdNode = reinterpret_cast<RDGSetViewportNode*>(child);
                        m_cmdList->SetViewports(cmdNode->viewport);
                    }
                    break;
                    case RDGPassCmdType::eSetDepthBias:
                    {
                        auto* cmdNode = reinterpret_cast<RDGSetDepthBiasNode*>(child);
                        m_cmdList->SetDepthBias(cmdNode->depthBiasConstantFactor,
                                                cmdNode->depthBiasClamp,
                                                cmdNode->depthBiasSlopeFactor);
                    }
                    break;

                    case RDGPassCmdType::eNone:
                    case RDGPassCmdType::eMax: break;
                }
            }
            m_cmdList->EndRendering();
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
    std::vector<RHIBufferTransition> bufferTransitions;
    std::vector<RHITextureTransition> textureTransitions;
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
                    s_trackerPool.UpdateTrackerState(transition.buffer, transition.newAccessMode,
                                                     transition.newUsage);
                }
                bufferTransitions.insert(bufferTransitions.end(), transitions.begin(),
                                         transitions.end());
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
                    s_trackerPool.UpdateTrackerState(transition.texture, transition.newAccessMode,
                                                     transition.newUsage);
                }

                textureTransitions.insert(textureTransitions.end(), transitions.begin(),
                                          transitions.end());
            }
        }
    }
    m_cmdList->AddPipelineBarrier(srcStages, dstStages, {}, bufferTransitions, textureTransitions);
}

void RenderGraph::EmitInitializationBarriers(uint32_t level)
{
    std::vector<RHITextureTransition> textureTransitions;
    std::vector<RHIBufferTransition> bufferTransitions;
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
                RDGResource* resource    = m_resources[resourceId];
                const auto& nodeAccesses = m_nodeAccessMap[nodeId];
                for (const auto& access : nodeAccesses)
                {
                    if (access.resourceId == resourceId)
                    {
                        if (resource->type == RDGResourceType::eTexture)
                        {
                            RDGResourceTracker* tracker =
                                s_trackerPool.GetTracker(resource->physicalRes);
                            RHITextureTransition textureTransition;
                            textureTransition.texture =
                                dynamic_cast<RHITexture*>(resource->physicalRes);
                            // set oldUsage and access based on RDGResourceTracker
                            textureTransition.oldUsage         = tracker->textureUsage;
                            textureTransition.newUsage         = access.textureUsage;
                            textureTransition.oldAccessMode    = tracker->accessMode;
                            textureTransition.newAccessMode    = access.accessMode;
                            textureTransition.subResourceRange = access.textureSubResourceRange;

                            textureTransitions.emplace_back(textureTransition);
                            // update tracker state
                            s_trackerPool.UpdateTrackerState(textureTransition.texture,
                                                             textureTransition.newAccessMode,
                                                             textureTransition.newUsage);
                            // dstStages.SetFlag(GetNodeBaseById(nodeId)->selfStages);
                            // dstStages.SetFlag(
                            //     RHITextureUsageToPipelineStage(textureTransition.newUsage));
                        }
                        if (resource->type == RDGResourceType::eBuffer)
                        {
                            RDGResourceTracker* tracker =
                                s_trackerPool.GetTracker(resource->physicalRes);
                            RHIBufferTransition bufferTransition;
                            bufferTransition.buffer =
                                dynamic_cast<RHIBuffer*>(resource->physicalRes);
                            bufferTransition.oldUsage      = tracker->bufferUsage;
                            bufferTransition.newUsage      = access.bufferUsage;
                            bufferTransition.oldAccessMode = tracker->accessMode;
                            bufferTransition.newAccessMode = access.accessMode;
                            bufferTransition.offset        = 0;

                            bufferTransitions.emplace_back(bufferTransition);
                            // update tracker state
                            s_trackerPool.UpdateTrackerState(bufferTransition.buffer,
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
        m_cmdList->AddPipelineBarrier(srcStages, dstStages, {}, bufferTransitions,
                                      textureTransitions);
    }
}
} // namespace zen::rc