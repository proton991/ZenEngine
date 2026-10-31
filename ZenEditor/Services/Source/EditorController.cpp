#include "AssetTools/SceneTools.h"
#include "Editor/Services/EditorController.h"
#include "Platform/ConfigLoader.h"
#include "Utils/Errors.h"
#include <chrono>

namespace zen::editor
{
namespace
{
#if defined(ZEN_MACOS)
constexpr uint16_t       kCommand = uint16_t(platform::KeyModifier::Super);
constexpr EditorShortcut kRedoShortcut{platform::Key::Z, uint16_t(kCommand | uint16_t(platform::KeyModifier::Shift))};
#else
constexpr uint16_t       kCommand = uint16_t(platform::KeyModifier::Control);
constexpr EditorShortcut kRedoShortcut{platform::Key::Y, kCommand};
#endif

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

EditorController::EditorController(rc::RenderDevice& device) :
    m_device(device),
    m_selection(m_scene),
    m_inspector(m_scene, m_selection),
    m_viewport(device, m_scene, m_selection, m_camera),
    m_playSession(device, m_viewport.GetSceneRendering(), m_viewport.GetSceneTarget(), m_camera),
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

        m_renderDefaults                = settings;

        m_configuredEnvironmentLighting = settings.environment.lighting;

        RegisterActions();

        RefreshEnvironmentTextures();
    }

    return valid;
}

void EditorController::Destroy()
{
    m_playSession.Shutdown();
    CancelSceneLoad();
    m_sceneLoader.Reset();

    m_loadResult = {};

    m_meshPreview.Destroy();

    m_viewport.Destroy();

    m_loadCandidate.Reset();

    m_loadState           = {};

    m_consecutiveDeclines = 0;
}

void EditorController::RegisterActions()
{
    m_inspector.RegisterActions(m_actions);

    m_actions.Register({actions::Open,
                        "Open...",
                        {platform::Key::O, kCommand},
                        "Wait for the current scene to finish opening.",
                        [this]() { return !IsPreviewFrozen() && !m_loadState.IsActive() && !m_environmentPending; },
                        [this]() { m_openRequested = true; }});

    m_actions.Register({actions::FrameAll,
                        "Frame All",
                        {platform::Key::Home, 0},
                        "Open a scene first.",
                        [this]() { return !IsPreviewFrozen() && m_scene.Get() != nullptr; },
                        [this]() { FrameAll(); }});

    m_actions.Register({actions::Recook,
                        "Recook from Source",
                        {},
                        "Open a source scene and wait for pending work to finish.",
                        [this]() { return CanRecook(); },
                        [this]() { RequestRecook(); }});

    m_actions.Register({actions::FrameSelection,
                        "Frame Selected",
                        {platform::Key::F, 0},
                        "Select a node first.",
                        [this]() { return !IsPreviewFrozen() && m_selection.GetNode().generation != 0; },
                        [this]() { FrameSelection(); }});

    const Placeholder placeholders[] = {
        {actions::Save, "Save", {platform::Key::S, kCommand}, "Available with scene documents (plan step 5)."},
        {actions::Undo, "Undo", {platform::Key::Z, kCommand}, "Available with undoable editing (plan step 6)."},
        {actions::Redo, "Redo", kRedoShortcut, "Available with undoable editing (plan step 6)."}};

    m_actions.Register({actions::Play,
                        "Run / Resume",
                        {},
                        "Run requires a loaded scene, valid settings and no active runtime. Resume is available when paused.",
                        [this]() { return CanRun() || m_playSession.GetState() == runtime::RuntimeState::Paused; },
                        [this]() { StartOrResumeRuntime(); }});
    m_actions.Register({actions::Pause,
                        "Pause",
                        {},
                        "Pause is available while the play window is running.",
                        [this]() { return m_playSession.GetState() == runtime::RuntimeState::Running; },
                        [this]() { m_playSession.Pause(); }});
    m_actions.Register(
        {actions::Stop,
         "Quit Play",
         {},
         "Open the play window first.",
         [this]() { return m_playSession.IsActive() && m_playSession.GetState() != runtime::RuntimeState::Stopping; },
         [this]() { m_playSession.Quit(); }});

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
    bool valid = false;
    if (!IsPreviewFrozen())
    {
        valid = m_rendering.Stage(settings);
        // Every valid edit previews; Apply also publishes resource changes and retries failures.
        m_renderResourcesRequested = valid && (applyResources || m_renderResourcesRequested);
        m_renderApplyRequested     = valid;
    }

    return valid;
}

bool EditorController::UpdateRenderingSettings()
{
    bool applied = false;

    if (!IsPreviewFrozen() && m_renderApplyRequested && !m_loadState.IsActive() && !m_environmentPending)
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

bool EditorController::RecordRenderingResult(rc::RenderFrameResult result, bool drawScene)
{
    const bool succeeded                = drawScene && result == rc::RenderFrameResult::eSubmitted;

    const bool retryDeclines            = rc::TrackDeclinedFrames(result, m_consecutiveDeclines);

    const EditorRenderSnapshot snapshot = m_viewport.GetSnapshot();

    if ((!m_hasSuccessfulRendering || m_successfulRenderingRevision != m_rendering.GetAppliedRevision()) && succeeded
        && m_viewport.HasScene() && snapshot.debug.available && snapshot.debug.output == rc::DebugOutput::eFinal
        && snapshot.status.fallbackReason.empty())
    {
        m_lastSuccessfulRendering     = m_rendering.GetApplied();

        m_hasSuccessfulRendering      = true;

        m_successfulRenderingRevision = m_rendering.GetAppliedRevision();
    }

    return result != rc::RenderFrameResult::eBlocked && retryDeclines;
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
                m_importedLights.push_back({entry.id, rc::LightOrigin::eImported, entry.light});

                m_nextLightId = std::max(m_nextLightId, entry.id + 1);
            }
        }

        const LoadedScene& loaded = *m_scene.Get();

        // Imported lights replace the environment light. The defaults follow, so resets keep it off.
        m_renderDefaults.environment.lighting = rc::ResolveEnvironmentLighting(
            m_configuredEnvironmentLighting, !m_importedLights.empty(), platform::ConfigLoader::GetInstance());

        m_rendering.ChangeScene(path, loaded.normalizationCenter, loaded.normalizationScale, m_importedLights,
                                m_renderDefaults.environment.lighting);

        // Only the lighting state follows the scene; the session's environment preview survives.
        const EditorEnvironment environment = m_viewport.GetEnvironment();

        m_viewport.SetEnvironmentLighting(environment.intensity, environment.rotationDegrees,
                                          m_renderDefaults.environment.lighting, environment.skybox);

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
    return Load(MakeLoadRequest(path));
}

asset::SceneImportRequest EditorController::MakeLoadRequest(std::string path) const
{
    asset::SceneImportRequest request{std::move(path)};
    ApplySceneProcessingSettings(m_sourceProcessing.draft, request);
    return request;
}

bool EditorController::Load(const asset::SceneImportRequest& request)
{
    bool valid = false;

    if (!IsPreviewFrozen() && !m_loadState.IsActive() && !m_environmentPending)
    {
        asset::SceneLoadResult imported  = asset::LoadSourceScene(request);
        std::string            error     = std::move(imported.error);
        UniquePtr<LoadedScene> candidate = std::move(imported.scene);

        valid = candidate && m_viewport.PrepareScene(*candidate, error) && PublishScene(std::move(candidate), error);

        if (valid)
        {
            CommitSourceProcessing(request, imported.cacheHit);
            // Messages belong to the published scene; a failed open keeps the previous ones.
            m_importDiagnostics = std::move(imported.diagnostics);
            m_loadState         = {};
            m_pendingDiagnostics.clear();
        }
        else
        {
            m_pendingDiagnostics = std::move(imported.diagnostics);
            m_error              = error;

            LOGE("Editor open failed: {}", m_error);
        }
    }

    return valid;
}

bool EditorController::RequestLoad(std::string path)
{
    return RequestLoad(MakeLoadRequest(std::move(path)));
}

bool EditorController::RequestLoad(asset::SceneImportRequest request)
{
    const bool accepted = !IsPreviewFrozen() && !request.path.empty() && !m_loadState.IsActive() && !m_environmentPending;

    if (accepted)
    {
        m_loadState        = {SceneLoadStage::Queued, request.path};
        m_loadState.recook = request.forceRecook;
        m_loadControl.Reset(new asset::SceneImportControl);
        request.control     = m_loadControl;
        m_queuedLoadRequest = std::move(request);
        m_pendingCacheHit   = false;
        m_pendingDiagnostics.clear();
        m_error.clear();
    }

    return accepted;
}

bool EditorController::CanRecook() const
{
    return !IsPreviewFrozen() && !m_sourceProcessing.sourcePath.empty() && !m_loadState.IsActive() && !m_environmentPending;
}

bool EditorController::RequestRecook()
{
    bool accepted = false;
    if (CanRecook())
    {
        asset::SceneImportRequest request = m_appliedLoadRequest;
        ApplySceneProcessingSettings(m_sourceProcessing.draft, request);
        request.useCache    = true;
        request.forceRecook = true;
        accepted            = RequestLoad(std::move(request));
    }
    return accepted;
}

bool EditorController::StageSourceProcessing(const SceneProcessingSettings& settings)
{
    const zenasset::Status status = zenasset::ValidateProcessingSettings(settings.settings);
    m_sourceProcessing.error      = status.message;
    if (status.Succeeded())
    {
        m_sourceProcessing.draft = settings;
    }
    return status.Succeeded();
}

const SceneProcessingState& EditorController::GetSourceProcessing() const
{
    return m_sourceProcessing;
}

void EditorController::CommitSourceProcessing(const asset::SceneImportRequest& request, bool cacheHit)
{
    const bool source             = asset::FindSceneImportFormat(request.path) != nullptr;
    m_sourceProcessing.sourcePath = source ? request.path : std::string{};
    m_sourceProcessing.applied =
        source ? SceneProcessingSettings{request.profile == "desktop", request.processing} : SceneProcessingSettings{};
    m_sourceProcessing.cacheHit = cacheHit;
    m_appliedLoadRequest        = source ? request : asset::SceneImportRequest{};
    m_appliedLoadRequest.control.Reset();
    m_appliedLoadRequest.forceRecook = false;
}

bool EditorController::CancelSceneLoad()
{
    const bool accepted = m_loadState.stage == SceneLoadStage::Queued || m_loadState.stage == SceneLoadStage::Reading
                       || m_loadState.stage == SceneLoadStage::Preparing || m_loadState.stage == SceneLoadStage::Publishing;
    if (accepted)
    {
        m_loadState.cancelRequested = true;
        if (m_loadControl)
        {
            m_loadControl->Cancel();
        }
        // A running future stays owned until its jobs have drained. Queued or
        // completed CPU work can be discarded immediately on this thread.
        if (m_loadState.stage != SceneLoadStage::Reading)
        {
            m_viewport.DiscardScene();
            m_loadCandidate.Reset();
            m_loadControl.Reset();
            m_loadState.stage = SceneLoadStage::Cancelled;
        }
    }
    return accepted;
}

const HeapVector<asset::SceneImportDiagnostic>& EditorController::GetImportDiagnostics() const
{
    return m_importDiagnostics;
}

const HeapVector<asset::SceneImportDiagnostic>& EditorController::GetLastLoadDiagnostics() const
{
    return m_pendingDiagnostics;
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
    const bool accepted =
        !IsPreviewFrozen() && !m_loadState.IsActive() && !m_environmentPending && (m_viewport.HasScene() || path.empty());

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
    if (m_loadState.stage == SceneLoadStage::Preparing || m_loadState.stage == SceneLoadStage::Publishing)
    {
        m_pendingDiagnostics.push_back({asset::SceneDiagnosticSeverity::Error, asset::SceneImportStatus::InvalidData,
                                        m_loadState.path, "Graphics preparation", error});
    }
    m_loadResult = {};
    m_loadControl.Reset();

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
            if (!m_sceneLoader)
            {
                m_sceneLoader = MakeUnique<asset::SourceSceneLoader>(1, 0);
            }
            m_loadResult      = m_sceneLoader->LoadAsync(m_queuedLoadRequest);

            m_loadState.stage = SceneLoadStage::Reading;

            break;
        }
        case SceneLoadStage::Reading:
        {
            m_loadState.importStage  = m_loadControl->GetStage();
            m_loadState.fileProgress = m_loadControl->GetFileProgress();
            if (m_loadResult.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
            {
                asset::SceneLoadResult result = m_loadResult.get();

                m_loadState.fileProgress      = m_loadControl->GetFileProgress();
                m_loadResult                  = {};
                m_pendingDiagnostics          = std::move(result.diagnostics);
                m_pendingCacheHit             = result.cacheHit;
                if (m_loadState.cancelRequested || result.status == asset::SceneImportStatus::Cancelled)
                {
                    m_loadControl.Reset();
                    m_loadState.stage = SceneLoadStage::Cancelled;
                }
                else if (result.scene)
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
                CommitSourceProcessing(m_queuedLoadRequest, m_pendingCacheHit);
                // Messages describe the published scene; failed or cancelled
                // opens leave the active scene's messages in place.
                m_importDiagnostics = std::move(m_pendingDiagnostics);
                m_loadControl.Reset();
                m_loadState.stage = SceneLoadStage::Complete;
            }
            else
            {
                FailSceneLoad(std::move(error));
            }

            break;
        }
        case SceneLoadStage::Complete:
        case SceneLoadStage::Cancelled: m_loadState = {}; break;
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

    if (m_viewport.TakePickResult(result) && !IsPreviewFrozen() && result.stamp.scene == m_scene.GetGeneration()
        && result.stamp.selection == m_selection.GetRevision())
    {
        m_selection.SelectNode(result.node);
    }
}

void EditorController::Pick(Vec2 normalized)
{
    if (!IsPreviewFrozen())
    {
        // Keep the current selection until the visible-surface result is ready.
        m_viewport.RequestPick(normalized, MakeStamp());
    }
}

bool EditorController::ResizeViewport(uint32_t width, uint32_t height)
{
    const bool valid = IsPreviewFrozen() ? m_viewport.GetRenderView().color != nullptr : m_viewport.Resize(width, height);

    if (valid && !IsPreviewFrozen())
    {
        m_camera.SetExtent(m_viewport.GetRenderView().width, m_viewport.GetRenderView().height);
    }

    return valid;
}

void EditorController::FrameAll()
{
    sg::AABB bounds;

    if (!IsPreviewFrozen() && m_scene.GetSceneBounds(bounds))
    {
        m_camera.Frame(bounds);
    }
}

void EditorController::FrameSelection()
{
    sg::AABB bounds;

    if (!IsPreviewFrozen() && m_scene.GetBounds(m_selection.GetNode(), bounds))
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

    const bool valid = !IsPreviewFrozen() && source != nullptr && m_viewport.HasScene() && m_meshPreview.Resize(width, height);

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

runtime::RuntimeWindow& EditorController::GetPlaySession()
{
    return m_playSession;
}

runtime::RuntimeWindowSettings& EditorController::GetRunSettings()
{
    return m_runSettings;
}

bool EditorController::IsPreviewFrozen() const
{
    return m_playSession.IsActive();
}

bool EditorController::CanRun() const
{
    std::string error;
    return !IsPreviewFrozen() && m_viewport.HasScene() && !m_loadState.IsActive() && !m_environmentPending
        && rc::ValidateRenderingSettings(m_rendering.GetDraft(), error);
}

bool EditorController::StartOrResumeRuntime()
{
    bool accepted = false;
    if (m_playSession.GetState() == runtime::RuntimeState::Paused)
    {
        accepted = m_playSession.Resume();
    }
    else if (CanRun())
    {
        bool ready = true;
        // Apply pending edits once; an already-applied scene needs no reconfiguration.
        if (m_rendering.IsPending() || m_renderApplyRequested)
        {
            m_renderApplyRequested     = true;
            m_renderResourcesRequested = m_renderResourcesRequested || m_rendering.NeedsResourceApply();
            ready                      = UpdateRenderingSettings();
        }
        if (ready)
        {
            accepted = m_playSession.Start(m_runSettings, m_preferences.cameraMoveSpeed);
            if (accepted)
            {
                m_meshPreview.Cancel();
            }
        }
    }
    return accepted;
}

void EditorController::UpdatePlaySession()
{
    m_playSession.Update();
    m_actions.SetLabel(actions::Play, m_playSession.GetState() == runtime::RuntimeState::Paused ? "Resume" : "Run");
}
} // namespace zen::editor
