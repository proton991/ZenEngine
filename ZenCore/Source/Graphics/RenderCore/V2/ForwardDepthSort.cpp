#include "Graphics/RenderCore/V2/Renderer/ForwardDepthSort.h"
#include "Graphics/RenderCore/V2/RenderDevice.h"
#include "Graphics/RenderCore/V2/RenderGraph/RenderGraph.h"
#include <bit>

namespace zen::rc
{
namespace
{
RDGBuffer SortBuffer(RenderGraph& graph, NameID name, uint64_t bytes)
{
    RDGBufferDesc description;
    description.name = name;
    description.size = bytes;
    description.usageFlags.SetFlag(RHIBufferUsageFlagBits::eStorageBuffer);
    return graph.GetResourceManager()->CreateBuffer(description);
}

void AddSortDispatch(RenderGraph& graph, RDGComputePassDesc description, glm::uvec4 parameters, uint32_t groups)
{
    graph.AddComputePass(std::move(description)).RecordPassCommands([parameters, groups](RDGPassCmdEncoder& encoder) {
        encoder.SetPushConstants(parameters);
        encoder.Dispatch(groups, 1, 1);
    });
}
} // namespace

bool SupportsForwardDepthSort(const RHIGPUInfo& gpu, uint32_t count)
{
    return count != 0 && count <= kForwardSortMaxKeys && gpu.supportComputeSubgroupBallot
        && gpu.maxComputeSharedMemorySize >= kForwardSortMaxGroups * sizeof(uint32_t)
        && gpu.maxComputeWorkGroupInvocations >= kForwardSortGroupSize
        && gpu.maxComputeWorkGroupSize[0] >= kForwardSortGroupSize
        && uint64_t(count) * sizeof(glm::uvec2) <= gpu.maxStorageBufferRange
        && gpu.maxStorageBufferRange >= uint64_t(kForwardSortMaxGroups + 1u) * 16u * sizeof(uint32_t)
        && gpu.IsDispatchWithinLimits((count + kForwardSortGroupSize - 1u) / kForwardSortGroupSize, 1, 1)
        && gpu.IsDispatchWithinLimits(16, 1, 1);
}

RDGBuffer AddForwardDepthSort(RenderDevice& device, RenderGraph& graph, RDGBuffer input, uint32_t count)
{
    RDGBuffer result;
    if (input && SupportsForwardDepthSort(device.GetGPUInfo(), count))
    {
        const uint32_t  groups     = (count + kForwardSortGroupSize - 1u) / kForwardSortGroupSize;
        const uint32_t  scanLength = std::bit_ceil(groups);
        const RDGBuffer scratch    = SortBuffer(graph, "forward_depth_scratch", uint64_t(count) * sizeof(glm::uvec2));
        const RDGBuffer histogram  = SortBuffer(graph, "forward_depth_histogram", uint64_t(groups) * 16u * sizeof(uint32_t));
        const RDGBuffer offsets    = SortBuffer(graph, "forward_depth_offsets", uint64_t(groups + 1u) * 16u * sizeof(uint32_t));
        if (scratch && histogram && offsets)
        {
            for (uint32_t digit = 0; digit < 8; ++digit)
            {
                const RDGBuffer    source      = (digit & 1u) != 0 ? scratch : input;
                const RDGBuffer    destination = (digit & 1u) != 0 ? input : scratch;
                const glm::uvec4   parameters(count, groups, digit * 4u, 0);
                RDGComputePassDesc counts;
                counts.SetShaderProgramName("ForwardSortHistogramSP");
                counts.SetPassTag("ForwardSortHistogram");
                counts.BindStorageBuffer("SortInput", source);
                counts.BindStorageBuffer("SortHistogram", histogram, RDGContentGuarantee::eFullWrite);
                AddSortDispatch(graph, std::move(counts), parameters, groups);

                RDGComputePassDesc scan;
                scan.SetShaderProgramName("ForwardSortScanSP");
                scan.SetPassTag("ForwardSortScan");
                scan.BindStorageBuffer("ScanInput", histogram);
                scan.BindStorageBuffer("ScanOutput", offsets, RDGContentGuarantee::eFullWrite);
                AddSortDispatch(graph, std::move(scan), glm::uvec4(groups, scanLength, 16, 0), 16);

                RDGComputePassDesc scatter;
                scatter.SetShaderProgramName("ForwardSortScatterSP");
                scatter.SetPassTag("ForwardSortScatter");
                scatter.BindStorageBuffer("SortInput", source);
                scatter.BindStorageBuffer("SortOffsets", offsets);
                scatter.BindStorageBuffer("SortOutput", destination, RDGContentGuarantee::eFullWrite);
                AddSortDispatch(graph, std::move(scatter), parameters, groups);
            }
            result = input;
        }
    }
    return result;
}
} // namespace zen::rc
