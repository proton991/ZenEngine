#include "Graphics/RenderCore/V2/GIVisibilityProvider.h"
#include "Graphics/RenderCore/V2/ComputeDispatch.h"
#include "Graphics/RenderCore/V2/DynamicVoxelGIPlanning.h"
#include <cmath>

namespace zen::rc
{
namespace
{
bool ValidVolume(RHITexture* texture, uint32_t side, DataFormat format)
{
    return texture != nullptr && texture->GetWidth() == side && texture->GetHeight() == side &&
        texture->GetDepth() == side && texture->GetFormat() == format &&
        texture->GetBaseInfo().type == RHITextureType::e3D &&
        texture->GetBaseInfo().usageFlags.HasFlag(RHITextureUsageFlagBits::eStorage);
}

bool ValidVoxelInputs(const VoxelTextures& v, uint32_t side, bool averaged)
{
    return v.pAlbedoView != nullptr && v.pNormalView != nullptr && v.pEmissiveView != nullptr &&
        v.pAlbedoView->GetTexture() == v.pAlbedo && v.pNormalView->GetTexture() == v.pNormal &&
        v.pEmissiveView->GetTexture() == v.pEmissive &&
        ValidVolume(v.pOwner, side, DataFormat::eR32UInt) &&
        ValidVolume(v.pAlbedo, side, DataFormat::eR8G8B8A8UNORM) &&
        ValidVolume(v.pNormal, side, DataFormat::eR8G8B8A8UNORM) &&
        ValidVolume(v.pEmissive, side, DataFormat::eR16G16B16A16SFloat) &&
        (!averaged || ValidVolume(v.pReflectance, side, DataFormat::eR8G8B8A8UNORM));
}

void BindGrid(RDGComputePassDesc& pass, const VoxelTextures& v, bool dynamic, bool averaged)
{
    pass.BindStorageImage(dynamic ? "giDynamicOwner" : "giStaticOwner", v.pOwner->GetDefaultView());
    pass.BindStorageImage(dynamic ? "giDynamicAlbedo" : "giStaticAlbedo", v.pAlbedoView);
    pass.BindStorageImage(dynamic ? "giDynamicNormal" : "giStaticNormal", v.pNormalView);
    pass.BindStorageImage(dynamic ? "giDynamicEmission" : "giStaticEmission", v.pEmissiveView);
    pass.BindStorageImage(dynamic ? "giDynamicReflectance" : "giStaticReflectance",
                          averaged ? v.pReflectance->GetDefaultView() : v.pAlbedoView);
}

void RecordQueries(RDGPassCmdEncoder& encoder, const HeapVector<ComputeDispatchChunk>& chunks)
{
    for (const ComputeDispatchChunk& chunk : chunks)
    {
        encoder.SetPushConstants(glm::uvec2(chunk.firstItem, chunk.itemCount));
        encoder.Dispatch(chunk.groups.x, chunk.groups.y, chunk.groups.z);
    }
}
} // namespace

bool VoxelDDAProvider::Prepare(const VoxelTextures& staticVoxels,
                               const VoxelTextures& dynamicVoxels,
                               const GIGridUniform& grid,
                               uint64_t generation)
{
    const uint32_t side = grid.dimensions.x;
    bool valid = side > 0 && side <= 256 && generation != 0 && grid.dimensions.y <= GI_ALL &&
        grid.averaged.x <= 1 && grid.averaged.y <= 1 && std::isfinite(grid.minimumCellSize.x) &&
        std::isfinite(grid.minimumCellSize.y) && std::isfinite(grid.minimumCellSize.z) &&
        std::isfinite(grid.minimumCellSize.w) && grid.minimumCellSize.w > 0 &&
        ValidVoxelInputs(staticVoxels, side, grid.averaged.x != 0) &&
        ValidVoxelInputs(dynamicVoxels, side, grid.averaged.y != 0);
    for (uint32_t axis = 0; axis < 3; ++axis)
    {
        valid = valid && std::isfinite(grid.minimumCellSize[axis] + side * grid.minimumCellSize.w);
    }
    m_info = {GIVisibilityBackend::eVoxelDDA, generation, GI_CELL_PRECISION, valid, grid};
    if (valid)
    {
        m_static  = staticVoxels;
        m_dynamic = dynamicVoxels;
    }
    return valid;
}

bool VoxelDDAProvider::BindQueryInputs(RDGComputePassDesc& pass) const
{
    if (m_info.ready)
    {
        pass.BindValue("uGIGrid", m_info.grid);
        BindGrid(pass, m_static, false, m_info.grid.averaged.x != 0);
        BindGrid(pass, m_dynamic, true, m_info.grid.averaged.y != 0);
    }
    return m_info.ready;
}

NameID VoxelDDAProvider::GetQueryShader() const
{
    return "GIQueryDDASP";
}

bool DeterministicGIProvider::Prepare(RHIBuffer* responses,
                                      uint32_t count,
                                      uint64_t generation,
                                      const RHIGPUInfo& gpu)
{
    uint64_t bytes   = 0;
    const bool valid = count > 0 && generation != 0 && responses != nullptr &&
        ValidateGIStorageBuffer(count, sizeof(GIHit), gpu, bytes) == GIResourceStatus::eSuccess &&
        responses->GetRequiredSize() >= bytes;
    m_info      = {GIVisibilityBackend::eDeterministic, generation, GI_CELL_PRECISION, valid, {}};
    m_responses = valid ? responses : nullptr;
    m_count     = valid ? count : 0;
    return valid;
}

bool DeterministicGIProvider::BindQueryInputs(RDGComputePassDesc& pass) const
{
    if (m_info.ready)
    {
        pass.BindStorageBuffer("GIReferenceHits", m_responses);
        pass.BindValue("uGIReference", glm::uvec4(m_count, 0, 0, 0));
    }
    return m_info.ready;
}

NameID DeterministicGIProvider::GetQueryShader() const
{
    return "GIQueryReferenceSP";
}

bool BuildGIQueryPass(RenderGraph& graph,
                      const GIVisibilityProvider& provider,
                      RHIBuffer* requests,
                      RHIBuffer* results,
                      uint32_t count,
                      const RHIGPUInfo& gpu,
                      RDGQueuePreference queue)
{
    uint64_t requestBytes = 0;
    uint64_t resultBytes  = 0;
    bool valid            = provider.GetInfo().ready && requests != nullptr && results != nullptr &&
        ValidateGIStorageBuffer(count, sizeof(GIQuery), gpu, requestBytes) ==
            GIResourceStatus::eSuccess &&
        ValidateGIStorageBuffer(count, sizeof(GIQueryResult), gpu, resultBytes) ==
            GIResourceStatus::eSuccess &&
        requests->GetRequiredSize() >= requestBytes && results->GetRequiredSize() >= resultBytes;
    HeapVector<ComputeDispatchChunk> chunks;
    uint32_t first = 0;
    while (valid && first < count)
    {
        ComputeDispatchChunk chunk;
        valid = BuildComputeDispatchChunk(first, count - first, GI_QUERY_GROUP_SIZE, gpu, chunk);
        if (valid)
        {
            chunks.push_back(chunk);
            first += chunk.itemCount;
        }
    }
    if (valid && count > 0)
    {
        RDGComputePassDesc pass;
        pass.SetShaderProgramName(provider.GetQueryShader());
        pass.SetPassTag("GIQueryConformance");
        pass.SetQueuePreference(queue);
        pass.independentDispatches = true;
        valid                      = provider.BindQueryInputs(pass);
        if (valid)
        {
            pass.BindStorageBuffer("GIRequests", requests);
            pass.BindStorageBuffer("GIResults", results,
                                   results->GetRequiredSize() == resultBytes ?
                                       RDGContentGuarantee::eFullWrite :
                                       RDGContentGuarantee::eProducedElements);
            graph.AddComputePass(std::move(pass))
                .RecordPassCommands([chunks = std::move(chunks)](RDGPassCmdEncoder& encoder) {
                    RecordQueries(encoder, chunks);
                });
        }
    }
    return valid;
}
} // namespace zen::rc
