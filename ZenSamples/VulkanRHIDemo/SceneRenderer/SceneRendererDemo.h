#pragma once
#include "Platform/ConfigLoader.h"
#include "Platform/Timer.h"
#include "Systems/Camera.h"
#include "Graphics/RenderCore/V2/SceneRenderer.h"

using namespace zen;

class SceneRendererDemo
{
public:
    SceneRendererDemo(const platform::WindowConfig& windowConfig, sys::CameraType type);

    void Prepare();

    void Run();

    void Destroy();

private:
    UniquePtr<sys::Camera> m_camera;

    UniquePtr<rc::SceneRenderer> m_sceneRenderer;

    UniquePtr<rc::RenderDevice> m_renderDevice;

    UniquePtr<sg::Scene> m_scene;

    platform::GlfwWindowImpl* m_window{nullptr};

    rhi::RHIViewport* m_viewport{nullptr};

    UniquePtr<platform::Timer> m_timer;
    // Defines a frame rate independent timer value clamped from -1.0...1.0
    // For use in animations, rotations, etc.
    float m_animationTimer{0.0f};
    float m_animationSpeed{0.25f};

    platform::ConfigLoader m_configLoader;
};