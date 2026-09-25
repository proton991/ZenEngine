#include "Graphics/RenderCore/V2/Renderer/VoxelizerBase.h"
#include "Graphics/RenderCore/V2/RenderScene.h"
#include "Graphics/RenderCore/V2/Renderer/RendererUtils.h"
#include "Graphics/RenderCore/V2/RenderConfig.h"
#include "Graphics/RenderCore/V2/DynamicVoxelGIPlanning.h"
#include "Graphics/Shared/VoxelGI.h"
#include <bit>
#include <limits>

namespace zen::rc
{
VoxelizerBase::VoxelizerBase(RenderDevice* pRenderDevice, RHIViewport* pViewport) :
    m_pRenderDevice(pRenderDevice), m_pViewport(pViewport)
{}

bool VoxelizerBase::IsReady() const
{
    return m_voxelTextures.pOwner != nullptr && m_voxelTextures.pAlbedoView != nullptr &&
        m_pVoxelSampler != nullptr && m_pColorSampler != nullptr;
}

bool VoxelizerBase::EnsureReady()
{
    if (!IsReady() && !m_textureInitializationAttempted)
    {
        m_textureInitializationAttempted = true;
        PrepareTextures();
    }
    return IsReady();
}

bool VoxelizerBase::BeginVoxelization(RenderGraph& graph, RDGQueuePreference queuePreference)
{
    bool result = false;
    const uint64_t sceneRevision =
        m_pScene != nullptr ? m_pScene->GetGeometryRevision(m_classMask) : 0;
    const uint64_t surfaceRevision =
        m_pScene != nullptr ? m_pScene->GetSurfaceRevision(m_classMask) : 0;
    if ((m_needVoxelization || sceneRevision != m_sceneRevision ||
         surfaceRevision != m_surfaceRevision) &&
        !m_voxelizationPending && EnsureReady())
    {
        m_visibilityChanged = m_needVoxelization || sceneRevision != m_sceneRevision;
        PrepareReflectance();
        RDGComputePassDesc reset;
        reset.SetShaderProgramName(m_useAveragedReflectance ? "VoxelClearOwnersAveragedSP" :
                                                              "VoxelClearOwnersSP");
        reset.SetPassTag("ResetVoxelOwners");
        reset.SetQueuePreference(queuePreference);
        reset.BindStorageImage("voxelOwner", m_voxelTextures.pOwner->GetDefaultView(),
                               RDGContentGuarantee::eFullWrite);
        if (m_useAveragedReflectance)
        {
            reset.BindStorageBuffer("ReflectanceSums", m_pReflectanceSums,
                                    RDGContentGuarantee::eFullWrite);
        }
        const glm::uvec3 groups =
            GetVoxelVolumeDispatchGroups(m_voxelTexResolution, m_pRenderDevice->GetGPUInfo());
        graph.AddComputePass(std::move(reset))
            .RecordPassCommands([groups](RDGPassCmdEncoder& encoder) {
                encoder.Dispatch(groups.x, groups.y, groups.z);
            });
        m_voxelizationPending = true;
        m_pendingSceneRevision = sceneRevision;
        m_pendingSurfaceRevision = surfaceRevision;
        m_needVoxelization    = false;
        result                = true;
    }
    return result;
}

void VoxelizerBase::OnRenderGraphExecuted(bool succeeded)
{
    if (m_voxelizationPending)
    {
        if (succeeded)
        {
            ++m_geometryRevision;
            if (m_visibilityChanged)
            {
                ++m_visibilityRevision;
            }
            m_sceneRevision = m_pendingSceneRevision;
            m_surfaceRevision = m_pendingSurfaceRevision;
        }
        else
        {
            RequestVoxelization();
        }
        m_voxelizationPending = false;
    }
}

RHITexture* VoxelizerBase::CreateVolume(DataFormat format, NameID name, uint32_t mipCount)
{
    TextureFormat info;
    info.dimension = TextureDimension::e3D;
    info.format    = format;
    info.width     = m_voxelTexResolution;
    info.height    = m_voxelTexResolution;
    info.depth     = m_voxelTexResolution;
    info.mipmaps   = mipCount;
    return m_pRenderDevice->CreateTextureStorage(info, {.copyUsage = true}, name);
}

void VoxelizerBase::PrepareTextures()
{
    const platform::ConfigLoader& config = platform::ConfigLoader::GetInstance();
    const std::string policy             = config.GetString("voxel_reflectance_policy", "owner");
    uint64_t budgetMiB                   = 0;
    const bool valid = config.ReadNumber("voxel_reflectance_budget_mb", budgetMiB) &&
        budgetMiB <= std::numeric_limits<uint64_t>::max() / (1024 * 1024);
    m_requestAveragedReflectance = policy == "averaged" && valid && budgetMiB != 0;
    m_reflectanceBudgetBytes     = valid ? budgetMiB * 1024 * 1024 : 0;
    if ((policy != "owner" && policy != "averaged") ||
        (policy == "averaged" && !m_requestAveragedReflectance))
    {
        LOGW(
            "Rejected voxel reflectance configuration: averaged requires a positive voxel_reflectance_budget_mb; using owner");
    }
    RHISamplerCreateInfo voxelSampler{};
    voxelSampler.minFilter = RHISamplerFilter::eLinear;
    voxelSampler.magFilter = RHISamplerFilter::eLinear;
    voxelSampler.mipFilter = RHISamplerFilter::eLinear;
    voxelSampler.maxLod    = static_cast<float>(std::bit_width(m_voxelTexResolution) - 1);
    voxelSampler.repeatU   = RHISamplerRepeatMode::eClampToEdge;
    voxelSampler.repeatV   = RHISamplerRepeatMode::eClampToEdge;
    voxelSampler.repeatW   = RHISamplerRepeatMode::eClampToEdge;
    m_pVoxelSampler        = m_pRenderDevice->CreateSampler(voxelSampler);
    m_pColorSampler = m_pRenderDevice->CreateSampler(RHISamplerCreateInfo::CreateLinearRepeat());
    m_voxelTextures.pOwner  = CreateVolume(DataFormat::eR32UInt, "voxel_owner");
    m_voxelTextures.pAlbedo =
        CreateVolume(DataFormat::eR8G8B8A8UNORM, "voxel_albedo",
                     m_classMask == GI_ALL ? std::bit_width(m_voxelTexResolution) : 1);
    if (m_voxelTextures.pAlbedo != nullptr)
    {
        TextureViewFormat view;
        view.dimension = TextureDimension::e3D;
        m_voxelTextures.pAlbedoView =
            m_pRenderDevice->CreateTextureView(m_voxelTextures.pAlbedo, view, "voxel_albedo_base");
    }
}

void VoxelizerBase::PrepareReflectance()
{
    m_useAveragedReflectance = false;
    if (m_requestAveragedReflectance)
    {
        uint64_t peakBytes = 0;
        // This fixed-resolution voxelizer reuses its scratch/output on every rebuild;
        // it does not replace allocations while an earlier generation is in flight.
        const GIResourceStatus status = ValidateVoxelReflectanceResources(
            m_voxelTexResolution, m_pScene->GetVoxelTriangleCount(), m_reflectanceBudgetBytes, 0,
            m_pRenderDevice->GetGPUInfo(), peakBytes);
        if (status == GIResourceStatus::eSuccess)
        {
            if (m_pReflectanceSums == nullptr)
            {
                m_pReflectanceSums = m_pRenderDevice->CreateStorageBuffer(
                    m_voxelCount * 16, nullptr, "voxel_reflectance_sums");
            }
            if (m_pReflectanceSums != nullptr && m_voxelTextures.pReflectance == nullptr)
            {
                m_voxelTextures.pReflectance =
                    CreateVolume(DataFormat::eR8G8B8A8UNORM, "voxel_reflectance");
            }
            m_useAveragedReflectance =
                m_pReflectanceSums != nullptr && m_voxelTextures.pReflectance != nullptr;
        }
        if (!m_useAveragedReflectance)
        {
            LOGW(
                "Averaged voxel reflectance rejected (resource status {}, peak {} bytes); using owner",
                static_cast<uint32_t>(status), peakBytes);
        }
    }
    LOGI("Selected voxel reflectance policy: {}", m_useAveragedReflectance ? "averaged" : "owner");
}

void VoxelizerBase::BindReflectanceSums(RDGPassDescBase& pass) const
{
    if (m_useAveragedReflectance)
    {
        pass.BindStorageBuffer("ReflectanceSums", m_pReflectanceSums);
    }
}

bool VoxelizerBase::EnableRadianceInputs()
{
    if (EnsureReady() && !ProducesRadianceInputs())
    {
        RHITexture* normal   = CreateVolume(DataFormat::eR8G8B8A8UNORM, "voxel_normal_metallic");
        RHITexture* emissive = normal != nullptr ?
            CreateVolume(DataFormat::eR16G16B16A16SFloat, "voxel_emission") :
            nullptr;
        if (normal != nullptr && emissive != nullptr)
        {
            m_voxelTextures.pNormal       = normal;
            m_voxelTextures.pEmissive     = emissive;
            m_voxelTextures.pNormalView   = normal->GetDefaultView();
            m_voxelTextures.pEmissiveView = emissive->GetDefaultView();
            RequestVoxelization();
        }
        else
        {
            m_pRenderDevice->DestroyTexture(normal);
            m_pRenderDevice->DestroyTexture(emissive);
        }
    }
    return IsReady() && ProducesRadianceInputs();
}

void VoxelizerBase::BindVoxelScene(RDGPassDescBase& pass) const
{
    pass.BindStorageBuffer("VertexBuffer", m_pScene->GetVertexBuffer());
    pass.BindStorageBuffer("IndexBuffer", m_pScene->GetIndexBuffer());
    pass.BindStorageBuffer("NodeBuffer", m_pScene->GetNodesDataSSBO());
    pass.BindStorageBuffer("MaterialBuffer", m_pScene->GetMaterialsDataSSBO());
    pass.BindStorageBuffer("TriangleRecords", m_pScene->GetVoxelTriangleBuffer());
    pass.BindValue("uVoxelGrid",
                   VoxelGridUniform{Vec4(GetSceneMinPoint(), GetVoxelSize()),
                                    glm::uvec4(m_classMask, 0, 0, 0)});
    BindSceneTextureArray(pass, m_pColorSampler, m_pScene->GetSceneTextures());
}

void VoxelizerBase::ResolveSurface(RDGQueuePreference queuePreference)
{
    if (m_pScene->GetVoxelTriangleCount() == 0)
    {
        for (RHITexture* texture : {m_voxelTextures.pAlbedo, m_voxelTextures.pNormal,
                                    m_voxelTextures.pEmissive, m_voxelTextures.pReflectance})
        {
            if (texture != nullptr)
            {
                m_pRenderDevice->GetCurrentFrameRDG()
                    ->AddTransferPass("ClearEmptyVoxelSurface")
                    .ClearTexture(texture, Color(0));
            }
        }
    }
    else
    {
        RDGComputePassDesc resolve;
        const char* program = ProducesRadianceInputs() ?
            (m_useAveragedReflectance ? "VoxelResolveSurfaceAveragedSP" : "VoxelResolveSurfaceSP") :
            (m_useAveragedReflectance ? "VoxelResolveAlbedoAveragedSP" : "VoxelResolveAlbedoSP");
        resolve.SetShaderProgramName(program);
        resolve.SetPassTag("ResolveVoxelSurface");
        resolve.SetQueuePreference(queuePreference);
        BindVoxelScene(resolve);
        BindReflectanceSums(resolve);
        if (m_useAveragedReflectance)
        {
            resolve.BindStorageImage("voxelReflectance",
                                     m_voxelTextures.pReflectance->GetDefaultView(),
                                     RDGContentGuarantee::eFullWrite);
        }
        resolve.BindStorageImage("voxelOwner", m_voxelTextures.pOwner->GetDefaultView());
        resolve.BindStorageImage("voxelAlbedo", m_voxelTextures.pAlbedoView,
                                 RDGContentGuarantee::eFullWrite);
        if (ProducesRadianceInputs())
        {
            resolve.BindStorageImage("voxelNormal", m_voxelTextures.pNormalView,
                                     RDGContentGuarantee::eFullWrite);
            resolve.BindStorageImage("voxelEmissive", m_voxelTextures.pEmissiveView,
                                     RDGContentGuarantee::eFullWrite);
        }
        const glm::uvec3 groups =
            GetVoxelVolumeDispatchGroups(m_voxelTexResolution, m_pRenderDevice->GetGPUInfo());
        m_pRenderDevice->GetCurrentFrameRDG()
            ->AddComputePass(std::move(resolve))
            .RecordPassCommands([groups](RDGPassCmdEncoder& encoder) {
                encoder.Dispatch(groups.x, groups.y, groups.z);
            });
    }
}

void VoxelizerBase::SetRenderScene(RenderScene* pScene)
{
    RequestVoxelization();
    m_pScene = pScene;
    m_sceneRevision = 0;
    m_surfaceRevision = 0;
}

void VoxelizerBase::BuildRenderGraph()
{
    BuildVoxelizationGraph();
    BuildVisualizationGraph();
}

sg::AABB VoxelizerBase::GetVoxelBounds() const
{
    const sg::AABB& bounds = m_pScene->GetVoxelSceneBounds();
    const float side       = std::max(bounds.GetMaxExtent(), 0.001f) *
        static_cast<float>(m_voxelTexResolution) / static_cast<float>(m_voxelTexResolution - 2);
    const Vec3 halfSize(side * 0.5f);
    return sg::AABB(bounds.GetCenter() - halfSize, bounds.GetCenter() + halfSize);
}

float VoxelizerBase::GetVoxelSize() const
{
    return GetVoxelBounds().GetMaxExtent() / m_voxelTexResolution;
}

float VoxelizerBase::GetVoxelScale() const
{
    return 1.0f / GetVoxelBounds().GetMaxExtent();
}

Vec3 VoxelizerBase::GetSceneMinPoint() const
{
    return GetVoxelBounds().GetMin();
}

void VoxelizerBase::Destroy()
{
    m_pRenderDevice->DestroyTexture(m_voxelTextures.pOwner);
    m_pRenderDevice->DestroyTexture(m_voxelTextures.pAlbedo);
    m_pRenderDevice->DestroyTexture(m_voxelTextures.pNormal);
    m_pRenderDevice->DestroyTexture(m_voxelTextures.pEmissive);
    m_pRenderDevice->DestroyTexture(m_voxelTextures.pReflectance);
    m_pRenderDevice->DestroyBuffer(m_pReflectanceSums);
    m_pRenderDevice->DestroyBuffer(m_occupiedList);
    m_pRenderDevice->DestroyBuffer(m_gridToList);
    m_pRenderDevice->DestroyBuffer(m_occupiedCount);
    m_occupiedList = m_gridToList = m_occupiedCount = nullptr;
    m_pReflectanceSums       = nullptr;
    m_useAveragedReflectance = false;
    m_voxelTextures = {};
}
} // namespace zen::rc
