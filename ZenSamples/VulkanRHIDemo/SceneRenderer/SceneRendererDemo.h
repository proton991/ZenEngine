#pragma once
#include "AssetTools/SceneTools.h"
#include "AssetLib/SceneLoader.h"
#include "Platform/Timer.h"
#include "SceneGraph/Camera.h"
#include "Graphics/RenderCore/V2/RenderScene.h"
#include "Platform/NativeWindow.h"
#include "SceneRendererDemoProfiling.h"
#if defined(ZEN_RUNTIME_UI)
#    include "RuntimeUI/RuntimeSceneControls.h"
#endif

namespace zen
{
namespace ui
{
class RuntimeDebugUI;
}

namespace rc
{
class VoxelizerBase;
}
class SceneRendererDemo
#if defined(ZEN_RUNTIME_UI)
    : public ui::RuntimeSceneControls
#endif
{
public:
    SceneRendererDemo(const platform::WindowConfig& windowConfig,
                      sg::CameraType                type,
                      const DemoProfilingOptions&   profiling     = {},
                      bool                          useAssetCache = true);

    ~SceneRendererDemo();

    bool Prepare(bool     captureVoxels             = false,
                 uint32_t calibrationGridPercent    = 0,
                 bool     deterministicGuideCapture = false,
                 uint32_t sceneLoadRepeats          = 0);

    bool EnableRuntimeUI();

#if defined(ZEN_RUNTIME_UI)
    ui::RuntimeSceneSettings GetRuntimeSceneSettings() const override;

    bool ApplyRuntimeSceneSettings(const ui::RuntimeSceneSettings& previous, const ui::RuntimeSceneSettings& next) override;

    const ui::RuntimeModelState& GetRuntimeModelState() const override;

    void RefreshRuntimeModels() override;

    bool RequestRuntimeModel(const std::string& path) override;
    bool CancelRuntimeModel() override;
#endif

    bool Run(uint32_t           frameLimit            = 0,
             bool               smokeTest             = false,
             uint32_t           initialMode           = 1,
             const std::string& frameTimesPath        = {},
             bool               fixedStep             = false,
             uint32_t           giStartFrame          = 0,
             bool               motionFixture         = false,
             bool               profileWarmup         = false,
             uint32_t           backgroundTestSeconds = 0);

    bool CaptureFrame(const std::string& path);

    bool CaptureLighting(const std::string& path);
    bool CaptureRayCounts(const std::string& path);

    bool CaptureProviderSwitching(const std::string& path, bool individualFeatures = false);

    bool CaptureOriginStress(const std::string& path);

    bool CaptureStability(const std::string& path);
    bool CaptureCameraMotion(const std::string& path);
    bool CaptureBounceLightChanges(const std::string& path);
    bool CaptureReflectionMotion(const std::string& path);
    bool CaptureGeometryMotion(const std::string& path);
    bool CaptureIndependentViews(const std::string& path);

    bool CaptureVoxelVolume(const std::string& path);

    bool CaptureVoxelReference(const std::string& path);

    bool CaptureVoxelLifecycle(const std::string& path);

    bool CaptureVoxelGBuffer(const std::string& path);

    void StopProfiling();

    bool Destroy(bool runSucceeded = true);

private:
    friend struct SceneRendererModelTestAccess;

    bool LoadModel(const std::string& path, bool configuredCameraPosition);

    bool PublishModel(UniquePtr<asset::LoadedScene> candidate, bool configuredCameraPosition);

#if defined(ZEN_RUNTIME_UI)
    void ProcessPendingModel();
#endif

    bool CaptureVoxelOutput(const std::string& path, rc::VoxelizerBase& output);

    void OnResize(uint32_t width, uint32_t height);

    void RunSmokeStep(uint32_t frame);

    void PrepareLighting(const platform::ConfigLoader& config);

    void UpdateDynamicLight(float frameTime);

    bool GetGIMotionFixture(uint32_t& moving, Mat4& original) const;
    bool        PrepareProfileWorkload();
    bool        UpdateProfileWorkload(uint32_t frame);
    std::string m_profileWorkload{"still"};
    std::string m_profileTargetScene;
    uint32_t    m_profileInstance{UINT32_MAX};
    Mat4        m_profileTransform{1};
    Vec3        m_profileCamera{0};
    float       m_profileExtent{1};
    bool        m_profileInitialized{false};

    bool m_useAssetCache{true};

    UniquePtr<sg::Camera> m_camera;

    sg::CameraType m_cameraType{sg::CameraType::eFirstPerson};

    UniquePtr<rc::RenderDevice> m_renderDevice;

    UniquePtr<sg::Scene>       m_scene;
    UniquePtr<rc::RenderScene> m_renderScene;

    platform::NativeWindow* m_pWindow{nullptr};

    RHIViewport* m_pViewport{nullptr};

    UniquePtr<platform::Timer> m_timer;
    rc::LightId                m_dynamicLight{0};
    // Config slots remain stable; without an override these IDs own the AABB preset.
    std::array<rc::LightId, rc::MaxSceneLights>    m_editableLightIds{};
    uint32_t                                       m_editableLightCount{0};
    std::array<rc::SceneLight, rc::MaxSceneLights> m_editableLightDefaults{};
    bool                                           m_boundsPresetLights{false};
    uint32_t                                       m_modelLightCount{0};
    uint32_t                                       m_animatedLightIndex{0};
    Vec3                                           m_orbitCenter{0.0f, 1.0f, 0.0f};
    float                                          m_orbitRadius{1.0f};
    float                                          m_orbitSpeedDegrees{45.0f};
    double                                         m_lightAngle{0.0};
    uint64_t                                       m_motionFrame{0};
    uint64_t                                       m_captureHistoryId{0};
    UniquePtr<SceneRendererProfiling>              m_profiling;

#if defined(ZEN_RUNTIME_UI)
    UniquePtr<ui::RuntimeDebugUI> m_runtimeUI;

    ui::RuntimeModelState                                    m_modelState;
    std::future<asset::SceneLoadResult>                      m_modelLoadResult;
    SharedPtr<asset::SceneImportControl, MultiThreadCounter> m_modelLoadControl;
    UniquePtr<asset::SourceSceneLoader>                      m_sceneLoader;
#endif
};
} // namespace zen
