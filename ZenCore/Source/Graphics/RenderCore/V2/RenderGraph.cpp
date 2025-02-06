#include "Graphics/RenderCore/V2/RenderGraph.h"
#ifdef ZEN_WIN32
#    include <queue>
#endif
namespace zen::rc
{
RDGPassNode* RenderGraph::AddGraphicsPassNode(rhi::RenderPassHandle renderPassHandle,
                                              rhi::FramebufferHandle framebufferHandle,
                                              rhi::Rect2<int> area,
                                              VectorView<rhi::RenderPassClearValue> clearValues,
                                              bool hasColorTarget,
                                              bool hasDepthTarget)
{
    auto* node           = AllocNode<RDGGraphicsPassNode>();
    node->renderPass     = std::move(renderPassHandle);
    node->framebuffer    = std::move(framebufferHandle);
    node->renderArea     = area;
    node->type           = RDGNodeType::eGraphicsPass;
    node->numAttachments = clearValues.size();

    for (auto i = 0; i < clearValues.size(); i++)
    {
        node->clearValues[i] = clearValues[i];
    }
    if (hasColorTarget)
    {
        node->selfStages.SetFlag(rhi::PipelineStageBits::eColorAttachmentOutput);
    }
    if (hasDepthTarget)
    {
        node->selfStages.SetFlag(rhi::PipelineStageBits::eEarlyFragmentTests);
        node->selfStages.SetFlag(rhi::PipelineStageBits::eLateFragmentTests);
    }

    return node;
}

RDGPassNode* RenderGraph::AddGraphicsPassNode(const rc::GraphicsPass& gfxPass,
                                              rhi::Rect2<int> area,
                                              VectorView<rhi::RenderPassClearValue> clearValues,
                                              bool hasColorTarget,
                                              bool hasDepthTarget)
{
    auto* node           = AllocNode<RDGGraphicsPassNode>();
    node->renderPass     = std::move(gfxPass.renderPass);
    node->framebuffer    = std::move(gfxPass.framebuffer);
    node->renderArea     = area;
    node->type           = RDGNodeType::eGraphicsPass;
    node->numAttachments = clearValues.size();

    for (auto i = 0; i < clearValues.size(); i++)
    {
        node->clearValues[i] = clearValues[i];
    }
    if (hasColorTarget)
    {
        node->selfStages.SetFlag(rhi::PipelineStageBits::eColorAttachmentOutput);
    }
    if (hasDepthTarget)
    {
        node->selfStages.SetFlag(rhi::PipelineStageBits::eEarlyFragmentTests);
        node->selfStages.SetFlag(rhi::PipelineStageBits::eLateFragmentTests);
    }
    AddGraphicsPassBindPipelineNode(node, gfxPass.pipeline, rhi::PipelineType::eGraphics);
    return node;
}

void RenderGraph::AddGraphicsPassBindIndexBufferNode(RDGPassNode* parent,
                                                     rhi::BufferHandle bufferHandle,
                                                     rhi::DataFormat format,
                                                     uint32_t offset)
{
    auto* node   = AllocPassChildNode<RDGBindIndexBufferNode>(parent);
    node->buffer = std::move(bufferHandle);
    node->format = format;
    node->offset = offset;
    node->type   = RDGPassCmdType::eBindIndexBuffer;
}

void RenderGraph::AddGraphicsPassBindVertexBufferNode(RDGPassNode* parent,
                                                      VectorView<rhi::BufferHandle> vertexBuffers,
                                                      VectorView<uint32_t> offsets)
{
    auto* node = AllocPassChildNode<RDGBindVertexBufferNode>(parent);
    node->vertexBuffers.resize(vertexBuffers.size());
    node->offsets.resize(offsets.size());
    for (auto i = 0; i < vertexBuffers.size(); i++)
    {
        node->vertexBuffers[i] = vertexBuffers[i];
        node->offsets[i]       = offsets[i];
    }
    node->type = RDGPassCmdType::eBindVertexBuffer;
}

void RenderGraph::AddGraphicsPassBindPipelineNode(RDGPassNode* parent,
                                                  rhi::PipelineHandle pipelineHandle,
                                                  rhi::PipelineType pipelineType)
{
    auto* node         = AllocPassChildNode<RDGBindPipelineNode>(parent);
    node->pipeline     = std::move(pipelineHandle);
    node->pipelineType = pipelineType;
    node->type         = RDGPassCmdType::eBindPipeline;
}

void RenderGraph::AddGraphicsPassSetPushConstants(RDGPassNode* parent,
                                                  const void* data,
                                                  uint32_t dataSize)
{
    auto* node = AllocPassChildNode<RDGSetPushConstantsNode>(parent);
    node->type = RDGPassCmdType::eSetPushConstant;
    node->data.resize(dataSize);
    std::memcpy(node->data.data(), data, dataSize);
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

void RenderGraph::AddGraphicsPassSetBlendConstantNode(RDGPassNode* parent, const rhi::Color& color)
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

void RenderGraph::AddGraphicsPassSetScissorNode(RDGPassNode* parent, const rhi::Rect2<int>& scissor)
{
    auto* node    = AllocPassChildNode<RDGSetScissorNode>(parent);
    node->scissor = scissor;
    node->type    = RDGPassCmdType::eSetScissor;
}

void RenderGraph::AddGraphicsPassSetViewportNode(RDGPassNode* parent,
                                                 const rhi::Rect2<float>& viewport)
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

void RenderGraph::AddBufferClearNode(rhi::BufferHandle bufferHandle, uint32_t offset, uint64_t size)
{
    auto* node   = AllocNode<RDGBufferClearNode>();
    node->buffer = std::move(bufferHandle);
    node->offset = offset;
    node->size   = size;
    node->type   = RDGNodeType::eClearBuffer;
    node->selfStages.SetFlag(rhi::PipelineStageBits::eTransfer);

    RDGAccess access{};
    access.type        = RDGAccessType::eReadWrite;
    access.nodeId      = node->id;
    access.bufferUsage = rhi::BufferUsage::eTransferDst;

    RDGResource* resource = GetOrAllocResource(bufferHandle, RDGResourceType::eBuffer, node->id);
    resource->acessNodeMap[RDGAccessType::eReadWrite].push_back(node->id);
    access.resourceId = resource->id;

    m_nodeAccessMap[node->id].emplace_back(std::move(access));
}

void RenderGraph::AddBufferCopyNode(rhi::BufferHandle srcBufferHandle,
                                    rhi::BufferHandle dstBufferHandle,
                                    const rhi::BufferCopyRegion& copyRegion)
{
    auto* node      = AllocNode<RDGBufferCopyNode>();
    node->srcBuffer = std::move(srcBufferHandle);
    node->dstBuffer = std::move(dstBufferHandle);
    node->region    = copyRegion;
    node->type      = RDGNodeType::eCopyBuffer;
    node->selfStages.SetFlag(rhi::PipelineStageBits::eTransfer);
    // read access to src buffer
    {
        RDGAccess readAccess{};
        readAccess.type        = RDGAccessType::eRead;
        readAccess.bufferUsage = rhi::BufferUsage::eTransferSrc;
        readAccess.nodeId      = node->id;

        RDGResource* resource =
            GetOrAllocResource(srcBufferHandle, RDGResourceType::eBuffer, node->id);
        resource->acessNodeMap[RDGAccessType::eRead].push_back(node->id);
        readAccess.resourceId = resource->id;

        m_nodeAccessMap[node->id].emplace_back(std::move(readAccess));
    }
    // write access to dst buffer
    {
        RDGAccess writeAccess{};
        writeAccess.type        = RDGAccessType::eReadWrite;
        writeAccess.bufferUsage = rhi::BufferUsage::eTransferDst;
        writeAccess.nodeId      = node->id;

        RDGResource* resource =
            GetOrAllocResource(dstBufferHandle, RDGResourceType::eBuffer, node->id);
        resource->acessNodeMap[RDGAccessType::eReadWrite].push_back(node->id);
        writeAccess.resourceId = resource->id;

        m_nodeAccessMap[node->id].emplace_back(std::move(writeAccess));
    }
}

void RenderGraph::AddBufferUpdateNode(rhi::BufferHandle dstBufferHandle,
                                      const VectorView<rhi::BufferCopySource>& sources)
{
    auto* node = AllocNode<RDGBufferUpdateNode>();
    for (const auto& source : sources)
    {
        node->sources.push_back(source);
    }
    node->dstBuffer = std::move(dstBufferHandle);
    node->type      = RDGNodeType::eCopyBuffer;
    node->selfStages.SetFlag(rhi::PipelineStageBits::eTransfer);
    // only support update through staging buffer
    // staging buffer is neither owned nor managed by RDG
    // write access to dst buffer
    RDGAccess writeAccess{};
    writeAccess.type        = RDGAccessType::eReadWrite;
    writeAccess.bufferUsage = rhi::BufferUsage::eTransferDst;
    writeAccess.nodeId      = node->id;

    RDGResource* resource = GetOrAllocResource(dstBufferHandle, RDGResourceType::eBuffer, node->id);
    resource->acessNodeMap[RDGAccessType::eReadWrite].push_back(node->id);
    writeAccess.resourceId = resource->id;

    m_nodeAccessMap[node->id].emplace_back(std::move(writeAccess));
}

void RenderGraph::AddTextureClearNode(rhi::TextureHandle textureHandle,
                                      const rhi::Color& color,
                                      const rhi::TextureSubResourceRange& range)
{
    auto* node    = AllocNode<RDGTextureClearNode>();
    node->color   = color;
    node->range   = range;
    node->texture = std::move(textureHandle);
    node->type    = RDGNodeType::eClearTexture;
    node->selfStages.SetFlag(rhi::PipelineStageBits::eTransfer);

    RDGAccess access{};
    access.type                    = RDGAccessType::eReadWrite;
    access.nodeId                  = node->id;
    access.textureUsage            = rhi::TextureUsage::eTransferDst;
    access.textureSubResourceRange = range;

    RDGResource* resource = GetOrAllocResource(textureHandle, RDGResourceType::eTexture, node->id);
    resource->acessNodeMap[RDGAccessType::eReadWrite].push_back(node->id);
    access.resourceId = resource->id;

    m_nodeAccessMap[node->id].emplace_back(std::move(access));
}

void RenderGraph::AddTextureCopyNode(rhi::TextureHandle srcTextureHandle,
                                     const rhi::TextureSubResourceRange& srcTextureSubresourceRange,
                                     rhi::TextureHandle dstTextureHandle,
                                     const rhi::TextureSubResourceRange& dstTextureSubresourceRange,
                                     const VectorView<rhi::TextureCopyRegion>& regions)
{
    auto* node       = AllocNode<RDGTextureCopyNode>();
    node->type       = RDGNodeType::eCopyTexture;
    node->srcTexture = std::move(srcTextureHandle);
    node->dstTexture = std::move(dstTextureHandle);
    for (auto& region : regions)
    {
        node->copyRegions.push_back(region);
    }
    // read access to src texture
    {
        RDGAccess readAccess{};
        readAccess.textureSubResourceRange = srcTextureSubresourceRange;
        readAccess.type                    = RDGAccessType::eRead;
        readAccess.textureUsage            = rhi::TextureUsage::eTransferSrc;
        readAccess.nodeId                  = node->id;

        RDGResource* resource =
            GetOrAllocResource(srcTextureHandle, RDGResourceType::eTexture, node->id);
        resource->acessNodeMap[RDGAccessType::eRead].push_back(node->id);
        readAccess.resourceId = resource->id;

        m_nodeAccessMap[node->id].emplace_back(std::move(readAccess));
    }
    // write access to dst texture
    {
        RDGAccess writeAccess{};
        writeAccess.textureSubResourceRange = dstTextureSubresourceRange;
        writeAccess.type                    = RDGAccessType::eReadWrite;
        writeAccess.textureUsage            = rhi::TextureUsage::eTransferDst;
        writeAccess.nodeId                  = node->id;

        RDGResource* resource =
            GetOrAllocResource(dstTextureHandle, RDGResourceType::eTexture, node->id);
        resource->acessNodeMap[RDGAccessType::eReadWrite].push_back(node->id);
        writeAccess.resourceId = resource->id;

        m_nodeAccessMap[node->id].emplace_back(std::move(writeAccess));
    }
}

void RenderGraph::AddTextureReadNode(rhi::TextureHandle srcTextureHandle,
                                     rhi::BufferHandle dstBufferHandle,
                                     const VectorView<rhi::BufferTextureCopyRegion>& regions)
{
    auto* node       = AllocNode<RDGTextureReadNode>();
    node->srcTexture = std::move(srcTextureHandle);
    node->dstBuffer  = std::move(dstBufferHandle);
    for (auto& region : regions)
    {
        node->bufferTextureCopyRegions.push_back(region);
    }
    // read access to src texture
    {
        RDGAccess readAccess{};
        readAccess.type         = RDGAccessType::eRead;
        readAccess.textureUsage = rhi::TextureUsage::eTransferSrc;
        readAccess.nodeId       = node->id;

        RDGResource* resource =
            GetOrAllocResource(srcTextureHandle, RDGResourceType::eTexture, node->id);
        resource->acessNodeMap[RDGAccessType::eRead].push_back(node->id);
        readAccess.resourceId = resource->id;

        m_nodeAccessMap[node->id].emplace_back(std::move(readAccess));
    }
    // write access to dst buffer
    {
        RDGAccess writeAccess{};
        writeAccess.type        = RDGAccessType::eReadWrite;
        writeAccess.bufferUsage = rhi::BufferUsage::eTransferDst;
        writeAccess.nodeId      = node->id;

        RDGResource* resource =
            GetOrAllocResource(dstBufferHandle, RDGResourceType::eBuffer, node->id);
        resource->acessNodeMap[RDGAccessType::eReadWrite].push_back(node->id);
        writeAccess.resourceId = resource->id;

        m_nodeAccessMap[node->id].emplace_back(std::move(writeAccess));
    }
}

void RenderGraph::AddTextureUpdateNode(rhi::TextureHandle dstTextureHandle,
                                       const VectorView<rhi::BufferTextureCopySource>& sources)
{
    auto* node = AllocNode<RDGTextureUpdateNode>();
    for (const auto& source : sources)
    {
        node->sources.push_back(source);
    }
    node->dstTexture = std::move(dstTextureHandle);
    node->type       = RDGNodeType::eCopyBuffer;
    node->selfStages.SetFlag(rhi::PipelineStageBits::eTransfer);
    // only support update through staging buffer
    // staging buffer is neither owned nor managed by RDG
    // write access to dst texture
    RDGAccess writeAccess{};
    writeAccess.type         = RDGAccessType::eReadWrite;
    writeAccess.textureUsage = rhi::TextureUsage::eTransferDst;
    writeAccess.nodeId       = node->id;

    RDGResource* resource =
        GetOrAllocResource(dstTextureHandle, RDGResourceType::eTexture, node->id);
    resource->acessNodeMap[RDGAccessType::eReadWrite].push_back(node->id);
    writeAccess.resourceId = resource->id;

    m_nodeAccessMap[node->id].emplace_back(std::move(writeAccess));
}

void RenderGraph::AddTextureResolveNode(rhi::TextureHandle srcTextureHandle,
                                        rhi::TextureHandle dstTextureHandle,
                                        uint32_t srcLayer,
                                        uint32_t srcMipmap,
                                        uint32_t dstLayer,
                                        uint32_t dstMipMap)
{
    auto* node       = AllocNode<RDGTextureResolveNode>();
    node->srcTexture = std::move(srcTextureHandle);
    node->dstTexture = std::move(dstTextureHandle);
    node->srcLayer   = srcLayer;
    node->srcMipmap  = srcMipmap;
    node->dstLayer   = dstLayer;
    node->dstMipmap  = dstMipMap;
    node->selfStages.SetFlag(rhi::PipelineStageBits::eTransfer);
    // read access to src texture
    {
        RDGAccess readAccess{};
        readAccess.type         = RDGAccessType::eRead;
        readAccess.textureUsage = rhi::TextureUsage::eTransferSrc;
        readAccess.nodeId       = node->id;

        RDGResource* resource =
            GetOrAllocResource(srcTextureHandle, RDGResourceType::eTexture, node->id);
        resource->acessNodeMap[RDGAccessType::eRead].push_back(node->id);
        readAccess.resourceId = resource->id;

        m_nodeAccessMap[node->id].emplace_back(std::move(readAccess));
    }
    // write access to dst texture
    {
        RDGAccess writeAccess{};
        writeAccess.type         = RDGAccessType::eReadWrite;
        writeAccess.textureUsage = rhi::TextureUsage::eTransferDst;
        writeAccess.nodeId       = node->id;

        RDGResource* resource =
            GetOrAllocResource(dstTextureHandle, RDGResourceType::eTexture, node->id);
        resource->acessNodeMap[RDGAccessType::eReadWrite].push_back(node->id);
        writeAccess.resourceId = resource->id;

        m_nodeAccessMap[node->id].emplace_back(std::move(writeAccess));
    }
}
// todo: implement more concise and explicit layout transition (consider RenderPass's layout transition)
void RenderGraph::DeclareTextureAccessForPass(const RDGPassNode* passNode,
                                              rhi::TextureHandle textureHandle,
                                              rhi::TextureUsage usage,
                                              const rhi::TextureSubResourceRange& range,
                                              RDGAccessType accessType)
{
    RDGResource* resource =
        GetOrAllocResource(textureHandle, RDGResourceType::eTexture, passNode->id);
    resource->acessNodeMap[accessType].push_back(passNode->id);

    RDGAccess access{};
    access.type                    = accessType;
    access.resourceId              = resource->id;
    access.nodeId                  = passNode->id;
    access.textureUsage            = usage;
    access.textureSubResourceRange = range;
    m_nodeAccessMap[passNode->id].emplace_back(std::move(access));
}

void RenderGraph::SortNodes()
{
    // resolve node dependencies
    HashMap<RDG_ID, std::vector<RDG_ID>> nodeDpedencies;
    std::vector<uint32_t> inDegrees(m_nodeCount, 0);
    for (RDGResource* resource : m_resources)
    {
        if (resource->acessNodeMap.contains(RDGAccessType::eReadWrite) &&
            resource->acessNodeMap.contains(RDGAccessType::eRead))
        {
            bool isTexutre         = resource->type == RDGResourceType::eTexture;
            auto& writtenByNodeIds = resource->acessNodeMap[RDGAccessType::eReadWrite];
            auto& readByNodeIDs    = resource->acessNodeMap[RDGAccessType::eRead];
            std::ranges::sort(writtenByNodeIds);
            std::ranges::sort(readByNodeIDs);
            // Combine write and read access nodes into a single vector while preserving the order
            std::vector<RDG_ID> accessNodes = writtenByNodeIds;
            accessNodes.insert(accessNodes.end(), readByNodeIDs.begin(), readByNodeIDs.end());

            // Sort the combined access nodes by RDG_ID to maintain proper order
            std::ranges::sort(accessNodes.begin(), accessNodes.end());

            // Create dependencies between consecutive nodes
            for (size_t i = 1; i < accessNodes.size(); ++i)
            {
                RDG_ID srcNodeId = accessNodes[i - 1];
                RDG_ID dstNodeId = accessNodes[i];
                // Add the dependency src -> dst
                nodeDpedencies[srcNodeId].push_back(dstNodeId);
                auto nodePairId = CreateNodePairKey(srcNodeId, dstNodeId);

                uint32_t oldUsage = 0;
                uint32_t newUsage = 0;
                rhi::TextureSubResourceRange texutureSubResourceRange;
                for (const auto& access : m_nodeAccessMap[srcNodeId])
                {
                    if (access.resourceId == resource->id)
                    {
                        if (isTexutre)
                        {
                            oldUsage                 = ToUnderlying(access.textureUsage);
                            texutureSubResourceRange = access.textureSubResourceRange;
                        }
                        else
                        {
                            oldUsage = ToUnderlying(access.bufferUsage);
                        }
                    }
                }
                for (const auto& access : m_nodeAccessMap[dstNodeId])
                {
                    if (access.resourceId == resource->id)
                    {
                        if (isTexutre)
                        {
                            newUsage                 = ToUnderlying(access.textureUsage);
                            texutureSubResourceRange = access.textureSubResourceRange;
                        }
                        else
                        {
                            newUsage = ToUnderlying(access.bufferUsage);
                        }
                    }
                }
                if (resource->type == RDGResourceType::eBuffer)
                {
                    rhi::BufferTransition bufferTransition;
                    bufferTransition.bufferHandle =
                        rhi::BufferHandle(resource->physicalHandle.value);
                    bufferTransition.oldUsage = static_cast<rhi::BufferUsage>(oldUsage);
                    bufferTransition.newUsage = static_cast<rhi::BufferUsage>(newUsage);
                    m_bufferTransitions[nodePairId].emplace_back(bufferTransition);
                }
                else if (resource->type == RDGResourceType::eTexture)
                {
                    rhi::TextureTransition textureTransition;
                    textureTransition.textureHandle =
                        rhi::TextureHandle(resource->physicalHandle.value);
                    textureTransition.oldUsage         = static_cast<rhi::TextureUsage>(oldUsage);
                    textureTransition.newUsage         = static_cast<rhi::TextureUsage>(newUsage);
                    textureTransition.subResourceRange = texutureSubResourceRange;
                    m_textureTransitions[nodePairId].emplace_back(textureTransition);
                }
                else
                {
                    LOGE("Invalid RDGResource type!");
                }
                inDegrees[dstNodeId]++;
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
            for (auto& neighbour : nodeDpedencies[nodeId])
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
    if (sortedCount != m_nodeCount)
    {
        LOGE("Cycle detected in RenderGraph");
    }
}

void RenderGraph::Destroy()
{
    // todo: fix bugs in memory allocation for rdg nodes
    // for (RDGNodeBase* node : m_allNodes)
    // {
    //     if (node->type == RDGNodeType::eGraphicsPass)
    //     {
    //         auto* casted = reinterpret_cast<RDGGraphicsPassNode*>(node);
    //         for (RDGPassChildNode* child : casted->childNodes)
    //         {
    //             delete child;
    //         }
    //     }
    // }
    for (RDGResource* resource : m_resources)
    {
        m_resourceAllocator.Free(resource);
    }
}

void RenderGraph::Begin()
{
    for (RDGResource* resource : m_resources)
    {
        m_resourceAllocator.Free(resource);
    }
    m_nodeData.clear();
    m_nodeDataOffset.clear();
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
    SortNodes();
}

void RenderGraph::Execute(rhi::RHICommandList* cmdList)
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
            m_cmdList->ClearTexture(node->texture, node->color, node->range);
        }
        break;
        case RDGNodeType::eCopyTexture:
        {
            RDGTextureCopyNode* node = reinterpret_cast<RDGTextureCopyNode*>(base);
            m_cmdList->CopyTexture(node->srcTexture, node->dstTexture, node->copyRegions);
        }
        break;
        case RDGNodeType::eReadTexture:
        {
            RDGTextureReadNode* node = reinterpret_cast<RDGTextureReadNode*>(base);
            m_cmdList->CopyTextureToBuffer(node->srcTexture, node->dstBuffer,
                                           node->bufferTextureCopyRegions);
        }
        break;
        case RDGNodeType::eUpdateTexture:
        {
            RDGTextureUpdateNode* node = reinterpret_cast<RDGTextureUpdateNode*>(base);
            for (auto& source : node->sources)
            {
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

        case RDGNodeType::eComputePass:
        {
        }
        break;

        case RDGNodeType::eGraphicsPass:
        {
            RDGGraphicsPassNode* node = reinterpret_cast<RDGGraphicsPassNode*>(base);
            m_cmdList->BeginRenderPass(node->renderPass, node->framebuffer, node->renderArea,
                                       VectorView(node->clearValues, node->numAttachments));
            for (RDGPassChildNode* child : node->childNodes)
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
                        m_cmdList->BindVertexBuffers(cmdNode->vertexBuffers,
                                                     cmdNode->offsets.data());
                    }
                    break;
                    case RDGPassCmdType::eBindPipeline:
                    {
                        auto* cmdNode = reinterpret_cast<RDGBindPipelineNode*>(child);
                        if (cmdNode->pipelineType == rhi::PipelineType::eGraphics)
                        {
                            m_cmdList->BindGfxPipeline(cmdNode->pipeline);
                        }
                        else if (cmdNode->pipelineType == rhi::PipelineType::eCompute)
                        {
                            m_cmdList->BindComputePipeline(cmdNode->pipeline);
                        }
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
                    case RDGPassCmdType::eExecuteCommands: break;
                    case RDGPassCmdType::eNextSubpass: break;
                    case RDGPassCmdType::eDispatch: break;
                    case RDGPassCmdType::eDispatchIndirect: break;
                    case RDGPassCmdType::eSetPushConstant:
                    {
                        auto* cmdNode = reinterpret_cast<RDGSetPushConstantsNode*>(child);
                        rhi::PipelineHandle pipelineHandle;
                        for (auto* sibling : node->childNodes)
                        {
                            if (sibling->type == RDGPassCmdType::eBindPipeline)
                            {
                                auto* casted   = reinterpret_cast<RDGBindPipelineNode*>(sibling);
                                pipelineHandle = casted->pipeline;
                            }
                        }
                        m_cmdList->SetPushConstants(pipelineHandle, cmdNode->data);
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
            m_cmdList->EndRenderPass();
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
    std::vector<rhi::BufferTransition> bufferTransitions;
    std::vector<rhi::TextureTransition> textureTransitions;
    BitField<rhi::PipelineStageBits> srcStages;
    BitField<rhi::PipelineStageBits> dstStages;
    for (const auto& srcNodeId : currLevel)
    {
        for (const auto& dstNodeId : nextLevel)
        {
            auto nodePairKey = CreateNodePairKey(srcNodeId, dstNodeId);
            if (m_bufferTransitions.contains(nodePairKey))
            {
                srcStages.SetFlag(GetNodeBaseById(srcNodeId)->selfStages);
                dstStages.SetFlag(GetNodeBaseById(dstNodeId)->selfStages);
                auto& transitions = m_bufferTransitions[nodePairKey];
                bufferTransitions.insert(bufferTransitions.end(), transitions.begin(),
                                         transitions.end());
            }
            if (m_textureTransitions.contains(nodePairKey))
            {
                auto& transitions = m_textureTransitions[nodePairKey];
                for (auto& transition : transitions)
                {
                    srcStages.SetFlag(rhi::TextureUsageToPipelineStage(transition.oldUsage));
                    dstStages.SetFlag(rhi::TextureUsageToPipelineStage(transition.newUsage));
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
    std::vector<rhi::TextureTransition> textureTransitions;
    BitField<rhi::PipelineStageBits> srcStages;
    BitField<rhi::PipelineStageBits> dstStages;
    srcStages.SetFlag(rhi::PipelineStageBits::eTopOfPipe);

    const auto& currLevel = m_sortedNodes[level];
    for (const auto& kv : m_resourceFirstUseNodeMap)
    {
        const auto& resourceId  = kv.first;
        const auto& firstNodeId = kv.second;

        for (const auto& nodeId : currLevel)
        {
            if (firstNodeId == nodeId)
            {
                RDGResource* resource    = m_resources[resourceId];
                const auto& nodeAccesses = m_nodeAccessMap[nodeId];
                for (const auto& access : nodeAccesses)
                {
                    if (access.resourceId == resourceId)
                    {
                        if (resource->type == RDGResourceType::eTexture)
                        {
                            rhi::TextureTransition textureTransition;
                            textureTransition.textureHandle =
                                rhi::TextureHandle(resource->physicalHandle.value);
                            textureTransition.oldUsage = rhi::TextureUsage::eNone;
                            textureTransition.newUsage =
                                static_cast<rhi::TextureUsage>(access.textureUsage);
                            textureTransition.subResourceRange = access.textureSubResourceRange;
                            textureTransitions.emplace_back(textureTransition);
                            // dstStages.SetFlag(GetNodeBaseById(nodeId)->selfStages);
                            dstStages.SetFlag(
                                rhi::TextureUsageToPipelineStage(textureTransition.newUsage));
                        }
                    }
                }
            }
        }
    }
    if (!textureTransitions.empty())
    {
        m_cmdList->AddPipelineBarrier(srcStages, dstStages, {}, {}, textureTransitions);
    }
}
} // namespace zen::rc