#include "SceneRendererDemo.h"
#include "Graphics/RHI/RHIOptions.h"
#include "AssetLib/FastGLTFLoader.h"
#include "Platform/ConfigLoader.h"
#include "Graphics/RenderCore/V2/Renderer/RendererServer.h"
#include "Graphics/RenderCore/V2/Renderer/VoxelizerBase.h"
#include "Graphics/RenderCore/V2/ShaderProgram.h"
#include "Graphics/RenderCore/V2/RenderConfig.h"
#include "Graphics/RenderCore/V2/RenderScene.h"
#include "Memory/Memory.h"
#include "Platform/InputController.h"
#include <algorithm>
#include <charconv>
#include <chrono>
#include <fstream>
#include <iterator>
#include <string_view>

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

    const float aspect = windowConfig.aspect != 0.0f ? windowConfig.aspect : m_pWindow->GetAspect();
    m_pWindow->SetOnResize([this](uint32_t width, uint32_t height) { OnResize(width, height); });

    m_camera = sg::Camera::CreateUnique(Vec3{0.0f, 0.0f, 2.0f}, Vec3{0.0f, 0.0f, 0.0f}, aspect,
                                        type, sg::CameraProjectionType::ePerspective);
    m_camera->SetOnUpdate([&] {});

    m_timer = MakeUnique<platform::Timer>();
}

SceneRendererDemo::~SceneRendererDemo()
{
    delete m_pWindow;
}

void SceneRendererDemo::OnResize(uint32_t width, uint32_t height)
{
    // Minimized windows retain the last valid camera projection.
    if (width > 0 && height > 0)
    {
        m_camera->UpdateAspect(m_pWindow->GetAspect());
        m_renderDevice->ProcessViewportResize(width, height);
    }
}

bool SceneRendererDemo::Prepare(bool captureVoxels, uint32_t calibrationGridPercent)
{
    m_scene                                          = MakeUnique<sg::Scene>();
    UniquePtr<zen::asset::FastGLTFLoader> gltfLoader = MakeUnique<asset::FastGLTFLoader>();
    gltfLoader->LoadFromFile(platform::ConfigLoader::GetInstance().GetDefaultGLTFModelPath(),
                             m_scene.Get());
    float timeUsed = static_cast<float>(m_timer->Tick());
    LOGI("Scene {} loaded in {} seconds", m_scene->GetName(), timeUsed);

    rc::SceneData sceneData{};
    sceneData.pCamera     = m_camera.Get();
    sceneData.pScene      = m_scene.Get();
    sceneData.pVertices   = gltfLoader->GetVertices().data();
    sceneData.pIndices    = gltfLoader->GetIndices().data();
    sceneData.numVertices = gltfLoader->GetVertices().size();
    sceneData.numIndices  = gltfLoader->GetIndices().size();
    sceneData.envTextureName =
        platform::ConfigLoader::GetInstance().GetString("environment_texture", "papermill.ktx");
    m_renderScene = MakeUnique<rc::RenderScene>(m_renderDevice.Get(), sceneData);
    m_renderScene->Init();
    PrepareLighting();

    m_camera->SetupOnAABB(m_scene->GetAABB());
    Vec3 cameraPosition = m_camera->GetPos();
    if (platform::ConfigLoader::GetInstance().ReadVec3("camera_position", cameraPosition) &&
        glm::length(cameraPosition - m_scene->GetAABB().GetCenter()) > 1e-4f)
    {
        m_camera->SetPosition(cameraPosition);
    }
    m_renderDevice->GetRendererServer()->SetRenderScene(m_renderScene.Get());
    //    m_sceneRenderer->SetRenderScene(m_renderScene.Get());
    bool gridValid = true;
    if (captureVoxels && calibrationGridPercent != 0)
    {
        // Diagnostic only: normalization and camera setup above use the real bounds.
        // Inset by one voxel to cancel GetVoxelBounds' normal one-cell padding.
        const uint32_t resolution =
            m_renderDevice->GetRendererServer()->RequestVoxelizer()->GetVoxelTexResolution();
        const float halfExtent = 0.005f * static_cast<float>(calibrationGridPercent) *
            static_cast<float>(resolution - 2) / static_cast<float>(resolution);
        gridValid = m_renderScene->SetVoxelBounds(sg::AABB(Vec3(-halfExtent), Vec3(halfExtent)));
    }
    return gridValid &&
        (!captureVoxels ||
         m_renderDevice->GetRendererServer()->RequestVoxelizer()->EnableRadianceInputs());
}

void SceneRendererDemo::PrepareLighting()
{
    const platform::ConfigLoader& config = platform::ConfigLoader::GetInstance();
    bool animate                         = false;
    uint32_t animatedIndex               = 0;
    bool valid                           = config.ReadBool("dynamic_light.enabled", animate);
    valid &= config.ReadNumber("dynamic_light.index", animatedIndex);
    valid &= config.ReadVec3("dynamic_light.orbit_center", m_orbitCenter);
    valid &= config.ReadNumber("dynamic_light.orbit_radius", m_orbitRadius);
    valid &= config.ReadNumber("dynamic_light.angular_speed_degrees", m_orbitSpeedDegrees);
    valid = valid && m_orbitRadius >= 0.0f && std::abs(m_orbitSpeedDegrees) <= 3600.0f;
    const HeapVector<rc::ConfiguredLight> lights = rc::LoadSceneLights(config);
    for (const rc::ConfiguredLight& light : lights)
    {
        const rc::LightId id = m_renderScene->GetLights().Add(light.light);
        if (valid && animate && light.configIndex == animatedIndex &&
            light.light.type != rc::SceneLightType::eDirectional)
        {
            m_dynamicLight = id;
        }
    }
    if (!valid || (animate && m_dynamicLight == 0))
    {
        LOGW("Invalid dynamic_light configuration or missing point/spot light; animation disabled");
    }
    float intensity = 1.0f;
    float rotation  = 0.0f;
    bool enabled    = true;
    bool visible    = true;
    valid           = config.ReadNumber("environment_intensity", intensity);
    valid &= config.ReadNumber("environment_rotation_degrees", rotation);
    valid &= config.ReadBool("environment_lighting", enabled);
    valid &= config.ReadBool("skybox_visible", visible);
    if (!valid || !m_renderScene->SetEnvironmentLighting(intensity, rotation, enabled, visible))
    {
        LOGW("Invalid environment settings; using defaults");
    }
}

void SceneRendererDemo::UpdateDynamicLight(float frameTime)
{
    const rc::SceneLight* current = m_renderScene->GetLights().Find(m_dynamicLight);
    if (current != nullptr && frameTime > 0.0f)
    {
        m_lightAngle = std::fmod(m_lightAngle +
                                     glm::radians(static_cast<double>(m_orbitSpeedDegrees)) *
                                         static_cast<double>(frameTime),
                                 glm::two_pi<double>());
        rc::SceneLight light = *current;
        light.position       = m_orbitCenter +
            m_orbitRadius *
                Vec3(static_cast<float>(std::cos(m_lightAngle)), 0.0f,
                     static_cast<float>(std::sin(m_lightAngle)));
        if (!m_renderScene->GetLights().Update(m_dynamicLight, light))
        {
            LOGE("Dynamic light update rejected");
        }
    }
}

// Explicit diagnostic capture only; normal frames never wait for GPU completion.
bool SceneRendererDemo::CaptureFrame(const std::string& path)
{
    m_renderDevice->FlushRHIThread();
    m_renderDevice->WaitForIdle();
    RHITexture* source    = m_pViewport->GetColorBackBuffer();
    const uint32_t width  = source->GetWidth();
    const uint32_t height = source->GetHeight();
    rc::TextureFormat format;
    format.width  = width;
    format.height = height;
    format.depth  = 1;
    format.format = source->GetFormat();
    RHITexture* sampled =
        m_renderDevice->CreateTextureSampled(format, {.copyUsage = true}, "capture_color");
    RHIBufferCreateInfo info;
    info.size         = width * height * 4;
    info.allocateType = RHIBufferAllocateType::eGPU;
    info.usageFlags.SetFlags(RHIBufferUsageFlagBits::eStorageBuffer,
                             RHIBufferUsageFlagBits::eTransferSrcBuffer);
    info.tag          = "capture_packed";
    RHIBuffer* packed = m_renderDevice->CreateBuffer(info);
    info.allocateType = RHIBufferAllocateType::eCPURead;
    info.usageFlags   = 0;
    info.usageFlags.SetFlag(RHIBufferUsageFlagBits::eTransferDstBuffer);
    info.tag            = "capture_readback";
    RHIBuffer* readback = m_renderDevice->CreateBuffer(info);
    RHISampler* sampler = m_renderDevice->CreateSampler(RHISamplerCreateInfo::CreateLinearRepeat());
    bool succeeded =
        sampled != nullptr && packed != nullptr && readback != nullptr && sampler != nullptr;
    if (succeeded)
    {
        rc::RenderGraph graph("CaptureFrame");
        succeeded = graph.Begin();
        RHITextureCopyRegion region{};
        region.size = Vec3i(width, height, 1);
        region.srcSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);
        region.dstSubresources = region.srcSubresources;
        graph.AddTransferPass("CopyCaptureColor").CopyTexture(source, sampled, {&region, 1});
        rc::RDGComputePassDesc pack;
        pack.SetShaderProgramName("CaptureFrameSP");
        pack.BindSampledTexture("sourceColor", sampler, sampled->GetDefaultView());
        pack.BindStorageBuffer("packedColor", packed, rc::RDGContentGuarantee::eFullWrite);
        pack.SetPassTag("PackCaptureColor");
        graph.AddComputePass(std::move(pack))
            .RecordPassCommands([width, height](rc::RDGPassCmdEncoder& encoder) {
                encoder.Dispatch((width + 7) / 8, (height + 7) / 8, 1);
            });
        graph.AddTransferPass("ReadCaptureColor")
            .CopyBuffer(packed, readback, {0, 0, info.size})
            .NeverCull();
        succeeded = succeeded && graph.End() && m_renderDevice->ExecuteRenderGraph(graph);
        m_renderDevice->FlushRHIThread();
        m_renderDevice->WaitForIdle();
        if (succeeded)
        {
            const uint8_t* pixels = readback->Map();
            std::ofstream output(path, std::ios::binary);
            succeeded = pixels != nullptr && output.good();
            if (succeeded)
            {
                output << "P6\n" << width << " " << height << "\n255\n";
                for (uint32_t pixel = 0; pixel < width * height; ++pixel)
                {
                    output.write(reinterpret_cast<const char*>(pixels + pixel * 4), 3);
                }
                output.flush();
                succeeded = output.good();
            }
            if (pixels != nullptr)
            {
                readback->Unmap();
            }
        }
    }
    m_renderDevice->DestroyTexture(sampled);
    m_renderDevice->DestroyBuffer(packed);
    m_renderDevice->DestroyBuffer(readback);
    if (!succeeded)
    {
        LOGE("Frame capture failed: {}", path);
    }
    return succeeded;
}

void SceneRendererDemo::Destroy()
{
    rc::ShaderProgramManager::GetInstance().Destroy();
    m_renderDevice->Destroy();
}

void SceneRendererDemo::RunSmokeStep(uint32_t frame)
{
    if (frame == 4 || frame == 24 || frame == 36)
    {
        m_renderDevice->GetRendererServer()->SetRenderOption(rc::RenderOption::ePBR);
    }
    else if (frame == 28)
    {
        m_renderDevice->GetRendererServer()->SetRenderOption(rc::RenderOption::eVoxelize);
    }
    else if (frame == 8 || frame == 32 || frame == 40)
    {
        m_renderDevice->GetRendererServer()->SetRenderOption(rc::RenderOption::eVoxelGI);
    }
    else if (frame == 12)
    {
        glfwSetWindowSize(m_pWindow->GetHandle(), 960, 640);
    }
    else if (frame == 20)
    {
        glfwIconifyWindow(m_pWindow->GetHandle());
        glfwPollEvents();
        glfwRestoreWindow(m_pWindow->GetHandle());
    }
    if (frame == 16 || frame == 18)
    {
        const rc::SceneLight* current = m_renderScene->GetLights().Find(m_dynamicLight);
        if (current != nullptr)
        {
            rc::SceneLight light = *current;
            light.enabled        = frame == 18;
            m_renderScene->GetLights().Update(m_dynamicLight, light);
        }
    }
    if (frame == 34 && m_dynamicLight != 0)
    {
        m_renderScene->GetLights().Remove(m_dynamicLight);
        m_dynamicLight = 0;
    }
    // Consecutive updates exercise overwrite dependencies while previous draws are in flight.
    // Also update after returning from PBR and on both sides of resize/restore.
    if (frame == 0 || frame == 2 || frame == 3 || frame == 8 || frame == 11 || frame == 12 ||
        frame == 21 || frame == 28)
    {
        m_renderDevice->GetRendererServer()->RequestVoxelizer()->RequestVoxelization();
        m_renderDevice->GetRDGMetrics().RequestCapture();
        LOGI("Smoke voxel update: frame={}", frame);
    }
}

bool SceneRendererDemo::Run(uint32_t frameLimit,
                            bool smokeTest,
                            uint32_t initialMode,
                            const std::string& frameTimesPath,
                            bool fixedStep,
                            uint32_t giStartFrame,
                            bool motionFixture)
{
    HeapVector<double> frameTimes;
    if (!frameTimesPath.empty())
    {
        frameTimes.reserve(frameLimit);
    }
    if (smokeTest)
    {
        rc::RDGMetricsOptions options = m_renderDevice->GetRDGMetrics().GetOptions();
        options.includeTransferNodes  = true;
        options.maxNodeDetails        = 64;
        m_renderDevice->GetRDGMetrics().Configure(options);
    }
    m_renderDevice->GetRendererServer()->SetRenderOption(
        giStartFrame != 0 ? rc::RenderOption::ePBR :
                            static_cast<rc::RenderOption>(initialMode - 1));
    bool succeeded        = true;
    uint32_t moving       = 0;
    Mat4 motionOriginal(1);
    if (motionFixture)
    {
        succeeded = GetGIMotionFixture(moving, motionOriginal) &&
            m_renderScene->SetInstanceClass(moving, GI_DYNAMIC);
        if (!succeeded)
        {
            LOGE("GI motion requires the four-mesh M4 lifecycle fixture");
        }
    }
    uint32_t frames       = 0;
    double renderThreadUs = 0;
    while (succeeded && !m_pWindow->ShouldClose() && (frameLimit == 0 || frames < frameLimit) &&
           !m_renderDevice->AreSubmissionsBlocked())
    {
        const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        if (giStartFrame != 0 && frames == giStartFrame)
        {
            LOGI("GI cold-start trigger: frame={}", frames);
            m_renderDevice->GetRendererServer()->SetRenderOption(rc::RenderOption::eVoxelGI);
        }
        if (smokeTest)
        {
            RunSmokeStep(frames);
        }
        float frameTime = static_cast<float>(m_timer->Tick());
        m_pWindow->Update();

        if (m_pWindow->ShouldClose())
        {
            break;
        }

        const VkExtent2D extent = m_pWindow->GetExtent2D();
        if (extent.width == 0 || extent.height == 0)
        {
            // Wait for restore/close without submitting to an unavailable surface.
            if (smokeTest)
            {
                glfwRestoreWindow(m_pWindow->GetHandle());
                glfwWaitEventsTimeout(0.05);
            }
            else
            {
                glfwWaitEvents();
            }
            m_timer->Tick();
            continue;
        }

        m_camera->Update(frameTime);
        UpdateDynamicLight(smokeTest ? 1.0f / 30.0f : fixedStep ? 1.0f / 60.0f : frameTime);
        if (motionFixture)
        {
            const float seconds = static_cast<float>(m_motionFrame++) / 60.0f;
            const Vec3 offset(-0.12f * std::sin(seconds), 0.03f * std::cos(seconds),
                              0.025f * std::sin(seconds * 2));
            succeeded = m_renderScene->SetInstanceTransform(
                moving, glm::translate(Mat4(1), offset) * motionOriginal);
            if (!succeeded)
            {
                break;
            }
        }

        if (platform::KeyboardMouseInput::GetInstance().WasKeyPressedOnce(GLFW_KEY_1) |
            platform::KeyboardMouseInput::GetInstance().WasKeyPressedOnce(GLFW_KEY_KP_1))
        {
            m_renderDevice->GetRendererServer()->SetRenderOption(rc::RenderOption::eVoxelize);
        }

        if (platform::KeyboardMouseInput::GetInstance().WasKeyPressedOnce(GLFW_KEY_2) |
            platform::KeyboardMouseInput::GetInstance().WasKeyPressedOnce(GLFW_KEY_KP_2))
        {
            m_renderDevice->GetRendererServer()->SetRenderOption(rc::RenderOption::ePBR);
        }

        if (platform::KeyboardMouseInput::GetInstance().WasKeyPressedOnce(GLFW_KEY_3) |
            platform::KeyboardMouseInput::GetInstance().WasKeyPressedOnce(GLFW_KEY_KP_3))
        {
            m_renderDevice->GetRendererServer()->SetRenderOption(rc::RenderOption::eVoxelGI);
        }

        // Always consume R; geometry updates are meaningful in both voxel modes.
        if (platform::KeyboardMouseInput::GetInstance().WasKeyPressedOnce(GLFW_KEY_R) &&
            m_renderDevice->GetRendererServer()->GetRenderOption() != rc::RenderOption::ePBR)
        {
            m_renderDevice->GetRendererServer()->RequestVoxelizer()->RequestVoxelization();
            m_renderDevice->GetRDGMetrics().RequestCapture();
        }

        succeeded &= m_renderDevice->GetRendererServer()->DispatchRenderWorkloads();
        if (smokeTest)
        {
            LOGI("Smoke lighting: frame={} mode={} geometry_revision={} lighting_revision={}",
                 frames,
                 static_cast<uint32_t>(m_renderDevice->GetRendererServer()->GetRenderOption()) + 1,
                 m_renderDevice->GetRendererServer()->RequestVoxelizer()->GetGeometryRevision(),
                 m_renderScene->GetLights().GetRevision());
        }
        m_renderDevice->NextFrame();
        ++frames;
        const double elapsed =
            std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start)
                .count();
        renderThreadUs += elapsed;
        if (!frameTimesPath.empty())
        {
            frameTimes.push_back(elapsed);
        }
    }
    m_renderDevice->FlushRHIThread();
    if (smokeTest)
    {
        LOGI("Smoke queue submissions: graphics={} compute={} transfer={}",
             GDynamicRHI->GetLastSubmittedSerial(RHICommandContextType::eGraphics),
             GDynamicRHI->GetLastSubmittedSerial(RHICommandContextType::eAsyncCompute),
             GDynamicRHI->GetLastSubmittedSerial(RHICommandContextType::eTransfer));
    }
    const RHIThreadMetrics metrics = m_renderDevice->GetRHIThreadMetrics();
    LOGI(
        "Render threads: mode={} frames={} render_wall_us={} rhi_cpu_us={} queue_wait_us={} batches={} peak_pending={}",
        GetRHIThread().IsThreaded() ? "threaded" : "inline", frames, renderThreadUs,
        metrics.executionCPUUs, metrics.queueWaitUs, metrics.completedBatches,
        metrics.peakPendingBatches);
    if (!frameTimesPath.empty())
    {
        std::ofstream output(frameTimesPath);
        output << "frame,cpu_frame_ms\n";
        for (size_t frame = 0; frame < frameTimes.size(); ++frame)
        {
            output << frame << ',' << frameTimes[frame] / 1000.0 << '\n';
        }
        output.flush();
        if (!output.good())
        {
            LOGE("Failed to write frame times: {}", frameTimesPath);
            succeeded = false;
        }
    }
    return succeeded && !m_renderDevice->AreSubmissionsBlocked();
}

} // namespace zen

namespace
{
struct DemoOptions
{
    uint32_t frames{0};
    uint32_t warmup{0};
    uint32_t initialMode{1};
    uint32_t giStartFrame{0};
    bool smokeTest{false};
    bool disableRT{false};
    bool disableValidation{false};
    bool gpuMarkers{false};
    bool gpuMemoryStats{false};
    bool motionFixture{false};
    bool fixedStep{false};
    std::string frameTimesPath;
    std::string capturePath;
    std::string lightingCapturePath;
    std::string traversalCapturePath;
    std::string voxelCapturePath;
    bool dynamicGILifecycle{false};
    bool giMethodSwitching{false};
    bool giContracts{false};
    bool voxelReference{false};
    bool voxelLifecycle{false};
    bool voxelClasses{false};
    bool voxelGBuffer{false};
    uint32_t voxelGridPercent{0};
    uint32_t width{1280};
    uint32_t height{720};
};

bool ParseDemoOptions(int argc, char** arguments, DemoOptions& options)
{
    bool valid = true;
    for (int i = 1; i < argc && valid; ++i)
    {
        const std::string_view argument(arguments[i]);
        if (argument == "--rhi-thread=0" || argument == "--rhi-thread=1")
        {
            zen::rc::RenderConfig::GetInstance().rhiExecutionMode = argument.back() == '1' ?
                zen::RHIExecutionMode::eThreaded :
                zen::RHIExecutionMode::eInline;
        }
        else if (argument.starts_with("--async-compute="))
        {
            valid = zen::rc::ParseAsyncComputeOverride(
                argument, zen::rc::RenderConfig::GetInstance().asyncComputeMode);
        }
        else if (argument.starts_with("--mode="))
        {
            valid = argument == "--mode=1" || argument == "--mode=2" || argument == "--mode=3";
            if (valid)
            {
                options.initialMode = static_cast<uint32_t>(argument.back() - '0');
            }
        }
        else if (argument.starts_with("--capture="))
        {
            options.capturePath = argument.substr(10);
            valid               = !options.capturePath.empty();
        }
        else if (argument.starts_with("--frame-times="))
        {
            options.frameTimesPath = argument.substr(14);
            valid                  = !options.frameTimesPath.empty();
        }
        else if (argument == "--fixed-step")
        {
            options.fixedStep = true;
        }
        else if (argument.starts_with("--capture-voxels="))
        {
            options.voxelCapturePath = argument.substr(17);
            valid                    = !options.voxelCapturePath.empty();
        }
        else if (argument.starts_with("--capture-lighting="))
        {
            options.lightingCapturePath = argument.substr(19);
            valid                       = !options.lightingCapturePath.empty();
        }
        else if (argument.starts_with("--capture-traversal="))
        {
            options.traversalCapturePath = argument.substr(20);
            valid                        = !options.traversalCapturePath.empty();
        }
        else if (argument == "--dynamic-gi-lifecycle")
        {
            options.dynamicGILifecycle = true;
        }
        else if (argument == "--gi-method-switching")
        {
            options.dynamicGILifecycle = true;
            options.giMethodSwitching  = true;
        }
        else if (argument == "--gi-contracts")
        {
            options.giContracts = true;
        }
        else if (argument == "--voxel-reference")
        {
            options.voxelReference = true;
        }
        else if (argument == "--voxel-lifecycle")
        {
            options.voxelLifecycle = true;
        }
        else if (argument == "--voxel-classes")
        {
            options.voxelClasses = true;
        }
        else if (argument == "--voxel-gbuffer")
        {
            options.voxelGBuffer = true;
        }
        else if (argument.starts_with("--voxel-grid-percent="))
        {
            const std::string_view value        = argument.substr(21);
            const std::from_chars_result parsed = std::from_chars(
                value.data(), value.data() + value.size(), options.voxelGridPercent);
            valid = parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() &&
                options.voxelGridPercent != 0;
        }
        else if (argument == "--disable-rt")
        {
            options.disableRT = true;
        }
        else if (argument == "--disable-validation")
        {
            options.disableValidation = true;
        }
        else if (argument == "--gpu-markers")
        {
            options.gpuMarkers = true;
        }
        else if (argument == "--gpu-memory-stats")
        {
            options.gpuMemoryStats = true;
        }
        else if (argument == "--gi-motion-fixture")
        {
            options.motionFixture = true;
        }
        else if (argument.starts_with("--gi-start-frame="))
        {
            const std::string_view value = argument.substr(17);
            const std::from_chars_result parsed =
                std::from_chars(value.data(), value.data() + value.size(), options.giStartFrame);
            valid = parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size();
        }
        else if (argument == "--smoke-test")
        {
            options.smokeTest = true;
        }
        else if (argument.starts_with("--gbuffer-size=") || argument.starts_with("--width=") ||
                 argument.starts_with("--height="))
        {
            const size_t offset          = argument.find('=') + 1;
            const std::string_view value = argument.substr(offset);
            uint32_t dimension           = 0;
            const std::from_chars_result parsed =
                std::from_chars(value.data(), value.data() + value.size(), dimension);
            valid = parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() &&
                dimension > 0 && dimension <= 4096;
            if (valid)
            {
                if (argument.starts_with("--gbuffer-size="))
                {
                    zen::rc::RenderConfig::GetInstance().offScreenFbSize = dimension;
                }
                else if (argument.starts_with("--width="))
                {
                    options.width = dimension;
                }
                else
                {
                    options.height = dimension;
                }
            }
        }
        else if (argument.starts_with("--frames=") || argument.starts_with("--warmup="))
        {
            const std::string_view value = argument.substr(9);
            uint32_t& count = argument.starts_with("--frames=") ? options.frames : options.warmup;
            const std::from_chars_result parsed =
                std::from_chars(value.data(), value.data() + value.size(), count);
            valid = parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size();
        }
        else
        {
            valid = false;
        }
    }
    if (options.smokeTest && options.frames == 0)
    {
        options.frames = 48;
    }
    return valid && (options.frameTimesPath.empty() || options.frames != 0) &&
        (options.giStartFrame == 0 ||
         (options.initialMode == 3 && options.warmup == 0 && !options.smokeTest &&
          (options.frames == 0 || options.giStartFrame < options.frames))) &&
        (!(options.dynamicGILifecycle || options.giContracts) ||
         !options.lightingCapturePath.empty()) &&
        (!(options.voxelReference || options.voxelLifecycle || options.voxelClasses ||
           options.voxelGBuffer || options.voxelGridPercent != 0) ||
         !options.voxelCapturePath.empty());
}
} // namespace

int main(int argc, char** pArgv)
{
    using namespace zen;
    DemoOptions options;
    int result = 1;
    if (ParseDemoOptions(argc, pArgv, options))
    {
        RHIOptions::GetInstance().SetRayTracingEnabled(!options.disableRT);
        RHIOptions::GetInstance().SetValidationEnabled(!options.disableValidation);
        RHIOptions::GetInstance().SetGPUProfilerMarkers(options.gpuMarkers);
        RHIOptions::GetInstance().SetGPUMemoryStats(options.gpuMemoryStats);
        platform::WindowConfig windowConfig{"scene_renderer_demo", true, options.width,
                                            options.height};
        SceneRendererDemo* pDemo =
            new SceneRendererDemo(windowConfig, sg::CameraType::eFirstPerson);
        const bool prepared =
            pDemo->Prepare(!options.voxelCapturePath.empty(), options.voxelGridPercent);
        const bool warmed = prepared &&
            (options.warmup == 0 ||
             pDemo->Run(options.warmup, false, options.initialMode, {}, options.fixedStep, 0,
                        options.motionFixture));
        result = warmed &&
                pDemo->Run(options.frames, options.smokeTest, options.initialMode,
                           options.frameTimesPath, options.fixedStep, options.giStartFrame,
                           options.motionFixture) ?
            0 :
            1;
        if (result == 0 && !options.lightingCapturePath.empty())
        {
            result =
                (options.giContracts ? pDemo->CaptureGIContracts(options.lightingCapturePath) :
                     options.dynamicGILifecycle ?
                                       pDemo->CaptureDynamicGILifecycle(options.lightingCapturePath,
                                                                        options.giMethodSwitching) :
                                       pDemo->CaptureLighting(options.lightingCapturePath)) ?
                0 :
                1;
        }
        if (result == 0 && !options.traversalCapturePath.empty())
        {
            result = pDemo->CaptureGITraversal(options.traversalCapturePath) ? 0 : 1;
        }
        if (result == 0 && !options.capturePath.empty())
        {
            result = pDemo->CaptureFrame(options.capturePath) ? 0 : 1;
        }
        if (result == 0 && !options.voxelCapturePath.empty())
        {
            result = pDemo->CaptureVoxelVolume(options.voxelCapturePath) ? 0 : 1;
        }
        if (result == 0 && options.voxelReference)
        {
            result = pDemo->CaptureVoxelReference(options.voxelCapturePath) ? 0 : 1;
        }
        if (result == 0 && options.voxelLifecycle)
        {
            result = pDemo->CaptureVoxelLifecycle(options.voxelCapturePath) ? 0 : 1;
        }
        if (result == 0 && options.voxelClasses)
        {
            result = pDemo->CaptureVoxelClasses(options.voxelCapturePath) ? 0 : 1;
        }
        if (result == 0 && options.voxelGBuffer)
        {
            result = pDemo->CaptureVoxelGBuffer(options.voxelCapturePath) ? 0 : 1;
        }
        pDemo->Destroy();
        delete pDemo;
    }
    else
    {
        LOGE(
            "Usage: scene_renderer_demo [--rhi-thread=0|1] [--async-compute=0|1] [--frames=N] [--warmup=N] [--frame-times=path.csv] [--fixed-step] [--mode=1|2|3] [--smoke-test] [--disable-rt] [--disable-validation] [--gpu-markers] [--gpu-memory-stats] [--gi-start-frame=N] [--gi-motion-fixture] [--capture=frame.ppm] [--capture-lighting=prefix] [--capture-traversal=prefix] [--dynamic-gi-lifecycle] [--gi-method-switching] [--gi-contracts] [--capture-voxels=prefix] [--voxel-reference] [--voxel-lifecycle] [--voxel-classes] [--voxel-gbuffer] [--voxel-grid-percent=N] [--gbuffer-size=N] [--width=N] [--height=N]");
    }
    return result;
}
