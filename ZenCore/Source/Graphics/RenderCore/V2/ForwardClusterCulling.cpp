#include "Graphics/RenderCore/V2/Renderer/ForwardGPUDraws.h"
#include "Graphics/RenderCore/V2/Renderer/ForwardDepthSort.h"
#include "Graphics/RenderCore/V2/RenderDevice.h"
#include "Graphics/RenderCore/V2/RenderGraph/RenderGraph.h"
#include <algorithm>
#include <bit>

namespace zen::rc
{
RDGTexture AddForwardOpaqueHiZ(RenderDevice& device, RenderGraph& graph, RHITexture* depth, uint32_t width, uint32_t height)
{
    RDGTextureDesc description;
    description.name              = "forward_opaque_hiz";
    description.texFormat.width   = width;
    description.texFormat.height  = height;
    description.texFormat.depth   = 1;
    description.texFormat.format  = DataFormat::eR32SFloat;
    description.texFormat.mipmaps = std::bit_width(std::max(width, height));
    description.usageFlags.SetFlags(RHITextureUsageFlagBits::eSampled, RHITextureUsageFlagBits::eStorage);
    const RDGTexture pyramid = graph.GetResourceManager()->CreateTexture(description);
    RHISampler*      sampler = device.CreateSampler({});
    for (uint32_t level = 0; level < description.texFormat.mipmaps; ++level)
    {
        RHITextureSubResourceRange range;
        range.aspect.SetFlag(RHITextureAspectFlagBits::eColor);
        range.baseMipLevel = level;
        const RDGTextureViewDesc target{range};
        RDGComputePassDesc       reduce;
        reduce.SetShaderProgramName(level == 0 ? "ForwardHiZCopySP" : "ForwardHiZSP");
        reduce.SetPassTag("ForwardHiZ");
        reduce.BindStorageImage("targetDepth", pyramid, target, RDGContentGuarantee::eFullWrite);
        if (level == 0)
        {
            reduce.BindSampledTexture("sourceDepth", sampler, depth->GetDefaultView());
        }
        else
        {
            range.baseMipLevel = level - 1u;
            reduce.BindStorageImage("sourceDepth", pyramid, RDGTextureViewDesc{range});
        }
        const glm::uvec2 groups((std::max(width >> level, 1u) + 7u) / 8u, (std::max(height >> level, 1u) + 7u) / 8u);
        graph.AddComputePass(std::move(reduce)).RecordPassCommands([groups](RDGPassCmdEncoder& encoder) {
            encoder.Dispatch(groups.x, groups.y, 1);
        });
    }
    return pyramid;
}

namespace
{
RDGBuffer CullBuffer(RenderGraph& graph, NameID name, uint64_t bytes)
{
    RDGBufferDesc description;
    description.name = name;
    description.size = bytes;
    description.usageFlags.SetFlags(RHIBufferUsageFlagBits::eStorageBuffer, RHIBufferUsageFlagBits::eTransferSrcBuffer);
    return graph.GetResourceManager()->CreateBuffer(description);
}
} // namespace

RDGBuffer AddForwardClusterVisibility(RenderDevice& device,
                                      RenderGraph&  graph,
                                      RDGBuffer     clusters,
                                      uint32_t      count,
                                      RDGTexture    pyramid,
                                      const Mat4&   projectionView)
{
    const RDGBuffer    visible = CullBuffer(graph, "forward_cluster_visibility", uint64_t(count) * sizeof(uint32_t));
    RDGComputePassDesc cull;
    cull.SetShaderProgramName("ForwardCullSP");
    cull.SetPassTag("ForwardCull");
    cull.BindValue("ForwardCullView", projectionView);
    cull.BindStorageBuffer("ForwardClusters", clusters);
    cull.BindStorageBuffer("ForwardClusterVisibility", visible, RDGContentGuarantee::eFullWrite);
    cull.BindSampledTexture("opaqueHiZ", device.CreateSampler({}), pyramid);
    graph.AddComputePass(std::move(cull)).RecordPassCommands([count](RDGPassCmdEncoder& encoder) {
        encoder.SetPushConstants(count);
        encoder.Dispatch((count + kForwardSortGroupSize - 1u) / kForwardSortGroupSize, 1, 1);
    });

    return visible;
}

void ForwardGPUDraws::Emit(RenderDevice&           device,
                           RenderGraph&            graph,
                           const ForwardGPUResult& result,
                           RHITexture*             depth,
                           uint32_t                width,
                           uint32_t                height,
                           const Mat4&             projectionView)
{
    const uint32_t   count        = static_cast<uint32_t>(m_contents.size());
    const uint32_t   clusterCount = static_cast<uint32_t>(m_clusterContents.size());
    const uint32_t   groups       = (count + kForwardSortGroupSize - 1u) / kForwardSortGroupSize;
    const RDGTexture pyramid      = AddForwardOpaqueHiZ(device, graph, depth, width, height);
    const RDGBuffer  visible      = AddForwardClusterVisibility(
        device, graph, graph.GetResourceManager()->ImportBuffer(m_clusters.GetBuffer()), clusterCount, pyramid, projectionView);
    const RDGBuffer ranks   = CullBuffer(graph, "forward_local_ranks", uint64_t(count) * sizeof(glm::uvec2));
    const RDGBuffer totals  = CullBuffer(graph, "forward_block_totals", uint64_t(groups) * sizeof(uint32_t));
    const RDGBuffer offsets = CullBuffer(graph, "forward_block_offsets", uint64_t(groups + 1u) * sizeof(uint32_t));

    RDGComputePassDesc prefix;
    prefix.SetShaderProgramName("ForwardPrefixSP");
    prefix.SetPassTag("ForwardPrefix");
    prefix.BindStorageBuffer("ForwardTriangles", m_triangles.GetBuffer());
    prefix.BindStorageBuffer("ForwardDepthKeys", result.keys);
    prefix.BindStorageBuffer("ForwardClusterVisibility", visible);
    prefix.BindStorageBuffer("ForwardLocalRanks", ranks, RDGContentGuarantee::eFullWrite);
    prefix.BindStorageBuffer("ForwardBlockTotals", totals, RDGContentGuarantee::eFullWrite);
    graph.AddComputePass(std::move(prefix)).RecordPassCommands([count, groups](RDGPassCmdEncoder& encoder) {
        encoder.SetPushConstants(count);
        encoder.Dispatch(groups, 1, 1);
    });

    RDGComputePassDesc scan;
    scan.SetShaderProgramName("ForwardSortScanSP");
    scan.SetPassTag("ForwardCullScan");
    scan.BindStorageBuffer("ScanInput", totals);
    scan.BindStorageBuffer("ScanOutput", offsets, RDGContentGuarantee::eFullWrite);
    graph.AddComputePass(std::move(scan)).RecordPassCommands([groups](RDGPassCmdEncoder& encoder) {
        encoder.SetPushConstants(glm::uvec4(groups, std::bit_ceil(groups), 1, 0));
        encoder.Dispatch(1, 1, 1);
    });

    RDGComputePassDesc emit;
    emit.SetShaderProgramName("ForwardEmitSP");
    emit.SetPassTag("ForwardEmit");
    emit.BindStorageBuffer("ForwardDepthKeys", result.keys);
    emit.BindStorageBuffer("ForwardLocalRanks", ranks);
    emit.BindStorageBuffer("ForwardBlockOffsets", offsets);
    emit.BindStorageBuffer("ForwardSortedIndices", result.indices, RDGContentGuarantee::eFullWrite);
    emit.BindStorageBuffer("ForwardArguments", result.arguments, RDGContentGuarantee::eFullWrite);
    graph.AddComputePass(std::move(emit)).RecordPassCommands([count, groups](RDGPassCmdEncoder& encoder) {
        encoder.SetPushConstants(glm::uvec2(count, groups));
        encoder.Dispatch(groups, 1, 1);
    });
}
} // namespace zen::rc
