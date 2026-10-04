#include "Editor/Services/EditorController.h"
#include "Utils/Errors.h"
#include "Utils/ThreadPool.h"
#include <chrono>

namespace zen::editor
{
namespace
{
constexpr uint16_t kControl = uint16_t(platform::KeyModifier::Control);

bool Unavailable()
{
    return false;
}

void Ignore() {}

// Commands shown now and implemented by later plan steps.
struct Placeholder
{
    const char*    id;
    const char*    label;
    EditorShortcut shortcut;
    const char*    reason;
};
} // namespace

// The worker owns only an unpublished CPU scene. It never touches the controller,
// active scene, window or GPU. ThreadPool teardown joins before the future dies.
struct SceneReadResult
{
    UniquePtr<LoadedScene> scene;
    std::string            error;
};

namespace
{
SceneReadResult ReadScene(std::string path)
{
    SceneReadResult result;

    // Import failures are reported through the error string, never exceptions.
    result.scene = ParseScene(path, result.error);

    return result;
}
} // namespace

class SceneLoadWork
{
public:
    explicit SceneLoadWork(const std::string& path) : m_worker(1)
    {
        m_result = m_worker.Push(&ReadScene, path);
    }

    bool IsReady() const
    {
        return m_result.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
    }

    SceneReadResult TakeResult()
    {
        return m_result.get();
    }

private:
    std::future<SceneReadResult> m_result;
    ThreadPool<void, uint32_t>   m_worker;
};

EditorController::EditorController(rc::RenderDevice& device) :
    m_selection(m_scene),
    m_inspector(m_scene, m_selection),
    m_viewport(device, m_scene, m_selection, m_camera),
    m_meshPreview(device, m_viewport)
{}

EditorController::~EditorController() = default;

bool EditorController::Init()
{
    const bool valid = m_viewport.Init() && m_meshPreview.Init();

    if (valid)
    {
        RegisterActions();

        RefreshEnvironmentTextures();
    }

    return valid;
}

void EditorController::Destroy()
{
    m_loadWork.Reset();

    m_meshPreview.Destroy();

    m_viewport.Destroy();

    m_loadCandidate.Reset();

    m_loadState = {};
}

void EditorController::RegisterActions()
{
    m_inspector.RegisterActions(m_actions);

    m_actions.Register({actions::Open,
                        "Open...",
                        {platform::Key::O, kControl},
                        "Wait for the current scene to finish opening.",
                        [this]() { return !m_loadState.IsActive() && !m_environmentPending; },
                        [this]() {
                            m_openRequested = true;
                        }});

    m_actions.Register({actions::FrameAll,
                        "Frame All",
                        {platform::Key::Home, 0},
                        "Open a scene first.",
                        [this]() { return m_scene.Get() != nullptr; },
                        [this]() {
                            FrameAll();
                        }});

    m_actions.Register({actions::FrameSelection,
                        "Frame Selected",
                        {platform::Key::F, 0},
                        "Select a node first.",
                        [this]() { return m_selection.GetNode().generation != 0; },
                        [this]() {
                            FrameSelection();
                        }});

    const Placeholder placeholders[] = {
        {actions::Save, "Save", {platform::Key::S, kControl}, "Available with scene documents (plan step 5)."},
        {actions::Undo, "Undo", {platform::Key::Z, kControl}, "Available with undoable editing (plan step 6)."},
        {actions::Redo, "Redo", {platform::Key::Y, kControl}, "Available with undoable editing (plan step 6)."},
        {actions::Move, "Move", {}, "Available with transform gizmos (plan step 6)."},
        {actions::Rotate, "Rotate", {}, "Available with transform gizmos (plan step 6)."},
        {actions::Scale, "Scale", {}, "Available with transform gizmos (plan step 6)."},
        {actions::Play, "Play", {}, "Available with runtime preview (plan step 8)."},
        {actions::Pause, "Pause", {}, "Available with runtime preview (plan step 8)."},
        {actions::Stop, "Stop", {}, "Available with runtime preview (plan step 8)."}};

    for (const Placeholder& placeholder : placeholders)
    {
        m_actions.Register(
            {placeholder.id, placeholder.label, placeholder.shortcut, placeholder.reason, &Unavailable, &Ignore});
    }
}

const EditorScene& EditorController::GetScene() const
{
    return m_scene;
}

EditorSelection& EditorController::GetSelection()
{
    return m_selection;
}

const EditorSelection& EditorController::GetSelection() const
{
    return m_selection;
}

InspectorNavigation& EditorController::GetInspector()
{
    return m_inspector;
}

const InspectorNavigation& EditorController::GetInspector() const
{
    return m_inspector;
}

EditorCamera& EditorController::GetCamera()
{
    return m_camera;
}

EditorActions& EditorController::GetActions()
{
    return m_actions;
}

EditorPreferences& EditorController::GetPreferences()
{
    return m_preferences;
}

EditorViewport& EditorController::GetViewport()
{
    return m_viewport;
}

bool EditorController::PublishScene(UniquePtr<LoadedScene> candidate, std::string& error)
{
    // Commit drains old-scene work before the CPU scene it references is replaced.
    const bool valid = m_viewport.CommitScene(error);

    if (valid)
    {
        const std::string path = candidate->path;

        m_meshPreview.Cancel();

        m_scene.Replace(std::move(candidate));

        m_viewport.RefreshSceneResources();

        m_selection.Clear();

        m_inspector.Synchronize();

        m_preferences.recentFiles.Add(path);

        m_error.clear();

        FrameAll();

        LOGI("Editor opened {}", path);
    }
    else
    {
        // Pending GPU data borrows candidate's CPU data, which dies on return.
        m_viewport.DiscardScene();
    }

    return valid;
}

bool EditorController::Load(const std::string& path)
{
    bool valid = false;

    if (!m_loadState.IsActive() && !m_environmentPending)
    {
        std::string error;

        UniquePtr<LoadedScene> candidate = ParseScene(path, error);

        valid = candidate && m_viewport.PrepareScene(*candidate, error) && PublishScene(std::move(candidate), error);

        if (valid)
        {
            m_loadState = {};
        }
        else
        {
            m_error = error;

            LOGE("Editor open failed: {}", m_error);
        }
    }

    return valid;
}

bool EditorController::RequestLoad(std::string path)
{
    const bool accepted = !path.empty() && !m_loadState.IsActive() && !m_environmentPending;

    if (accepted)
    {
        m_loadState = {SceneLoadStage::Queued, std::move(path)};

        m_error.clear();
    }

    return accepted;
}

const HeapVector<EnvironmentTextureItem>& EditorController::GetEnvironmentTextures() const
{
    return m_environmentTextures;
}

void EditorController::RefreshEnvironmentTextures()
{
    m_environmentTextures = DiscoverEnvironmentTextures(ZEN_TEXTURE_PATH);
}

bool EditorController::RequestEnvironmentTexture(std::string path)
{
    const bool accepted = !m_loadState.IsActive() && !m_environmentPending && (m_viewport.HasScene() || path.empty());

    if (accepted)
    {
        m_requestedEnvironment = std::move(path);

        m_environmentError.clear();

        m_environmentPending = true;
    }

    return accepted;
}

bool EditorController::UpdateEnvironment()
{
    const bool changed = m_environmentPending && !m_loadState.IsActive();

    if (changed)
    {
        if (!m_viewport.SetEnvironmentTexture(m_requestedEnvironment, m_environmentError))
        {
            LOGW("Environment switch failed: {}", m_environmentError);
        }

        m_environmentPending = false;

        m_requestedEnvironment.clear();
    }

    return changed;
}

bool EditorController::IsEnvironmentPending() const
{
    return m_environmentPending;
}

const std::string& EditorController::GetEnvironmentError() const
{
    return m_environmentError;
}

void EditorController::RequestEnvironmentFileOpen()
{
    m_environmentOpenRequested = true;
}

bool EditorController::TakeEnvironmentFileOpenRequest()
{
    const bool requested       = m_environmentOpenRequested;

    m_environmentOpenRequested = false;

    return requested;
}

const SceneLoadState& EditorController::GetLoadState() const
{
    return m_loadState;
}

void EditorController::FailSceneLoad(std::string error)
{
    m_loadWork.Reset();

    // Destroy unpublished GPU data before the CPU scene it references.
    m_viewport.DiscardScene();

    m_loadCandidate.Reset();

    m_error           = std::move(error);

    m_loadState.stage = SceneLoadStage::Failed;

    LOGE("Editor open failed: {}", m_error);
}

bool EditorController::UpdateSceneLoad()
{
    const SceneLoadStage previous = m_loadState.stage;

    switch (previous)
    {
        case SceneLoadStage::Queued:
        {
            m_loadWork        = MakeUnique<SceneLoadWork>(m_loadState.path);

            m_loadState.stage = SceneLoadStage::Reading;

            break;
        }
        case SceneLoadStage::Reading:
        {
            if (m_loadWork->IsReady())
            {
                SceneReadResult result = m_loadWork->TakeResult();

                m_loadWork.Reset();

                if (result.scene)
                {
                    m_loadCandidate   = std::move(result.scene);

                    m_loadState.stage = SceneLoadStage::Preparing;
                }
                else
                {
                    FailSceneLoad(std::move(result.error));
                }
            }

            break;
        }
        case SceneLoadStage::Preparing:
        {
            std::string error;

            if (m_viewport.PrepareScene(*m_loadCandidate, error))
            {
                m_loadState.stage = SceneLoadStage::Publishing;
            }
            else
            {
                FailSceneLoad(std::move(error));
            }

            break;
        }
        case SceneLoadStage::Publishing:
        {
            std::string error;

            if (PublishScene(std::move(m_loadCandidate), error))
            {
                m_loadState.stage = SceneLoadStage::Complete;
            }
            else
            {
                FailSceneLoad(std::move(error));
            }

            break;
        }
        case SceneLoadStage::Complete: m_loadState = {}; break;
        case SceneLoadStage::Idle:
        case SceneLoadStage::Failed: break;
    }

    return previous != m_loadState.stage;
}

void EditorController::DismissLoadError()
{
    if (m_loadState.stage == SceneLoadStage::Failed)
    {
        m_loadState = {};
    }
}

const std::string& EditorController::GetError() const
{
    return m_error;
}

bool EditorController::TakeFileOpenRequest()
{
    const bool requested = m_openRequested;

    m_openRequested      = false;

    return requested;
}

PickStamp EditorController::MakeStamp() const
{
    return MakePickStamp(m_scene, m_camera, m_selection, m_viewport.GetTargetRevision());
}

void EditorController::ProcessPicks()
{
    PickResult result;

    if (m_viewport.TakePickResult(result) && result.stamp == MakeStamp())
    {
        m_selection.SelectNode(result.node);
    }
}

void EditorController::Pick(Vec2 normalized)
{
    Vec3 origin;

    Vec3 direction;

    m_camera.MakeRay(normalized, origin, direction);

    m_selection.SelectNode(m_scene.PickBounds(origin, direction));

    // Stamped after the immediate selection, which advances the selection revision.
    m_viewport.RequestPick(normalized, MakeStamp());
}

bool EditorController::ResizeViewport(uint32_t width, uint32_t height)
{
    const bool valid = m_viewport.Resize(width, height);

    if (valid)
    {
        m_camera.SetExtent(m_viewport.GetRenderView().width, m_viewport.GetRenderView().height);
    }

    return valid;
}

void EditorController::FrameAll()
{
    sg::AABB bounds;

    if (m_scene.GetSceneBounds(bounds))
    {
        m_camera.Frame(bounds);
    }
}

void EditorController::FrameSelection()
{
    sg::AABB bounds;

    if (m_scene.GetBounds(m_selection.GetNode(), bounds))
    {
        m_camera.Frame(bounds);
    }
}

MeshPreviewRenderer& EditorController::GetMeshPreview()
{
    return m_meshPreview;
}

MeshPreviewSettings& EditorController::GetMeshPreviewSettings()
{
    return m_previewSettings;
}

EditorCamera& EditorController::GetMeshPreviewCamera()
{
    return m_previewCamera;
}

bool EditorController::PreviewMesh(SceneAssetId mesh, uint32_t width, uint32_t height)
{
    const sg::Mesh* source = m_scene.GetAssets().ResolveMesh(mesh);

    const bool valid       = source != nullptr && m_viewport.HasScene() && m_meshPreview.Resize(width, height);

    if (valid)
    {
        m_previewCamera.SetExtent(m_meshPreview.GetWidth(), m_meshPreview.GetHeight());

        if (mesh != m_previewMesh)
        {
            m_previewMesh = mesh;

            ResetMeshPreview();
        }

        const sg::Camera& camera = m_previewCamera.GetCamera();

        const Mat4 view          = camera.GetViewMatrix();

        m_meshPreview.Request({source, camera.GetProjectionMatrix() * view, Vec3(glm::inverse(view)[3]), m_previewSettings});
    }

    return valid;
}

void EditorController::ResetMeshPreview()
{
    sg::AABB bounds;

    if (m_scene.GetMeshBounds(m_previewMesh, bounds))
    {
        m_previewCamera.Frame(bounds);
    }
}
} // namespace zen::editor
