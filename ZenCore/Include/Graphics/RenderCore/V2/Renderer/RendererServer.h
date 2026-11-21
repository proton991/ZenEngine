#pragma once
#include "Graphics/RenderCore/V2/RenderView.h"
#include "Graphics/RenderCore/V2/RenderCoreDefs.h"
#include "Graphics/RenderCore/V2/VoxelGISettings.h"
#include "Graphics/RenderCore/V2/VoxelResourcePlanning.h"
#include "Graphics/RenderCore/V2/RenderingSettings.h"
#include "Graphics/RenderCore/V2/Renderer/DebugVisualization.h"
#include "Graphics/RenderCore/V2/Renderer/SceneCulling.h"

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
struct RDGDebugSnapshot;
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
    RenderAlgorithm    requested{RenderAlgorithm::ePBR};
    RenderAlgorithm    effective{RenderAlgorithm::ePBR};
    std::string        fallbackReason;
    SceneCullingResult gpuCulling;
};

class RendererServer
{
public:
    RendererServer(RenderDevice* pRenderDevice, RHIViewport* presentationViewport);

    void Init(const RenderingSettings* initialSettings = nullptr);

    void Destroy();

    void SetRenderScene(RenderScene* pScene);

    // Scene targets are explicit. Presentation and UI use the native viewport separately.
    // drawScene=false keeps UI alive while the scene panel is hidden.
    bool PrecacheSceneView(const RenderView& view, RenderOverlay* overlay, RHIViewport* presentation);

    RenderFrameResult DispatchRenderWorkloads(const RenderView& view,
                                              RenderOverlay*    overlay      = nullptr,
                                              bool              drawScene    = true,
                                              RHIViewport*      presentation = nullptr,
                                              RDGDebugSnapshot* capture      = nullptr);

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
    // How the GI tier was chosen; "hardware_failed_compute_fallback" once an automatic HW tier moved to Compute.
    const char* GetGISelectionReason() const;
    // The persistent HW failure behind an automatic Compute fallback, or nullptr.
    const char* GetGITierFallbackReason() const;

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

    RenderOption GetRenderOption() const
    {
        return m_frameRenderOption;
    }

    RenderOption GetRequestedRenderOption() const
    {
        return m_renderOption;
    }

    // Compute uses PBR plus environment lighting until the scene's distance field, built on the GPU in
    // bounded per-frame work, is ready (VoxelGIRenderer::IsPreparingField).
    bool IsPreparingGI() const;

    // The requested GI renders, but its distance field is still being completed
    // (VoxelGIRenderer::IsRefiningField). Measurements of converged GI wait for it.
    bool IsRefiningGI() const;

    // The applied configuration as explicit settings (optimal false), with an automatic provider reported
    // as Auto. A caller can edit and apply them, and the edits take effect.
    VoxelGIRuntimeSettings GetVoxelGISettings() const;

    // The settings as requested: the optimal configuration stays unresolved, so an unchanged request
    // compares equal. The editor's draft starts from these.
    VoxelGIRuntimeSettings GetRequestedVoxelGISettings() const;

    // The settings the renderer applies, with the optimal configuration and the provider resolved.
    VoxelGIRuntimeSettings GetEffectiveVoxelGISettings() const;

    // Read-only preflight: no waits, settings changes or resource allocation.
    GIResourceStatus ValidateVoxelGIResources(const VoxelGIRuntimeSettings& settings, uint64_t& reflectanceBytes) const;

    // Main/render thread between frames. Invalid inputs leave the current settings intact.
    // Structural changes synchronously retire the old resources, then rebuild lazily.
    // Pointers returned by RequestVoxelizer/RequestVoxelGI may change. A latched fallback
    // clears only when resources change or on an explicit retry, which also rebuilds a
    // voxelizer whose allocation failed; lighting-only edits keep it.
    bool ApplyVoxelGISettings(const VoxelGIRuntimeSettings& settings, bool retryFailedResources = false);

    // Preflight the proposed selection; active hardware shadows do not require map memory.
    bool ValidateShadowResources(const RenderingSettings& settings, uint32_t faces) const;

private:
    RenderFrameResult PrepareOrDispatchView(const RenderView& view,
                                            RenderOverlay*    overlay,
                                            bool              drawScene,
                                            RHIViewport*      presentation,
                                            RDGDebugSnapshot* capture,
                                            bool              precache);
    void              BuildDiagnosticFrame(const RenderView& view);

    void DestroyVoxelGIResources();
    void CreateVoxelGIResources();

    RHIViewport*  m_pPresentationViewport{nullptr};
    RenderDevice* m_pRenderDevice{nullptr};
    RenderScene*  m_pScene{nullptr};

    DeferredLightingRenderer* m_pDeferredLightingRenderer{nullptr};
    SkyboxRenderer*           m_pSkyboxRenderer{nullptr};
    VoxelizerBase*            m_pVoxelizer{nullptr};
    VoxelGIRenderer*          m_pVoxelGI{nullptr};
    SceneShadowRenderer*      m_pSceneShadows{nullptr};

    RenderOption           m_renderOption{RenderOption::eVoxelize};
    RenderOption           m_frameRenderOption{RenderOption::eVoxelize};
    VoxelGIRuntimeSettings m_giSettings;
    VoxelGIRuntimeSettings m_giRequested; // Before ResolveVoxelGIAutomaticProvider.
    RenderingStatus        m_status;
    DebugSelection         m_debug{DebugOutput::eVoxels};
    DebugOutputDescription m_debugDescription;
    const char*            m_giSelectionReason{"not_selected"};
    bool                   m_giAutomaticTier{false}; // Keep the requested Auto policy across live edits.
    bool                   m_frameShadows{false};
    bool                   m_frameGI{false};
    bool                   m_frameVisibility{false}; // BuildRayQueryGraph ran this frame.
};
} // namespace zen::rc
