#include "SceneRendererDemo.h"
#include "AssetLib/FastGLTFLoader.h"
#include "Platform/ConfigLoader.h"
#include "Graphics/RenderCore/V2/Renderer/RendererServer.h"
#include "Graphics/RenderCore/V2/ShaderProgram.h"
#include "Graphics/RenderCore/V2/RenderConfig.h"
#include "Graphics/RenderCore/V2/RenderScene.h"
#include "Memory/Memory.h"
#include "Platform/InputController.h"

// #if defined(ZEN_WIN32) && defined(ZEN_DEBUG)
// #    define _CRTDBG_MAP_ALLOC
// #    include <crtdbg.h>
// static int CrtReportHook(int reportType, char* message, int* returnValue)
// {
//     fprintf(stderr, "%s", message);
//     // LOGE("{}", message);
//     return FALSE; // let CRT continue processing
// }
// #endif

namespace zen
{
SceneRendererDemo::SceneRendererDemo(const platform::WindowConfig& windowConfig,
                                     sg::CameraType type)
{
    m_pWindow = new platform::GlfwWindowImpl(windowConfig);

    m_renderDevice = MakeUnique<rc::RenderDevice>(RHIAPIType::eVulkan,
                                                  rc::RenderConfig::GetInstance().numFrames);

    m_pViewport =
        m_renderDevice->CreateViewport(m_pWindow, windowConfig.width, windowConfig.height, true);

    rc::ShaderProgramManager::GetInstance().BuildShaderPrograms(m_renderDevice.Get());

    m_renderDevice->Init(m_pViewport);

    float aspect = windowConfig.aspect != 0.0f ? windowConfig.aspect : m_pWindow->GetAspect();
    m_pWindow->SetOnResize([&](uint32_t width, uint32_t height) {
        m_camera->UpdateAspect(m_pWindow->GetAspect());
        m_renderDevice->ProcessViewportResize(width, height);
    });

    m_camera = sg::Camera::CreateUnique(Vec3{0.0f, 0.0f, 2.0f}, Vec3{0.0f, 0.0f, 0.0f}, aspect,
                                        type, sg::CameraProjectionType::ePerspective);
    m_camera->SetOnUpdate([&] {});

    m_timer = MakeUnique<platform::Timer>();
}

SceneRendererDemo::~SceneRendererDemo()
{
    delete m_pWindow;
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
    sceneData.pCamera      = m_camera.Get();
    sceneData.pScene       = m_scene.Get();
    sceneData.pVertices    = gltfLoader->GetVertices().data();
    sceneData.pIndices     = gltfLoader->GetIndices().data();
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

    m_renderScene = MakeUnique<rc::RenderScene>(m_renderDevice.Get(), sceneData);
    m_renderScene->Init();

    m_camera->SetupOnAABB(m_scene->GetAABB());
    m_renderDevice->GetRendererServer()->SetRenderScene(m_renderScene.Get());
    //    m_sceneRenderer->SetRenderScene(m_renderScene.Get());
}

void SceneRendererDemo::Destroy()
{
    rc::ShaderProgramManager::GetInstance().Destroy();
    m_renderDevice->Destroy();
}

void SceneRendererDemo::Run()
{

    while (!m_pWindow->ShouldClose())
    {
        auto frameTime = static_cast<float>(m_timer->Tick());
        m_animationTimer += frameTime * m_animationSpeed;
        if (m_animationTimer > 1.0f)
        {
            m_animationTimer -= 1.0f;
        }

        m_pWindow->Update();

        m_camera->Update(frameTime);

        m_renderDevice->GetRendererServer()->DispatchRenderWorkloads();

        if (platform::KeyboardMouseInput::GetInstance().IsKeyPressed(GLFW_KEY_1))
        {
            m_renderDevice->GetRendererServer()->SetRenderOption(rc::RenderOption::eVoxelize);
        }

        if (platform::KeyboardMouseInput::GetInstance().IsKeyPressed(GLFW_KEY_2))
        {
            m_renderDevice->GetRendererServer()->SetRenderOption(rc::RenderOption::ePBR);
        }

        m_renderDevice->NextFrame();
    }
}

} // namespace zen

int main(int argc, char** pArgv)
{
    using namespace zen;

    platform::WindowConfig windowConfig{"scene_renderer_demo", true, 1280, 720};

    SceneRendererDemo* pDemo = new SceneRendererDemo(windowConfig, sg::CameraType::eFirstPerson);

    pDemo->Prepare();

    pDemo->Run();

    pDemo->Destroy();

    delete pDemo;
}