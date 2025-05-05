#include "Application.h"
#include "Graphics/RenderCore/V2/ShaderProgram.h"

Application::Application(const platform::WindowConfig& windowConfig, sg::CameraType type)
{
    m_window = new platform::GlfwWindowImpl(windowConfig);

    m_renderDevice = new rc::RenderDevice(GraphicsAPIType::eVulkan, 3);

    m_viewport =
        m_renderDevice->CreateViewport(m_window, windowConfig.width, windowConfig.height, true);

    rc::ShaderProgramManager::GetInstance().BuildShaderPrograms(m_renderDevice);

    m_renderDevice->Init(m_viewport);

    m_camera = sg::Camera::CreateUnique(Vec3{0.0f, 0.0f, -0.1f}, Vec3{0.0f, 0.0f, 0.0f},
                                        m_window->GetAspect(), type);
    m_camera->SetOnUpdate([&] {
        m_renderDevice->UpdateBuffer(m_cameraUBO, sizeof(sg::CameraUniformData),
                                     m_camera->GetUniformData());
    });

    m_timer = MakeUnique<platform::Timer>();
}

void Application::LoadResources()
{
    m_cameraUBO = m_renderDevice->CreateUniformBuffer(sizeof(sg::CameraUniformData),
                                                      m_camera->GetUniformData());
}

void Application::Prepare() {}

void Application::Destroy()
{
    rc::ShaderProgramManager::GetInstance().Destroy();
    m_renderDevice->Destroy();
    delete m_renderDevice;
}

void Application::Run() {}