#pragma once
#include "Graphics/RenderCore/V2/RenderCoreDefs.h"
#include "Graphics/RenderCore/V2/DynamicVoxelGIPlanning.h"
#include "Graphics/RenderCore/V2/GIVisibilityProvider.h"
#include "Graphics/RenderCore/V2/VoxelGISettings.h"

namespace zen
{
class RHIViewport;
}

namespace zen::rc
{
class RenderDevice;
class SkyboxRenderer;
class DeferredLightingRenderer;
class VoxelizerBase;
class VoxelGIRenderer;
class DynamicVoxelGIRenderer;
class SceneShadowRenderer;
class RenderScene;
class RenderGraph;

enum class RenderOption : uint32_t
{
    eVoxelize = 0,
    ePBR      = 1,
    eVoxelGI  = 2,
    eMax      = 3
};

class RendererServer
{
public:
    RendererServer(RenderDevice* pRenderDevice, RHIViewport* pViewport);

    void Init();

    void Destroy();

    void SetRenderScene(RenderScene* pScene);

    bool DispatchRenderWorkloads();

    DeferredLightingRenderer* RequestDeferredLightingRenderer() const
    {
        return m_pDeferredLightingRenderer;
    }

    SkyboxRenderer* RequestSkyboxRenderer() const
    {
        return m_pSkyboxRenderer;
    }

    VoxelizerBase* RequestVoxelizer(uint32_t classMask = GI_ALL) const
    {
        return classMask == GI_STATIC ? m_pStaticVoxels :
            classMask == GI_DYNAMIC   ? m_pDynamicVoxels :
                                        m_pVoxelizer;
    }

    // Opt-in M2 resources; allocate once after the complete transition preflight.
    bool EnableClassVoxelization(uint64_t budgetBytes);

    const VoxelDDAProvider& GetClassVisibility() const
    {
        return m_classVisibility;
    }

    VoxelGIRenderer* RequestVoxelGI() const
    {
        return m_pVoxelGI;
    }

    void SetRenderOption(RenderOption option)
    {
        m_renderOption      = option;
        m_frameRenderOption = option;
    }

    RenderOption GetRenderOption() const
    {
        return m_frameRenderOption;
    }

    RenderOption GetRequestedRenderOption() const
    {
        return m_renderOption;
    }

    const VoxelGISelection& GetVoxelGISelection() const
    {
        return m_giSelection;
    }

    // Select between retained methods between frame recordings.
    bool SetVoxelGIMethod(VoxelGIMethod method);

    VoxelGIRuntimeSettings GetVoxelGISettings() const;

    // Main/render thread between frames. Invalid inputs leave the current settings intact.
    // Structural changes synchronously retire the old resources, then rebuild lazily.
    // Success accepts settings; GetVoxelGISelection after rendering reports actual fallback.
    // Pointers returned by RequestVoxelizer/RequestVoxelGI/RequestDynamicVoxelGI may change.
    bool ApplyVoxelGISettings(const VoxelGIRuntimeSettings& settings);

    DynamicVoxelGIRenderer* RequestDynamicVoxelGI() const
    {
        return m_pDynamicVoxelGI;
    }

    // Optional 32-byte status/work-count readback, borrowed for the next frame graph.
    // The caller must wait for ordinary resource retirement before mapping/reusing it.
    void SetGIDiagnosticsReadback(RHIBuffer* buffer)
    {
        m_giDiagnosticsReadback = buffer;
    }

    bool HasRecordedGIDiagnostics() const
    {
        return m_recordedGIDiagnostics;
    }

private:
    friend struct VoxelGIRuntimeTestAccess;

    void DestroyVoxelGIResources();

    VoxelizerBase* CreateVoxelizer(RHIViewport* viewport, uint32_t classMask);

    bool BuildClassVoxelization();

    bool PrepareVoxelGI();
    platform::VoxelizerMode m_voxelizerMode{platform::VoxelizerMode::eCompute};
    VoxelizerBase* m_pStaticVoxels{nullptr};
    VoxelizerBase* m_pDynamicVoxels{nullptr};
    VoxelDDAProvider m_classVisibility;
    RHIViewport* m_pViewport{nullptr};
    RenderDevice* m_pRenderDevice{nullptr};
    RenderScene* m_pScene{nullptr};

    DeferredLightingRenderer* m_pDeferredLightingRenderer{nullptr};
    SkyboxRenderer* m_pSkyboxRenderer{nullptr};
    VoxelizerBase* m_pVoxelizer{nullptr};
    VoxelGIRenderer* m_pVoxelGI{nullptr};
    DynamicVoxelGIRenderer* m_pDynamicVoxelGI{nullptr};
    SceneShadowRenderer* m_pSceneShadows{nullptr};

    RenderOption m_renderOption{RenderOption::eVoxelize};
    RenderOption m_frameRenderOption{RenderOption::eVoxelize};
    VoxelGISelection m_giSelection;
    VoxelGIRuntimeSettings m_giSettings;
    RHIBuffer* m_giDiagnosticsReadback{nullptr};
    bool m_recordedGIDiagnostics{false};
};
} // namespace zen::rc
