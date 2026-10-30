#include "Graphics/RenderCore/V2/SceneRendering.h"
#include "Graphics/RenderCore/V2/Renderer/RendererServer.h"
#include "Graphics/RenderCore/V2/Renderer/DeferredLightingRenderer.h"
#include "AssetLib/SceneRuntimeBuilder.h"

namespace zen::rc
{
SceneRendering::SceneRendering(RenderDevice& device) : m_device(device) {}

void SceneRendering::InitializeEnvironment(std::string defaultTexture, const EnvironmentSettings& environment)
{
    VERIFY_EXPR_MSG(!m_scene && !m_pendingScene, "Set initial environment before preparing a scene");
    m_defaultEnvironment = std::move(defaultTexture);
    m_environment        = environment;
}

void SceneRendering::Destroy()
{
    DiscardScene();
    m_device.GetRendererServer()->SetRenderScene(nullptr);
    if (m_scene)
    {
        m_scene->Destroy();
        m_scene.Reset();
    }
    m_appliedLights.clear();
    m_runtimeLightIds.clear();
}

const EnvironmentSettings& SceneRendering::GetEnvironment() const
{
    return m_environment;
}

const std::string& SceneRendering::GetDefaultEnvironment() const
{
    return m_defaultEnvironment;
}

RenderScene* SceneRendering::GetScene()
{
    return m_scene.Get();
}

const RenderScene* SceneRendering::GetScene() const
{
    return m_scene.Get();
}

bool SceneRendering::SetEnvironmentTexture(const std::string& path, std::string& error)
{
    error.clear();

    // Allow clearing an override after opening an empty scene, including when its
    // source file has disappeared and would prevent the next scene from loading.
    const bool valid = m_scene ? m_scene->SetEnvironmentTexture(path, error) : path.empty();

    if (valid)
    {
        m_environment.texturePath = path;
    }
    else if (!m_scene)
    {
        error = "Open a scene with renderable geometry first.";
    }

    return valid;
}

bool SceneRendering::SetEnvironmentLighting(float intensity, float rotationDegrees, bool lighting, bool skybox)
{
    const bool valid = m_scene && m_scene->SetEnvironmentLighting(intensity, rotationDegrees, lighting, skybox);

    if (valid)
    {
        m_environment.intensity       = intensity;

        m_environment.rotationDegrees = rotationDegrees;

        m_environment.lighting        = lighting;

        m_environment.skybox          = skybox;
    }

    return valid;
}

bool SceneRendering::PrepareScene(const asset::LoadedScene& candidate, sg::Camera& camera, std::string& error)
{
    DiscardScene();

    error      = rc::RenderScene::GetResourceError(m_device.GetGPUInfo(), *candidate.scene, candidate.vertices.size(),
                                                   candidate.indices.size());

    bool valid = error.empty() && m_device.PrepareForResourceReconfiguration();

    if (valid && candidate.scene->GetRenderableCount() > 0)
    {
        asset::PrepareScenePlacement(*candidate.scene, asset::ScenePlacement::CenterAndNormalize);

        rc::SceneData data{};

        data.pScene              = candidate.scene.Get();

        data.pVertices           = candidate.vertices.data();

        data.numVertices         = uint32_t(candidate.vertices.size());

        data.pIndices            = candidate.indices.data();

        data.numIndices          = uint32_t(candidate.indices.size());

        data.pCamera             = &camera;

        data.envTextureName      = m_defaultEnvironment;

        data.environmentOverride = m_environment.texturePath;

        m_pendingScene           = MakeUnique<rc::RenderScene>(&m_device, data);

        valid                    = m_pendingScene->Init()
                                && m_pendingScene->SetEnvironmentLighting(m_environment.intensity, m_environment.rotationDegrees,
                                                                          m_environment.lighting, m_environment.skybox);
    }

    // Finish candidate uploads without resetting the active scene's bindings. The
    // progress dialog can render another frame with the old scene before commit.
    valid = valid && m_device.PrepareForResourceReconfiguration();

    if (!valid)
    {
        DiscardScene();

        error = error.empty() ? "Scene GPU preparation failed. The previous scene remains active." : error;
    }

    m_pendingPrepared = valid;

    return valid;
}

bool SceneRendering::CommitScene(std::string& error)
{
    VERIFY_EXPR_MSG(m_pendingPrepared, "SceneRendering::CommitScene requires a successful PrepareScene");

    // Drain the last old-scene frame and reset its bindings at the actual switch.
    // No rendering may occur between this boundary and publishing the new scene.
    const bool valid = m_device.PrepareForSceneReplacement();

    if (valid)
    {
        m_device.GetRendererServer()->SetRenderScene(m_pendingScene.Get());

        if (m_scene)
        {
            m_scene->Destroy();
        }

        m_scene = std::move(m_pendingScene);

        m_appliedLights.clear();

        m_runtimeLightIds.clear();

        if (m_scene)
        {
            m_scene->SetCameraLight(m_cameraLight);

            for (const rc::LightEntry& entry : m_scene->GetLights().GetEntries())
            {
                m_appliedLights.push_back({entry.id, rc::LightOrigin::eImported, entry.light});

                m_runtimeLightIds.push_back(entry.id);
            }
        }

        rc::RendererServer& server = *m_device.GetRendererServer();

        rc::DebugSelection debug   = server.GetDebugSelection();

        debug.lightId              = m_runtimeLightIds.empty() ? 0 : m_runtimeLightIds.front();

        server.SetRenderingSelection(server.GetRequestedRenderOption() == rc::RenderOption::eVoxelGI
                                         ? rc::RenderAlgorithm::eVoxelGI
                                         : rc::RenderAlgorithm::ePBR,
                                     debug);

        m_pendingPrepared = false;

        error.clear();
    }
    else
    {
        error = "Could not prepare the renderer for the scene switch. The previous scene remains active.";
    }

    return valid;
}

void SceneRendering::DiscardScene()
{
    if (m_pendingScene)
    {
        m_device.PrepareForResourceReconfiguration();

        m_pendingScene->Destroy();

        m_pendingScene.Reset();
    }

    m_pendingPrepared = false;
}

bool SceneRendering::ApplyRenderingSettings(const rc::RenderingSettings& settings, bool retryResources, std::string& error)
{
    rc::RendererServer& server = *m_device.GetRendererServer();

    uint64_t reflectanceBytes  = 0;

    bool valid                 = rc::ValidateRenderingSettings(settings, error);

    if (valid
        && (settings.environment.texturePath != m_environment.texturePath
            || (m_scene
                && !m_scene->ValidateEnvironmentLighting(settings.environment.intensity,
                                                         settings.environment.rotationDegrees))))
    {
        error = "Environment texture must finish loading and its intensity must fit the scene's authored environment.";

        valid = false;
    }

    HeapVector<rc::SceneLight> lights;

    HeapVector<rc::SceneLight> appliedLights;

    HeapVector<rc::LightEntry> replacements;

    bool lightsChanged = settings.lights.size() != m_appliedLights.size();

    for (const rc::RenderingLight& light : m_appliedLights)
    {
        appliedLights.push_back(light.light);
    }

    for (size_t index = 0; index < settings.lights.size(); ++index)
    {
        const rc::RenderingLight& light = settings.lights[index];

        lights.push_back(light.light);

        rc::LightId runtimeId = 0;

        for (size_t old = 0; old < m_appliedLights.size(); ++old)
        {
            if (m_appliedLights[old].id == light.id)
            {
                runtimeId = m_runtimeLightIds[old];
            }
        }

        replacements.push_back({runtimeId, light.light});

        lightsChanged |= index >= m_appliedLights.size() || light.id != m_appliedLights[index].id
                      || !rc::EqualSceneLight(light.light, m_appliedLights[index].light);
    }

    const uint32_t shadowFaces = rc::CountShadowFaces(lights) + rc::CameraLightShadowFaces(settings.cameraLight);

    if (valid && !server.ValidateShadowResources(settings, shadowFaces))
    {
        error = "Shadow memory preflight rejected the edit. Reduce resolution or the number of shadow-casting lights.";

        valid = false;
    }

    if (valid && server.ValidateVoxelGIResources(settings.gi, reflectanceBytes) != rc::GIResourceStatus::eSuccess)
    {
        error = "GI resource preflight rejected the settings. Check the reflectance budget and device limits.";

        valid = false;
    }

    // Light, environment and output edits arrive every frame during a drag. They skip the GI
    // apply unless its values change or Apply explicitly retries resources.
    if (valid && (retryResources || settings.gi != server.GetVoxelGISettings()))
    {
        valid = server.ApplyVoxelGISettings(settings.gi, retryResources);

        if (!valid)
        {
            error = "Renderer could not apply the settings at this frame boundary.";
        }
    }

    if (valid)
    {
        if (m_scene)
        {
            if (lightsChanged)
            {
                valid = m_scene->ReplaceLights(replacements);

                if (valid)
                {
                    m_appliedLights = settings.lights;

                    m_runtimeLightIds.clear();

                    for (const rc::LightEntry& entry : replacements)
                    {
                        m_runtimeLightIds.push_back(entry.id);
                    }
                }

                // A different shadow array may now fit, so a latched fallback gets one new attempt.
                if (valid && rc::CountShadowFaces(lights) != rc::CountShadowFaces(appliedLights))
                {
                    server.ResetRenderingFailure();
                }
            }

            valid = valid && m_scene->SetCameraLight(settings.cameraLight);

            if (valid && rc::CameraLightShadowFaces(settings.cameraLight) != rc::CameraLightShadowFaces(m_cameraLight))
            {
                server.ResetRenderingFailure();
            }

            valid = valid
                 && m_scene->SetEnvironmentLighting(settings.environment.intensity, settings.environment.rotationDegrees,
                                                    settings.environment.lighting, settings.environment.skybox);
        }

        if (valid)
        {
            m_environment            = settings.environment;

            m_cameraLight            = settings.cameraLight;

            rc::DebugSelection debug = settings.debug;

            debug.lightId            = 0;

            for (size_t index = 0; index < m_appliedLights.size(); ++index)
            {
                if (m_appliedLights[index].id == settings.debug.lightId)
                {
                    debug.lightId = m_runtimeLightIds[index];
                }
            }

            server.SetRenderingSelection(settings.algorithm, debug);

            server.RequestDeferredLightingRenderer()->SetLightMarkers(
                settings.lightMarkers && debug.output == rc::DebugOutput::eFinal, settings.lightMarkerSize);
        }
    }

    return valid;
}

} // namespace zen::rc
