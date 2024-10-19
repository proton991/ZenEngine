#include "Application.h"

Application::Application(const platform::WindowConfig& windowConfig)
{
    m_renderDevice = new rc::RenderDevice(GraphicsAPIType::eVulkan, 3);
    m_renderDevice->Init();

    m_window = new platform::GlfwWindowImpl(windowConfig);
    m_window->SetOnResize([&](uint32_t width, uint32_t height) {
        m_renderDevice->ResizeViewport(&m_viewport, m_window, width, height);
        BuildRenderGraph();
        // recreate camera
        m_camera = sys::Camera::CreateUnique(Vec3{0.0f, 0.0f, -0.1f}, Vec3{0.0f, 0.0f, 0.0f},
                                             m_window->GetAspect());
    });
    m_viewport =
        m_renderDevice->CreateViewport(m_window, windowConfig.width, windowConfig.height, true);

    m_camera = sys::Camera::CreateUnique(Vec3{0.0f, 0.0f, -0.1f}, Vec3{0.0f, 0.0f, 0.0f},
                                         m_window->GetAspect());
    m_camera->SetOnUpdate([&] {
        m_renderDevice->UpdateBuffer(m_cameraUBO, sizeof(sys::CameraUniformData),
                                     m_camera->GetUniformData());
    });

    m_timer = MakeUnique<platform::Timer>();
}

void Application::LoadResources() {}

void Application::Prepare() {}

void Application::Destroy()
{
    m_rdg->Destroy();
    m_renderDevice->Destroy();
    delete m_renderDevice;
}

void Application::Run()
{
    while (!m_window->ShouldClose())
    {
        m_window->Update();
        m_camera->Update(static_cast<float>(m_timer->Tick()));
        m_renderDevice->ExecuteFrame(m_viewport, m_rdg.Get());
        m_renderDevice->NextFrame();
    }
}