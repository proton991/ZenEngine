#include "SceneRendererDemo.h"
#include "Graphics/RenderCore/V2/RenderConfig.h"
#include "AssetLib/FastGLTFLoader.h"
#include "Platform/ConfigLoader.h"


SceneRendererDemo::SceneRendererDemo(const platform::WindowConfig& windowConfig,
                                     sys::CameraType type)
{
    m_window = new platform::GlfwWindowImpl(windowConfig);

    m_renderDevice = MakeUnique<rc::RenderDevice>(GraphicsAPIType::eVulkan,
                                                  rc::RenderConfig::GetInstance().numFrames);

    m_viewport =
        m_renderDevice->CreateViewport(m_window, windowConfig.width, windowConfig.height, true);

    m_renderDevice->Init(m_viewport);

    m_sceneRenderer = MakeUnique<rc::SceneRenderer>(m_renderDevice.Get(), m_viewport);


    float aspect = windowConfig.aspect != 0.0f ? windowConfig.aspect : m_window->GetAspect();
    m_window->SetOnResize([&](uint32_t width, uint32_t height) {
        m_camera->UpdateAspect(m_window->GetAspect());
        m_sceneRenderer->OnResize(width, height);
    });

    m_camera =
        sys::Camera::CreateUnique(Vec3{0.0f, 0.0f, 2.0f}, Vec3{0.0f, 0.0f, 0.0f}, aspect, type);
    m_camera->SetOnUpdate([&] {});

    m_timer = MakeUnique<platform::Timer>();
}

void SceneRendererDemo::Prepare()
{
    m_scene         = MakeUnique<sg::Scene>();
    auto gltfLoader = MakeUnique<asset::FastGLTFLoader>();
    gltfLoader->LoadFromFile(platform::ConfigLoader::GetInstance().GetDefaultGLTFModelPath(),
                             m_scene.Get());
    auto timeUsed = static_cast<float>(m_timer->Tick());
    LOGI("Scene {} loaded in {} seconds", m_scene->GetName(), timeUsed);

    rc::SceneData sceneData{};
    sceneData.camera      = m_camera.Get();
    sceneData.scene       = m_scene.Get();
    sceneData.vertices    = gltfLoader->GetVertices().data();
    sceneData.indices     = gltfLoader->GetIndices().data();
    sceneData.numVertices = gltfLoader->GetVertices().size();
    sceneData.numIndices  = gltfLoader->GetIndices().size();
    // light info
    // todo: support more flex configuration throught config file
    sceneData.lightPositions[0] = glm::vec4(-1.0f, 1.0f, -1.0f, 1.0f); // Top-left
    sceneData.lightPositions[1] = glm::vec4(1.0f, 1.0f, -1.0f, 1.0f);  // Top-right
    sceneData.lightPositions[2] = glm::vec4(-1.0f, 1.0f, 1.0f, 1.0f);  // Bottom-left
    sceneData.lightPositions[3] = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);   // Bottom-right

    sceneData.lightColors[0] = Vec4(1.0f, 1.0f, 1.0f, 0.0f);
    sceneData.lightColors[1] = Vec4(1.0f, 1.0f, 1.0f, 0.0f);
    sceneData.lightColors[2] = Vec4(1.0f, 1.0f, 1.0f, 0.0f);
    sceneData.lightColors[3] = Vec4(1.0f, 1.0f, 1.0f, 0.0f);

    sceneData.lightIntensities[0] = Vec4(5.0f);
    sceneData.lightIntensities[1] = Vec4(5.0f);
    sceneData.lightIntensities[2] = Vec4(5.0f);
    sceneData.lightIntensities[3] = Vec4(5.0f);

    m_sceneRenderer->SetScene(sceneData);
    m_sceneRenderer->Bake();
}

void SceneRendererDemo::Destroy()
{
    m_sceneRenderer->Destroy();

    m_renderDevice->Destroy();
}

void SceneRendererDemo::Run()
{

    while (!m_window->ShouldClose())
    {
        auto frameTime = static_cast<float>(m_timer->Tick());
        m_animationTimer += frameTime * m_animationSpeed;
        if (m_animationTimer > 1.0f)
        {
            m_animationTimer -= 1.0f;
        }

        m_window->Update();

        m_camera->Update(frameTime);

        m_sceneRenderer->DrawScene();

        m_renderDevice->NextFrame();
    }
}

int main(int argc, char** argv)
{
    platform::WindowConfig windowConfig{"scene_renderer_demo", true, 1280, 720};

    SceneRendererDemo* demo = new SceneRendererDemo(windowConfig, sys::CameraType::eFirstPerson);

    demo->Prepare();

    demo->Run();

    demo->Destroy();

    delete demo;
}
