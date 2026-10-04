#pragma once
#include "Graphics/RenderCore/V2/RenderView.h"
#include "Graphics/RenderCore/V2/RenderCoreDefs.h"
#include "Graphics/RenderCore/V2/VoxelGISettings.h"
#include "Graphics/RenderCore/V2/VoxelResourcePlanning.h"
#include "Graphics/RenderCore/V2/RenderingSettings.h"
#include "Graphics/RenderCore/V2/Renderer/DebugVisualization.h"

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
class SceneShadowRenderer;
class RenderScene;
class RenderGraph;
class RenderOverlay;

enum class RenderOption : uint32_t
{
    eVoxelize = 0,
    ePBR      = 1,
    eVoxelGI  = 2,
    eMax      = 3
};

struct RenderingStatus
{
    RenderAlgorithm requested{RenderAlgorithm::ePBR};
    RenderAlgorithm effective{RenderAlgorithm::ePBR};
    std::string     fallbackReason;
};

class RendererServer
{
public:
    RendererServer(RenderDevice* pRenderDevice, RHIViewport* presentationViewport);

    void Init();

    void Destroy();

    void SetRenderScene(RenderScene* pScene);

    // Scene targets are explicit. Presentation and UI use the native viewport separately.
    // drawScene=false keeps UI alive while the scene panel is hidden.
    bool DispatchRenderWorkloads(const RenderView& view, RenderOverlay* overlay = nullptr, bool drawScene = true);

    DeferredLightingRenderer* RequestDeferredLightingRenderer() const
    {
        return m_pDeferredLightingRenderer;
    }

    SkyboxRenderer* RequestSkyboxRenderer() const
    {
        return m_pSkyboxRenderer;
    }

    VoxelizerBase* RequestVoxelizer() const
    {
        return m_pVoxelizer;
    }

    VoxelGIRenderer* RequestVoxelGI() const
    {
        return m_pVoxelGI;
    }

    void SetRenderOption(RenderOption option)
    {
        if (m_renderOption != option)
        {
            ResetRenderingFailure();
        }

        m_renderOption      = option;
        m_frameRenderOption = option;
        m_debug.output      = option == RenderOption::eVoxelize ? DebugOutput::eVoxels : DebugOutput::eFinal;
    }

    void SetRenderingSelection(RenderAlgorithm algorithm, const DebugSelection& selection);

    const DebugOutputDescription& GetDebugOutputDescription() const
    {
        return m_debugDescription;
    }

    const DebugSelection& GetDebugSelection() const
    {
        return m_debug;
    }

    const RenderingStatus& GetRenderingStatus() const
    {
        return m_status;
    }

    void ResetRenderingFailure();

    platform::VoxelizerMode GetEffectiveVoxelizer() const
    {
        return m_voxelizerMode;
    }

    RenderOption GetRenderOption() const
    {
        return m_frameRenderOption;
    }

    RenderOption GetRequestedRenderOption() const
    {
        return m_renderOption;
    }

    VoxelGIRuntimeSettings GetVoxelGISettings() const;

    // Read-only preflight: no waits, settings changes or resource allocation.
    GIResourceStatus ValidateVoxelGIResources(const VoxelGIRuntimeSettings& settings, uint64_t& reflectanceBytes) const;

    // Main/render thread between frames. Invalid inputs leave the current settings intact.
    // Structural changes synchronously retire the old resources, then rebuild lazily.
    // Pointers returned by RequestVoxelizer/RequestVoxelGI may change. A latched fallback
    // clears only when resources change or on an explicit retry, which also rebuilds a
    // voxelizer whose allocation failed; cone-only edits keep it.
    bool ApplyVoxelGISettings(const VoxelGIRuntimeSettings& settings, bool retryFailedResources = false);

    bool ValidateShadowResources(uint32_t resolution, uint32_t faces) const;

private:
    void BuildDiagnosticFrame(const RenderView& view);

    void DestroyVoxelGIResources();

    VoxelizerBase* CreateVoxelizer();

    platform::VoxelizerMode m_voxelizerMode{platform::VoxelizerMode::eCompute};
    RHIViewport*            m_pPresentationViewport{nullptr};
    RenderDevice*           m_pRenderDevice{nullptr};
    RenderScene*            m_pScene{nullptr};

    DeferredLightingRenderer* m_pDeferredLightingRenderer{nullptr};
    SkyboxRenderer*           m_pSkyboxRenderer{nullptr};
    VoxelizerBase*            m_pVoxelizer{nullptr};
    VoxelGIRenderer*          m_pVoxelGI{nullptr};
    SceneShadowRenderer*      m_pSceneShadows{nullptr};

    RenderOption           m_renderOption{RenderOption::eVoxelize};
    RenderOption           m_frameRenderOption{RenderOption::eVoxelize};
    VoxelGIRuntimeSettings m_giSettings;
    RenderingStatus        m_status;
    DebugSelection         m_debug{DebugOutput::eVoxels};
    DebugOutputDescription m_debugDescription;
    bool                   m_frameShadows{false};
    bool                   m_frameGI{false};
};
} // namespace zen::rc
