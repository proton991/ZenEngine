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
        const EditorRenderSnapshot snapshot = m_viewport.GetSnapshot();

        rc::RenderingSettings settings;

        settings.gi              = snapshot.settings;

        settings.environment     = m_viewport.GetEnvironment();

        settings.lightMarkers    = snapshot.lightMarkers;

        settings.lightMarkerSize = snapshot.lightMarkerSize;

        settings.algorithm =
            snapshot.requestedMode == rc::RenderOption::eVoxelGI ? rc::RenderAlgorithm::eVoxelGI : rc::RenderAlgorithm::ePBR;

        settings.debug.output =
            snapshot.requestedMode == rc::RenderOption::eVoxelize ? rc::DebugOutput::eVoxels : rc::DebugOutput::eFinal;

        m_rendering.Initialize(settings);

        m_renderDefaults = settings;

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
        {actions::Play, "Run", {}, "Available with zen_player (rendering milestone 4)."},
        {actions::Pause, "Pause", {}, "Simulation is deferred; rendering sessions do not pause."},
        {actions::Stop, "Stop", {}, "Available with zen_player (rendering milestone 4)."}};

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

const EditorRenderingState& EditorController::GetRenderingState() const
{
    return m_rendering;
}

bool EditorController::StageRenderingSettings(const rc::RenderingSettings& settings, bool applyResources)
{
    const bool valid = m_rendering.Stage(settings);

    // Every valid edit previews; Apply also publishes resource changes and retries failures.
    m_renderResourcesRequested = valid && (applyResources || m_renderResourcesRequested);

    m_renderApplyRequested     = valid;

    return valid;
}

bool EditorController::UpdateRenderingSettings()
{
    bool applied = false;

    if (m_renderApplyRequested && !m_loadState.IsActive() && !m_environmentPending)
    {
        const bool resources                 = m_renderResourcesRequested || !m_rendering.NeedsResourceApply();

        const rc::RenderingSettings settings = resources ? m_rendering.GetDraft() : m_rendering.GetPreview();

        std::string error;

        applied = m_viewport.ApplyRenderingSettings(settings, m_renderResourcesRequested, error);

        if (applied && resources)
        {
            m_rendering.Commit();
        }
        else if (applied)
        {
            m_rendering.CommitPreview();
        }
        else
        {
            m_rendering.SetError(std::move(error));
        }

        m_renderApplyRequested     = false;

        m_renderResourcesRequested = false;
    }

    return applied;
}

void EditorController::RevertRenderingSettings()
{
    m_rendering.Revert();

    m_renderApplyRequested     = false;

    m_renderResourcesRequested = false;
}

bool EditorController::AddRenderingLight(rc::SceneLightType type)
{
    rc::RenderingSettings draft = m_rendering.GetDraft();

    rc::RenderingLight entry;

    entry.id         = m_nextLightId++;

    entry.light.type = type;

    m_camera.MakeRay(Vec2(0.5f), entry.light.position, entry.light.direction);

    entry.light.position = m_camera.GetCamera().GetPos();

    sg::AABB bounds;

    const float span = m_scene.GetSceneBounds(bounds) ? std::max(bounds.GetMaxExtent(), 0.01f) : 1.0f;

    // Put new positional lights in view so their viewport handles can be grabbed immediately.
    if (type != rc::SceneLightType::eDirectional)
    {
        entry.light.position += entry.light.direction * span;
    }

    entry.light.intensity = type == rc::SceneLightType::eDirectional ? 1.0f : span * span * 0.75f;

    entry.light.range     = span * 2.0f;

    draft.lights.push_back(entry);

    const bool valid = draft.lights.size() <= rc::MaxSceneLights;

    if (valid)
    {
        StageRenderingSettings(draft);
    }
    else
    {
        m_rendering.SetError("Cannot add light: maximum 32 lights.");
    }

    return valid;
}

bool EditorController::AddBoundsLights(bool corners)
{
    rc::RenderingSettings draft = m_rendering.GetDraft();

    sg::AABB bounds;

    const bool valid = m_scene.GetSceneBounds(bounds) && draft.lights.size() + (corners ? 8u : 6u) <= rc::MaxSceneLights;

    if (valid)
    {
        for (const rc::SceneLight& light : rc::BuildBoundsLightPreset(bounds, corners))
        {
            draft.lights.push_back({m_nextLightId++, corners ? rc::LightOrigin::eCorners : rc::LightOrigin::eSides, light});
        }

        StageRenderingSettings(draft);
    }
    else
    {
        m_rendering.SetError("Cannot add preset: open a scene and leave space for the entire preset (maximum 32 lights).");
    }

    return valid;
}

void EditorController::ResetRenderingLights()
{
    rc::RenderingSettings draft = m_rendering.GetDraft();

    draft.lights                = m_importedLights;

    StageRenderingSettings(draft);
}

const rc::RenderingSettings& EditorController::GetRenderingDefaults() const
{
    return m_renderDefaults;
}

void EditorController::ResetRenderingSetup()
{
    rc::RenderingSettings settings   = m_renderDefaults;

    settings.scenePath               = m_rendering.GetDraft().scenePath;

    settings.normalizationCenter     = m_rendering.GetDraft().normalizationCenter;

    settings.normalizationScale      = m_rendering.GetDraft().normalizationScale;

    settings.lights                  = m_importedLights;

    settings.environment.texturePath = m_rendering.GetApplied().environment.texturePath;

    StageRenderingSettings(settings);

    RequestEnvironmentTexture("");
}

void EditorController::RecordRenderingResult(bool succeeded)
{
    const EditorRenderSnapshot snapshot = m_viewport.GetSnapshot();

    if ((!m_hasSuccessfulRendering || m_successfulRenderingRevision != m_rendering.GetAppliedRevision()) && succeeded
        && m_viewport.HasScene() && snapshot.debug.available && snapshot.debug.output == rc::DebugOutput::eFinal
        && snapshot.status.fallbackReason.empty())
    {
        m_lastSuccessfulRendering     = m_rendering.GetApplied();

        m_hasSuccessfulRendering      = true;

        m_successfulRenderingRevision = m_rendering.GetAppliedRevision();
    }
}

bool EditorController::CanRestoreRenderingSettings() const
{
    return m_hasSuccessfulRendering && m_lastSuccessfulRendering.scenePath == m_rendering.GetApplied().scenePath;
}

void EditorController::RestoreRenderingSettings()
{
    if (CanRestoreRenderingSettings())
    {
        rc::RenderingSettings settings   = m_lastSuccessfulRendering;

        settings.environment.texturePath = m_rendering.GetApplied().environment.texturePath;

        StageRenderingSettings(settings, true);

        if (settings.environment.texturePath != m_lastSuccessfulRendering.environment.texturePath)
        {
            RequestEnvironmentTexture(m_lastSuccessfulRendering.environment.texturePath);
        }
    }
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

        m_importedLights.clear();

        if (m_viewport.GetRenderScene() != nullptr)
        {
            for (const rc::LightEntry& entry : m_viewport.GetRenderScene()->GetLights().GetEntries())
            {
                m_importedLights.push_back({entry.id, rc::LightOrigin::eGLTF, entry.light});

                m_nextLightId = std::max(m_nextLightId, entry.id + 1);
            }
        }

        const LoadedScene& loaded = *m_scene.Get();

        m_rendering.ChangeScene(path, loaded.normalizationCenter, loaded.normalizationScale, m_importedLights);

        m_hasSuccessfulRendering = false;

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
        else
        {
            m_rendering.CommitEnvironmentTexture(m_requestedEnvironment);
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
