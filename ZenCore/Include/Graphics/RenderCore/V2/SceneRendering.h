#pragma once
#include "AssetLib/SceneLoader.h"
#include "Graphics/RenderCore/V2/RenderScene.h"
#include "Graphics/RenderCore/V2/RenderingSettings.h"

namespace zen::rc
{
// Shared scene publication and settings application for tools and runtime apps.
// CPU scenes and the camera are borrowed; the caller retains them through Destroy.
class SceneRendering
{
public:
    explicit SceneRendering(RenderDevice& device);
    void InitializeEnvironment(std::string defaultTexture, const EnvironmentSettings& environment);
    void Destroy();
    bool PrepareScene(const asset::LoadedScene& candidate, sg::Camera& camera, std::string& error);
    bool CommitScene(std::string& error);
    void DiscardScene();
    bool SetEnvironmentTexture(const std::string& path, std::string& error);
    bool SetEnvironmentLighting(float intensity, float rotationDegrees, bool lighting, bool skybox);
    bool ApplyRenderingSettings(const RenderingSettings& settings, bool retryResources, std::string& error);

    const EnvironmentSettings& GetEnvironment() const;
    const std::string&         GetDefaultEnvironment() const;
    RenderScene*               GetScene();
    const RenderScene*         GetScene() const;

private:
    RenderDevice&              m_device;
    UniquePtr<RenderScene>     m_scene;
    UniquePtr<RenderScene>     m_pendingScene;
    bool                       m_pendingPrepared{false};
    EnvironmentSettings        m_environment;
    HeapVector<RenderingLight> m_appliedLights;
    HeapVector<LightId>        m_runtimeLightIds;
    CameraLightSettings        m_cameraLight;
    std::string                m_defaultEnvironment;
};
} // namespace zen::rc
