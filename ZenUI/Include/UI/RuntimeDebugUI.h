#pragma once

#include "UI/UIContext.h"
#include "UI/UIRenderer.h"
#include "Graphics/RenderCore/V2/Renderer/RenderOverlay.h"
#include "Graphics/RenderCore/V2/VoxelGISettings.h"
#include "UI/RuntimeSceneControls.h"

namespace zen::platform
{
class GlfwWindowImpl;
}

namespace zen::ui
{
// Session-only runtime controls. Editor selection/documents/undo do not live here.
class RuntimeDebugUI : public rc::RenderOverlay
{
public:
    RuntimeDebugUI(rc::RenderDevice& device,
                   platform::GlfwWindowImpl& window,
                   RuntimeSceneControls& sceneControls);

    ~RuntimeDebugUI() override;

    bool Init();

    void Update(float deltaSeconds, RHIViewport& viewport);

    bool BuildRenderGraph(rc::RenderGraph& graph, RHIViewport& viewport) override;

private:
    friend struct RuntimeDebugUITestAccess;

    void SetVisible(bool visible);

    void BuildPanel();

    void BuildSettings();

    void BuildSceneSettings();

    void BuildConfigReference();

    void MarkGIEdit(bool changed);

    void EnsureReflectanceBudget();

    void MarkSceneEdit(bool changed);

    void ApplyPendingSettings(bool manual);

    void ReloadSettings();

    rc::RenderDevice& m_device;
    platform::GlfwWindowImpl& m_window;
    RuntimeSceneControls& m_sceneControls;
    UIContext m_context;
    UIRenderer m_renderer;
    rc::VoxelGIRuntimeSettings m_draft;
    RuntimeSceneSettings m_sceneDraft;
    RuntimeSceneSettings m_sceneBaseline;
    bool m_initialized{false};
    bool m_visible{true};
    bool m_dirty{false};
    bool m_sceneDirty{false};
    bool m_autoApply{true};
    bool m_applyFailed{false};
    char m_configFilter[128]{};
    const char* m_status{"Settings apply to this session only."};
};
} // namespace zen::ui
