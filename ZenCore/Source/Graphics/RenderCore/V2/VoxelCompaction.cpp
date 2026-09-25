#include "Graphics/RenderCore/V2/Renderer/VoxelizerBase.h"
#include "Graphics/RenderCore/V2/RenderDevice.h"
#include "Graphics/RenderCore/V2/RenderConfig.h"
#include "Graphics/RenderCore/V2/DynamicVoxelGIPlanning.h"

namespace zen::rc
{
bool VoxelizerBase::ConfigureClass(uint32_t mask)
{
    const bool valid = m_voxelTextures.pOwner == nullptr &&
        (mask == GI_STATIC || mask == GI_DYNAMIC || mask == GI_ALL);
    if (valid)
    {
        m_classMask = mask;
    }
    return valid;
}

bool VoxelizerBase::EnableCompaction()
{
    uint64_t bytes = 0;
    bool valid     = IsReady() &&
        ValidateGIStorageBuffer(m_voxelCount, sizeof(uint32_t), m_pRenderDevice->GetGPUInfo(),
                                bytes) == GIResourceStatus::eSuccess;
    if (valid && m_occupiedList == nullptr)
    {
        RHIBufferCreateInfo info;
        info.allocateType = RHIBufferAllocateType::eGPU;
        info.usageFlags.SetFlags(RHIBufferUsageFlagBits::eStorageBuffer,
                                 RHIBufferUsageFlagBits::eTransferSrcBuffer,
                                 RHIBufferUsageFlagBits::eTransferDstBuffer);
        info.size       = static_cast<uint32_t>(bytes);
        info.tag        = "voxel_occupied_list";
        m_occupiedList  = m_pRenderDevice->CreateBuffer(info);
        info.tag        = "voxel_grid_to_list";
        m_gridToList    = m_pRenderDevice->CreateBuffer(info);
        info.tag        = "voxel_occupied_count";
        info.size       = sizeof(uint32_t);
        m_occupiedCount = m_pRenderDevice->CreateBuffer(info);
        valid = m_occupiedList != nullptr && m_gridToList != nullptr && m_occupiedCount != nullptr;
        if (valid)
        {
            RequestVoxelization();
        }
    }
    return valid && m_occupiedList != nullptr && m_gridToList != nullptr &&
        m_occupiedCount != nullptr;
}

void VoxelizerBase::BuildCompaction(RDGQueuePreference queuePreference)
{
    if (m_visibilityChanged && m_occupiedList != nullptr && m_gridToList != nullptr &&
        m_occupiedCount != nullptr)
    {
        const glm::uvec3 groups =
            GetVoxelVolumeDispatchGroups(m_voxelTexResolution, m_pRenderDevice->GetGPUInfo());
        for (uint32_t phase = 0; phase < 2; ++phase)
        {
            RDGComputePassDesc pass;
            pass.SetShaderProgramName(phase == 0 ? "VoxelCompactClearSP" : "VoxelCompactSP");
            pass.SetPassTag(phase == 0 ? "ClearVoxelList" : "CompactVoxelList");
            pass.SetQueuePreference(queuePreference);
            pass.BindStorageImage("voxelOwner", m_voxelTextures.pOwner->GetDefaultView());
            pass.BindStorageBuffer("OccupiedList", m_occupiedList,
                                   phase == 0 ? RDGContentGuarantee::eFullWrite :
                                                RDGContentGuarantee::eProducedElements);
            pass.BindStorageBuffer("OccupiedCount", m_occupiedCount,
                                   phase == 0 ? RDGContentGuarantee::eFullWrite :
                                                RDGContentGuarantee::eNone);
            if (phase != 0)
            {
                pass.BindStorageBuffer("GridToList", m_gridToList, RDGContentGuarantee::eFullWrite);
            }
            m_pRenderDevice->GetCurrentFrameRDG()
                ->AddComputePass(std::move(pass))
                .RecordPassCommands([groups](RDGPassCmdEncoder& encoder) {
                    encoder.Dispatch(groups.x, groups.y, groups.z);
                });
        }
    }
}
} // namespace zen::rc
