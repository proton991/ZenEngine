#pragma once
#include "Platform/Timer.h"
#include "SceneGraph/Camera.h"
#include "Graphics/RenderCore/V2/RenderScene.h"
#include "Platform/GlfwWindow.h"

using namespace zen;

class SceneRendererDemo
{
public:
    SceneRendererDemo(const platform::WindowConfig& windowConfig, sg::CameraType type);

    ~SceneRendererDemo();

    void Prepare();

    void Run();

    void Destroy();

private:
    UniquePtr<sg::Camera> m_camera;

    UniquePtr<rc::RenderDevice> m_renderDevice;

    UniquePtr<sg::Scene> m_scene;
    UniquePtr<rc::RenderScene> m_renderScene;

    platform::GlfwWindowImpl* m_window{nullptr};

    rhi::RHIViewport* m_viewport{nullptr};

    UniquePtr<platform::Timer> m_timer;
    // Defines a frame rate independent timer value clamped from -1.0...1.0
    // For use in animations, rotations, etc.
    float m_animationTimer{0.0f};
    float m_animationSpeed{0.25f};
};