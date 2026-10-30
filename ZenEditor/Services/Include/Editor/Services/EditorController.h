#pragma once
#include "AssetTools/SceneTools.h"
#include "Editor/Model/EditorActions.h"
#include "Editor/Model/EditorPreferences.h"
#include "Editor/Model/InspectorNavigation.h"
#include "Editor/Model/SceneLoadState.h"
#include "Editor/Model/SceneProcessingState.h"
#include "Editor/Model/EditorRenderingState.h"
#include "Editor/Rendering/EditorViewport.h"
#include "Editor/Rendering/MeshPreviewRenderer.h"
#include "ZenApp/RuntimeWindow.h"

namespace zen::editor
{
// Toolkit-independent coordinator above the model and render services. It owns the
// editor state, runs transactions that span both layers (scene replacement, picking,
// framing), and registers the core actions. Frontends draw this state and invoke its
// actions; a replacement frontend reuses it unchanged.
class EditorController
{
public:
    explicit EditorController(rc::RenderDevice& device);

    ~EditorController();

    EditorController(const EditorController&)            = delete;

    EditorController& operator=(const EditorController&) = delete;

    // The device's RendererServer must be initialized first.
    bool Init();

    void Destroy();

    const EditorScene& GetScene() const;

    EditorSelection& GetSelection();

    const EditorSelection& GetSelection() const;

    // Accessors do not synchronize. The frontend updates navigation before drawing;
    // successful scene replacement synchronizes it as part of the load transaction.
    InspectorNavigation& GetInspector();

    const InspectorNavigation& GetInspector() const;

    EditorCamera& GetCamera();

    EditorActions& GetActions();

    EditorPreferences& GetPreferences();

    EditorViewport& GetViewport();

    const EditorRenderingState& GetRenderingState() const;

    // Edits preview at the next update with Apply-only resource values held at their applied
    // state. applyResources publishes those values too and retries latched GPU failures.
    bool StageRenderingSettings(const rc::RenderingSettings& settings, bool applyResources = false);

    bool UpdateRenderingSettings();

    void RevertRenderingSettings();

    bool AddRenderingLight(rc::SceneLightType type);

    bool AddBoundsLights(bool corners);

    void ResetRenderingLights();

    const rc::RenderingSettings& GetRenderingDefaults() const;

    void ResetRenderingSetup();

    // Returns whether the application loop may continue. Recoverable errors remain visible in the renderer status.
    bool RecordRenderingResult(rc::RenderFrameResult result, bool drawScene = true);

    bool CanRestoreRenderingSettings() const;

    void RestoreRenderingSettings();

    // Parses, prepares GPU data and publishes both, or keeps the current scene and
    // reports the error. Synchronous; frontends normally call RequestLoad instead.
    bool Load(const std::string& path);
    bool Load(const asset::SceneImportRequest& request);

    // Queues an interactive load. Rejects empty paths or another request while busy.
    bool                        RequestLoad(std::string path);
    bool                        RequestLoad(asset::SceneImportRequest request);
    bool                        CanRecook() const;
    bool                        RequestRecook();
    bool                        StageSourceProcessing(const SceneProcessingSettings& settings);
    const SceneProcessingState& GetSourceProcessing() const;
    bool                        CancelSceneLoad();

    const SceneLoadState&                           GetLoadState() const;
    const HeapVector<asset::SceneImportDiagnostic>& GetImportDiagnostics() const;
    const HeapVector<asset::SceneImportDiagnostic>& GetLastLoadDiagnostics() const;

    // Advances at most one stage before drawing each frame. CPU import runs on a
    // worker; GPU preparation/publication stay here on the engine thread. Present
    // a frame between calls so each stage is visible before its work begins.
    // Returns whether the stage changed (also useful for resetting frame timing).
    bool UpdateSceneLoad();

    void DismissLoadError();

    const std::string& GetError() const;

    // Raised by the Open action. The application shows its platform picker, or the
    // frontend asks for a path when the platform has none.
    bool TakeFileOpenRequest();

    const HeapVector<EnvironmentTextureItem>& GetEnvironmentTextures() const;

    void RefreshEnvironmentTextures();

    bool RequestEnvironmentTexture(std::string path);

    // Called between frames, never while a scene replacement is in progress.
    bool UpdateEnvironment();

    bool IsEnvironmentPending() const;

    const std::string& GetEnvironmentError() const;

    void RequestEnvironmentFileOpen();

    bool TakeEnvironmentFileOpenRequest();

    // Applies completed GPU picks that still match the current scene, camera, target
    // and selection.
    void ProcessPicks();

    // Immediate bounds selection, refined later by the GPU surface pick.
    void Pick(Vec2 normalized);

    bool ResizeViewport(uint32_t width, uint32_t height);

    void FrameAll();

    void FrameSelection();

    MeshPreviewRenderer& GetMeshPreview();

    MeshPreviewSettings& GetMeshPreviewSettings();

    // Orbits the previewed mesh in its own space, independent of the scene camera.
    EditorCamera& GetMeshPreviewCamera();

    // Renders a mesh asset into the preview image this frame, at about this size in
    // pixels. A different mesh is framed first. Returns false when there is nothing
    // to draw, for example a mesh of a scene without GPU geometry.
    bool PreviewMesh(SceneAssetId mesh, uint32_t width, uint32_t height);

    // Frames the previewed mesh again.
    void ResetMeshPreview();

    runtime::RuntimeWindow&         GetPlaySession();
    runtime::RuntimeWindowSettings& GetRunSettings();
    bool                            CanRun() const;
    bool                            StartOrResumeRuntime();
    void                            UpdatePlaySession();
    bool                            IsPreviewFrozen() const;

private:
    void RegisterActions();

    bool PublishScene(UniquePtr<LoadedScene> candidate, std::string& error);

    void                      FailSceneLoad(std::string error);
    asset::SceneImportRequest MakeLoadRequest(std::string path) const;
    void                      CommitSourceProcessing(const asset::SceneImportRequest& request, bool cacheHit);

    PickStamp MakeStamp() const;

    rc::RenderDevice&              m_device;
    runtime::RuntimeWindowSettings m_runSettings;
    EditorScene                    m_scene;
    EditorSelection                m_selection;
    InspectorNavigation            m_inspector;
    EditorCamera                   m_camera;
    EditorActions                  m_actions;
    EditorPreferences              m_preferences;
    EditorViewport                 m_viewport;
    runtime::RuntimeWindow         m_playSession;
    EditorRenderingState           m_rendering;
    rc::RenderingSettings          m_renderDefaults;
    // engine.cfg's environment lighting, before a scene's imported lights replace it.
    bool                           m_configuredEnvironmentLighting{true};
    rc::RenderingSettings          m_lastSuccessfulRendering;
    bool                           m_hasSuccessfulRendering{false};
    uint32_t                       m_consecutiveDeclines{0};
    uint64_t                       m_successfulRenderingRevision{0};
    bool                           m_renderApplyRequested{false};
    bool                           m_renderResourcesRequested{false};
    HeapVector<rc::RenderingLight> m_importedLights;
    uint64_t                       m_nextLightId{1};
    // Declared after the viewport, whose GPU scene it draws from.
    MeshPreviewRenderer       m_meshPreview;
    EditorCamera              m_previewCamera;
    MeshPreviewSettings       m_previewSettings;
    SceneAssetId              m_previewMesh;
    SceneLoadState            m_loadState;
    SceneProcessingState      m_sourceProcessing;
    asset::SceneImportRequest m_queuedLoadRequest;
    asset::SceneImportRequest m_appliedLoadRequest;
    bool                      m_pendingCacheHit{false};
    UniquePtr<LoadedScene>    m_loadCandidate;
    // Joins the import worker before its future/result or controller are destroyed.
    std::future<asset::SceneLoadResult>                      m_loadResult;
    SharedPtr<asset::SceneImportControl, MultiThreadCounter> m_loadControl;
    // Messages of the published scene, and of the open still in progress.
    HeapVector<asset::SceneImportDiagnostic> m_importDiagnostics;
    HeapVector<asset::SceneImportDiagnostic> m_pendingDiagnostics;
    UniquePtr<asset::SourceSceneLoader>      m_sceneLoader;
    std::string                              m_error;
    bool                                     m_openRequested{false};
    HeapVector<EnvironmentTextureItem>       m_environmentTextures;
    std::string                              m_requestedEnvironment;
    std::string                              m_environmentError;
    bool                                     m_environmentPending{false};
    bool                                     m_environmentOpenRequested{false};
};
} // namespace zen::editor
