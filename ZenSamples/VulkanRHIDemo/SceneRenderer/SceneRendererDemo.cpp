#include "SceneRendererDemo.h"

#include "AssetLib/GLTFLoader.h"


SceneRendererDemo::SceneRendererDemo(const platform::WindowConfig& windowConfig,
                                     sys::CameraType type)
{
    m_renderDevice = MakeUnique<rc::RenderDevice>(GraphicsAPIType::eVulkan, 3);
    m_renderDevice->Init();

    m_window = new platform::GlfwWindowImpl(windowConfig);
    m_viewport =
        m_renderDevice->CreateViewport(m_window, windowConfig.width, windowConfig.height, true);

    m_sceneRenderer = MakeUnique<rc::SceneRenderer>(m_renderDevice.Get(), m_viewport);


    float aspect = windowConfig.aspect != 0.0f ? windowConfig.aspect : m_window->GetAspect();
    m_window->SetOnResize([&](uint32_t width, uint32_t height) {
        m_renderDevice->ResizeViewport(&m_viewport, m_window, width, height);
        m_sceneRenderer->OnViewportResized();
        // recreate camera
        m_camera =
            sys::Camera::CreateUnique(Vec3{0.0f, 0.0f, -0.1f}, Vec3{0.0f, 0.0f, 0.0f}, aspect);
    });


    m_camera =
        sys::Camera::CreateUnique(Vec3{0.0f, 0.0f, 2.0f}, Vec3{0.0f, 0.0f, 0.0f}, aspect, type);
    m_camera->SetOnUpdate([&] {

    });

    m_timer = MakeUnique<platform::Timer>();
}

void SceneRendererDemo::Prepare()
{
    m_scene         = MakeUnique<sg::Scene>();
    auto gltfLoader = MakeUnique<gltf::GltfLoader>();
    gltfLoader->LoadFromFile(m_scenePath, m_scene.Get());

    rc::SceneData sceneData{};
    sceneData.camera      = m_camera.Get();
    sceneData.scene       = m_scene.Get();
    sceneData.vertices    = gltfLoader->GetVertices().data();
    sceneData.indices     = gltfLoader->GetIndices().data();
    sceneData.numVertices = gltfLoader->GetVertices().size();
    sceneData.numIndices  = gltfLoader->GetIndices().size();
    // light info
    sceneData.lightPosition  = Vec4(1.0f, 1.0f, 1.0f, 0.0f);
    sceneData.lightColor     = Vec4(1.0f, 1.0f, 1.0f, 0.0f);
    sceneData.lightIntensity = Vec4(1.0f, 1.0f, 1.0f, 0.0f);

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

        m_renderDevice->BeginDrawingViewport(m_viewport);

        m_sceneRenderer->DrawScene();

        m_renderDevice->EndDrawingViewport(m_viewport);

        m_renderDevice->NextFrame();
    }
}

int main(int argc, char** argv)
{
    platform::WindowConfig windowConfig{"scene_renderer_demo", true, 1280, 720};

    SceneRendererDemo* demo = new SceneRendererDemo(windowConfig);

    demo->Prepare();

    demo->Run();

    demo->Destroy();

    delete demo;
}
