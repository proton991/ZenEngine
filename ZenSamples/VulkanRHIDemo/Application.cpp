#include "Application.h"

Application::Application(const platform::WindowConfig& windowConfig, sys::CameraType type)
{
    m_renderDevice = new rc::RenderDevice(GraphicsAPIType::eVulkan, 3);
    m_renderDevice->Init();

    m_window = new platform::GlfwWindowImpl(windowConfig);

    m_viewport =
        m_renderDevice->CreateViewport(m_window, windowConfig.width, windowConfig.height, true);

    m_camera = sys::Camera::CreateUnique(Vec3{0.0f, 0.0f, -0.1f}, Vec3{0.0f, 0.0f, 0.0f},
                                         m_window->GetAspect(), type);
    m_camera->SetOnUpdate([&] {
        m_renderDevice->UpdateBuffer(m_cameraUBO, sizeof(sys::CameraUniformData),
                                     m_camera->GetUniformData());
    });

    m_timer = MakeUnique<platform::Timer>();
}

void Application::LoadResources()
{
    m_cameraUBO = m_renderDevice->CreateUniformBuffer(sizeof(sys::CameraUniformData),
                                                      m_camera->GetUniformData());
}

void Application::Prepare() {}

void Application::Destroy()
{
    m_rdg->Destroy();
    m_renderDevice->Destroy();
    delete m_renderDevice;
}

void Application::Run() {}