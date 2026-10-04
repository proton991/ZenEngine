#pragma once
#include "Editor/Model/EditorSelection.h"
#include "Editor/Model/EditorEnvironment.h"
#include "Graphics/RenderCore/V2/RenderScene.h"
#include "Graphics/RenderCore/V2/RenderView.h"
#include "Graphics/RenderCore/V2/Renderer/RendererServer.h"

namespace zen::editor
{
struct EditorRenderSnapshot
{
    RHIGPUMemoryStats          memory;
    rc::VoxelGIRuntimeSettings settings;
    rc::RenderOption           requestedMode{rc::RenderOption::ePBR};
    rc::RenderOption           renderedMode{rc::RenderOption::ePBR};
};

// A completed GPU surface pick. The stamp is the one supplied with the request; the
// caller decides whether the result still applies.
struct PickResult
{
    NodeId    node;
    PickStamp stamp;
};

// GPU publication of the open scene, its offscreen image, selection overlay, surface
// picking and asset previews. Reads the model and never changes it.
class EditorViewport
{
public:
    EditorViewport(rc::RenderDevice& device, const EditorScene& scene, const EditorSelection& selection, EditorCamera& camera);

    // The application must initialize the device's RendererServer before this service.
    bool Init();

    void Destroy();

    // Scene replacement is a transaction run by the caller: prepare GPU data for a parsed
    // candidate, commit it immediately before the model replaces its scene, then refresh
    // the per-scene resources. Rendering the current scene between prepare and commit is
    // allowed. A failed preparation/commit leaves it active; discard the pending scene
    // before releasing the candidate's CPU data.
    bool PrepareScene(const LoadedScene& candidate, std::string& error);

    bool CommitScene(std::string& error);

    void DiscardScene();

    // Rebuilds previews and cancels picks for the model's current scene.
    void RefreshSceneResources();

    bool Resize(uint32_t width, uint32_t height);

    // Borrowed offscreen scene targets, independent of the native presentation window.
    const rc::RenderView& GetRenderView() const;

    bool HasScene() const;

    // The open scene's GPU data, or null when it has no renderable geometry.
    const rc::RenderScene* GetRenderScene() const;

    EditorRenderSnapshot GetSnapshot() const;

    void SetRenderMode(rc::RenderOption mode);

    const EditorEnvironment& GetEnvironment() const;

    bool SetEnvironmentTexture(const std::string& path, std::string& error);

    bool SetEnvironmentLighting(float intensity, float rotationDegrees, bool lighting, bool skybox);

    RHISampler* GetImageSampler() const;

    uint64_t GetTargetRevision() const;

    void RequestPick(Vec2 normalized, const PickStamp& stamp);

    // Returns a result once its readback completes; never waits for the GPU.
    bool TakePickResult(PickResult& result);

    void BuildOverlays(rc::RenderGraph& graph);

    void OnSubmitted(bool succeeded);

    bool IsPickPending() const;

    // Encoded RGBA8 thumbnail of a listed texture, created when its scene opens.
    // Null for stale IDs and for formats without a CPU preview.
    RHITexture* GetAssetPreview(SceneAssetId id) const;

private:
    void BuildPick(rc::RenderGraph& graph);

    void BuildBounds(rc::RenderGraph& graph);

    void ReleasePreviews();

    void CreatePreviews();

    rc::RenderDevice&          m_device;
    const EditorScene&         m_editorScene;
    const EditorSelection&     m_selection;
    EditorCamera&              m_camera;
    UniquePtr<rc::RenderScene> m_scene;
    UniquePtr<rc::RenderScene> m_pendingScene;
    bool                       m_pendingPrepared{false};
    rc::RenderView             m_view;
    EditorEnvironment          m_environment;
    uint64_t                   m_targetRevision{0};
    HeapVector<RHIBuffer*>     m_boundsBuffers;
    RHISampler*                m_pickSampler{nullptr};
    RHISampler*                m_materialSampler{nullptr};
    RHISampler*                m_imageSampler{nullptr};
    RHIBuffer*                 m_pickOutput{nullptr};
    RHIBuffer*                 m_pickReadback{nullptr};
    bool                       m_pickRequested{false};
    bool                       m_pickRecorded{false};
    bool                       m_pickInFlight{false};
    Vec2                       m_pickPosition{0.0f};
    PickStamp                  m_requestedStamp;
    PickStamp                  m_recordedStamp;
    rc::ResourceRetirement     m_pickRetirement;
    HeapVector<NodeId>         m_pickNodes;
    HeapVector<RHITexture*>    m_previews;
    uint64_t                   m_previewGeneration{0};
};
} // namespace zen::editor
