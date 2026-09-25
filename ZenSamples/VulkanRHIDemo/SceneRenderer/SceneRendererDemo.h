#pragma once
#include "Platform/Timer.h"
#include "SceneGraph/Camera.h"
#include "Graphics/RenderCore/V2/RenderScene.h"
#include "Platform/GlfwWindow.h"


namespace zen
{
namespace rc
{
class VoxelizerBase;
}
class SceneRendererDemo
{
public:
    SceneRendererDemo(const platform::WindowConfig& windowConfig, sg::CameraType type);

    ~SceneRendererDemo();

    bool Prepare(bool captureVoxels = false, uint32_t calibrationGridPercent = 0);

    bool Run(uint32_t frameLimit               = 0,
             bool smokeTest                    = false,
             uint32_t initialMode              = 1,
             const std::string& frameTimesPath = {},
             bool fixedStep                    = false,
             uint32_t giStartFrame             = 0,
             bool motionFixture                = false);

    bool CaptureFrame(const std::string& path);
    bool CaptureLighting(const std::string& path);
    bool CaptureDynamicGILifecycle(const std::string& path, bool switchMethods = false);
    bool CaptureGIContracts(const std::string& path);
    bool CaptureStaticGI(const std::string& path, uint32_t& fallbackFlags);
    bool CaptureGITraversal(const std::string& path);

    bool CaptureVoxelVolume(const std::string& path, uint32_t classMask = GI_ALL);
    bool CaptureVoxelClasses(const std::string& path);
    bool CaptureVoxelReference(const std::string& path);
    bool CaptureVoxelLifecycle(const std::string& path);
    bool CaptureVoxelGBuffer(const std::string& path);

    void Destroy();

private:
    bool CaptureVoxelOutput(const std::string& path, rc::VoxelizerBase& output);
    bool CaptureClassState(const std::string& path);
    bool CaptureClassQueries(const std::string& path);
    void OnResize(uint32_t width, uint32_t height);

    void RunSmokeStep(uint32_t frame);
    void PrepareLighting();
    void UpdateDynamicLight(float frameTime);
    bool GetGIMotionFixture(uint32_t& moving, Mat4& original) const;

    UniquePtr<sg::Camera> m_camera;

    UniquePtr<rc::RenderDevice> m_renderDevice;

    UniquePtr<sg::Scene> m_scene;
    UniquePtr<rc::RenderScene> m_renderScene;

    platform::GlfwWindowImpl* m_pWindow{nullptr};

    RHIViewport* m_pViewport{nullptr};

    UniquePtr<platform::Timer> m_timer;
    rc::LightId m_dynamicLight{0};
    Vec3 m_orbitCenter{0.0f, 1.0f, 0.0f};
    float m_orbitRadius{1.0f};
    float m_orbitSpeedDegrees{45.0f};
    double m_lightAngle{0.0};
    uint64_t m_motionFrame{0};
};
} // namespace zen
