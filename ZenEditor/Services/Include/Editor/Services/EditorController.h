#pragma once
#include "Editor/Model/EditorActions.h"
#include "Editor/Model/EditorPreferences.h"
#include "Editor/Model/InspectorNavigation.h"
#include "Editor/Model/SceneLoadState.h"
#include "Editor/Rendering/EditorViewport.h"
#include "Editor/Rendering/MeshPreviewRenderer.h"

namespace zen::editor
{
class SceneLoadWork;

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

    // Parses, prepares GPU data and publishes both, or keeps the current scene and
    // reports the error. Synchronous; frontends normally call RequestLoad instead.
    bool Load(const std::string& path);

    // Queues an interactive load. Rejects empty paths or another request while busy.
    bool RequestLoad(std::string path);

    const SceneLoadState& GetLoadState() const;

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

private:
    void RegisterActions();

    bool PublishScene(UniquePtr<LoadedScene> candidate, std::string& error);

    void FailSceneLoad(std::string error);

    PickStamp MakeStamp() const;

    EditorScene         m_scene;
    EditorSelection     m_selection;
    InspectorNavigation m_inspector;
    EditorCamera        m_camera;
    EditorActions       m_actions;
    EditorPreferences   m_preferences;
    EditorViewport      m_viewport;
    // Declared after the viewport, whose GPU scene it draws from.
    MeshPreviewRenderer    m_meshPreview;
    EditorCamera           m_previewCamera;
    MeshPreviewSettings    m_previewSettings;
    SceneAssetId           m_previewMesh;
    SceneLoadState         m_loadState;
    UniquePtr<LoadedScene> m_loadCandidate;
    // Joins the import worker before its future/result or controller are destroyed.
    UniquePtr<SceneLoadWork>           m_loadWork;
    std::string                        m_error;
    bool                               m_openRequested{false};
    HeapVector<EnvironmentTextureItem> m_environmentTextures;
    std::string                        m_requestedEnvironment;
    std::string                        m_environmentError;
    bool                               m_environmentPending{false};
    bool                               m_environmentOpenRequested{false};
};
} // namespace zen::editor
