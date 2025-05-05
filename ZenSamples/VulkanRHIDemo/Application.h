#pragma once
#include "Graphics/RenderCore/V2/RenderDevice.h"
#include "Graphics/RenderCore/V2/RenderGraph.h"
#include "Graphics/VulkanRHI/VulkanRHI.h"
#include "Platform/Timer.h"
#include "SceneGraph/Camera.h"

using namespace zen;
using namespace zen::rhi;

namespace zen::rc
{
class RenderDevice;
}

class Application
{
public:
    explicit Application(const platform::WindowConfig& windowConfig,
                         sg::CameraType type = sg::CameraType::eFirstPerson);

    virtual void Prepare();

    virtual void Run();

    virtual void Destroy();

    virtual ~Application()
    {
        delete m_window;
    }

protected:
    virtual void LoadResources();

    virtual void BuildRenderGraph() = 0;

    rc::RenderDevice* m_renderDevice{nullptr};
    UniquePtr<rc::RenderGraph> m_rdg;
    platform::GlfwWindowImpl* m_window{nullptr};
    RHIViewport* m_viewport{nullptr};

    BufferHandle m_cameraUBO;
    UniquePtr<sg::Camera> m_camera;

    UniquePtr<platform::Timer> m_timer;
    // Defines a frame rate independent timer value clamped from -1.0...1.0
    // For use in animations, rotations, etc.
    float m_animationTimer{0.0f};
    float m_animationSpeed{0.25f};
};

#define ZEN_RHI_DEMO_MAIN(AppClass, AppName)                            \
    int main(int argc, char** argv)                                     \
    {                                                                   \
        platform::WindowConfig windowConfig{AppName, false, 1280, 720}; \
        Application* app = new AppClass(windowConfig);                  \
        app->Prepare();                                                 \
        app->Run();                                                     \
        app->Destroy();                                                 \
        delete app;                                                     \
    }
