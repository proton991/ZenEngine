#include "AssetTools/SceneTools.h"
#include "AssetLib/SceneRuntimeBuilder.h"
#if defined(ZEN_WIN32)
#    include <Windows.h>
#endif
#include "SceneRendererDemo.h"
#include "SceneRendererWindowTest.h"
#include "Graphics/RHI/RHIOptions.h"
#include "AssetLib/SceneLoader.h"
#include "Platform/ConfigLoader.h"
#include "Graphics/RenderCore/V2/Renderer/RendererServer.h"
#include "Graphics/RenderCore/V2/Renderer/DeferredLightingRenderer.h"
#include "Graphics/RenderCore/V2/Renderer/VoxelizerBase.h"
#include "Graphics/RenderCore/V2/ShaderProgram.h"
#include "Graphics/RenderCore/V2/RenderConfig.h"
#include "Graphics/RenderCore/V2/RenderScene.h"
#include "Memory/Memory.h"
#include "Platform/InputController.h"
#if defined(ZEN_RUNTIME_UI)
#    include "RuntimeUI/RuntimeDebugUI.h"
#endif
#include <algorithm>
#include <charconv>
#include <chrono>
#include <fstream>
#include <filesystem>
#include <iterator>
#include <optional>
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
namespace
{
// Scripted runs fail rather than wait forever for a GI scene BVH that never finishes.
constexpr double kGIPreparationLimitSeconds = 600.0;

void LogSceneUploadGraph(const rc::RDGMetricsSnapshot& snapshot)
{
    // The execution ID is unique within each transfer/frame stream. It joins this
    // synchronous publication range to the profiler's deferred GPU pass records.
    LOGI("SCENE_UPLOAD_GRAPH {{\"execution\":{},\"transfer_only\":{},\"node_count\":{},\"omitted_nodes\":{}}}",
         snapshot.execution, snapshot.transferOnly, snapshot.nodeCount, snapshot.omittedNodes);
}

void LogSceneUploadStatistics(const rc::RenderDevice&          device,
                              const rc::StagingPoolStatistics& before,
                              uint64_t                         stagedBefore,
                              uint64_t                         transferSerialBefore,
                              bool                             published)
{
    const rc::StagingPoolStatistics statistics = device.GetStagingPoolStatistics();
    LOGI("SCENE_UPLOAD_METRICS {{\"published\":{},\"staged_bytes\":{},\"baseline_capacity_bytes\":{},"
         "\"baseline_occupied_bytes\":{},\"capacity_bytes\":{},\"peak_capacity_bytes\":{},"
         "\"occupied_bytes\":{},\"peak_occupied_bytes\":{},\"native_allocation_available\":{},"
         "\"native_allocation_bytes\":{},\"peak_native_allocation_bytes\":{},\"block_count\":{},"
         "\"peak_block_count\":{},\"allocation_flush_requests\":{},\"allocation_failures\":{},"
         "\"pool_wait_calls\":{},\"pool_wait_failures\":{},\"pool_wait_cpu_ns\":{}}}",
         published, device.GetStagedUploadBytes() - stagedBefore, before.capacityBytes, before.occupiedBytes,
         statistics.capacityBytes, statistics.peakCapacityBytes, statistics.occupiedBytes, statistics.peakOccupiedBytes,
         statistics.allocationBytesAvailable, statistics.allocationBytes, statistics.peakAllocationBytes, statistics.blockCount,
         statistics.peakBlockCount, statistics.allocationFlushRequests, statistics.allocationFailures, statistics.waitCalls,
         statistics.waitFailures, statistics.waitCPUTimeNS);
    const RHIGPUMemoryStats native = GDynamicRHI->GetGPUMemoryStats();
    LOGI("SCENE_UPLOAD_NATIVE {{\"available\":{},\"host_upload_allocation_bytes\":{},"
         "\"process_peak_host_upload_allocation_bytes\":{},\"committed_bytes\":{},\"process_peak_committed_bytes\":{},"
         "\"transfer_serial_before\":{},\"transfer_serial_after\":{}}}",
         native.available, native.hostUploadAllocationBytes, native.peakHostUploadAllocationBytes, native.committedBytes,
         native.peakCommittedBytes, transferSerialBefore,
         GDynamicRHI->GetLastSubmittedSerial(RHICommandContextType::eTransfer));
}
} // namespace

SceneRendererDemo::SceneRendererDemo(const platform::WindowConfig& windowConfig,
                                     sg::CameraType                type,
                                     const DemoProfilingOptions&   profiling,
                                     bool                          useAssetCache) :
    m_useAssetCache(useAssetCache), m_cameraType(type)
{
    m_profileWorkload    = profiling.workload;
    m_profileTargetScene = profiling.targetScene;
    m_pWindow      = new platform::NativeWindow(windowConfig);

    m_renderDevice = MakeUnique<rc::RenderDevice>(RHIAPIType::eVulkan, rc::RenderConfig::GetInstance().numFrames);

    if (!profiling.prefix.empty())
    {
        m_profiling = MakeUnique<SceneRendererProfiling>(*m_renderDevice, profiling);
    }

    const platform::WindowExtent pixels = m_pWindow->GetFramebufferExtent();

    m_pViewport = m_renderDevice->CreateViewport(m_pWindow, pixels.width, pixels.height, profiling.vsync);

    rc::ShaderProgramManager::GetInstance().BuildShaderPrograms(m_renderDevice.Get());

    m_renderDevice->Init(m_pViewport);

    m_renderDevice->InitializeRendererServer();

    const float aspect = windowConfig.aspect != 0.0f ? windowConfig.aspect : m_pWindow->GetAspect();

    m_pWindow->SetOnResize([this](uint32_t width, uint32_t height) { OnResize(width, height); });

    m_camera = sg::Camera::CreateUnique(Vec3{0.0f, 0.0f, 2.0f}, Vec3{0.0f, 0.0f, 0.0f}, aspect, type,
                                        sg::CameraProjectionType::ePerspective);

    m_camera->SetOnUpdate([&] {});

    m_timer = MakeUnique<platform::Timer>();
}

SceneRendererDemo::~SceneRendererDemo()
{
#if defined(ZEN_RUNTIME_UI)
    CancelRuntimeModel();
    m_sceneLoader.Reset();
    m_runtimeUI.Reset();
#endif

    delete m_pWindow;
}

bool SceneRendererDemo::EnableRuntimeUI()
{
    bool enabled = false;

#if defined(ZEN_RUNTIME_UI)
    if (!m_runtimeUI)
    {
        m_runtimeUI = MakeUnique<ui::RuntimeDebugUI>(*m_renderDevice, *m_pWindow, *this);

        enabled     = m_runtimeUI->Init();

        if (!enabled)
        {
            m_runtimeUI.Reset();
        }
    }
    else
    {
        enabled = true;
    }
#else
    LOGE("Runtime UI was disabled at build time; configure ZEN_BUILD_RUNTIME_UI=ON");
#endif

    return enabled;
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

bool SceneRendererDemo::Prepare(bool     captureVoxels,
                                uint32_t calibrationGridPercent,
                                bool     deterministicGuideCapture,
                                uint32_t sceneLoadRepeats)
{
    m_renderDevice->GetRendererServer()->RequestDeferredLightingRenderer()->SetDeterministicGuideCapture(
        deterministicGuideCapture);
    if (deterministicGuideCapture)
    {
        LOGI("GI capture uses exact learned-guide accumulation");
    }
    const std::string path = platform::ConfigLoader::GetInstance().GetDefaultModelPath();

    bool prepared          = LoadModel(path, true);

    for (uint32_t repeat = 0; prepared && repeat < sceneLoadRepeats; ++repeat)
    {
        prepared = LoadModel(path, true);
    }

#if defined(ZEN_RUNTIME_UI)
    if (prepared)
    {
        RefreshRuntimeModels();

        const std::u8string current =
            std::filesystem::absolute(std::filesystem::u8path(path)).lexically_normal().generic_u8string();

        m_modelState.currentPath.assign(reinterpret_cast<const char*>(current.data()), current.size());

        ++m_modelState.revision;
    }
#endif

    if (prepared && captureVoxels && calibrationGridPercent != 0)
    {
        // Diagnostic only: normalization and camera setup above use the real bounds.
        // Inset by one voxel to cancel GetVoxelBounds' normal one-cell padding.
        const uint32_t resolution = m_renderDevice->GetRendererServer()->RequestVoxelizer()->GetVoxelTexResolution();

        const float halfExtent    = 0.005f * static_cast<float>(calibrationGridPercent) * static_cast<float>(resolution - 2)
                                  / static_cast<float>(resolution);

        prepared                  = m_renderScene->SetVoxelBounds(sg::AABB(Vec3(-halfExtent), Vec3(halfExtent)));
    }

    if (prepared && captureVoxels)
    {
        prepared = m_renderDevice->GetRendererServer()->RequestVoxelizer()->EnableRadianceInputs();
    }

    return prepared;
}

bool SceneRendererDemo::LoadModel(const std::string& path, bool configuredCameraPosition)
{
    const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    asset::SceneImportRequest                   request{path};
    request.useCache                                   = m_useAssetCache;
    asset::SceneLoadResult                      result = asset::LoadSourceScene(request);
    const std::chrono::steady_clock::time_point ready  = std::chrono::steady_clock::now();
#if defined(ZEN_RUNTIME_UI)
    m_modelState.error = result.error;
#endif
    const bool loaded = result.scene && PublishModel(std::move(result.scene), configuredCameraPosition);
    if (RHIOptions::GetInstance().GPUMemoryStats())
    {
        const int64_t loadNS = std::chrono::duration_cast<std::chrono::nanoseconds>(ready - start).count();
        const int64_t publishNS =
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - ready).count();
        LOGI("SCENE_LOAD_TIMING {{\"loaded\":{},\"load_cpu_ns\":{},\"publish_cpu_ns\":{}}}", loaded, loadNS, publishNS);
    }
#if defined(ZEN_RUNTIME_UI)
    if (loaded)
    {
        // Messages describe the published model; a failed load keeps the previous ones.
        m_modelState.diagnostics = std::move(result.diagnostics);
    }
#endif
    return loaded;
}

bool SceneRendererDemo::PublishModel(UniquePtr<asset::LoadedScene> candidate, bool configuredCameraPosition)
{
    const bool profileUploads = m_profiling && RHIOptions::GetInstance().GPUMemoryStats();
    if (profileUploads)
    {
        // The demo profiler owns an otherwise empty CPU sink; its GPU sink remains
        // intact. Mark only graphs executed inside this production publication.
        LOGI("SCENE_UPLOAD_PROFILE {{\"event\":\"{}\"}}", "begin");
        m_renderDevice->GetRDGMetrics().SetSink(LogSceneUploadGraph);
    }
    const uint64_t transferSerialBefore =
        RHIOptions::GetInstance().GPUMemoryStats() ? GDynamicRHI->GetLastSubmittedSerial(RHICommandContextType::eTransfer) : 0;
    const rc::StagingPoolStatistics stagingBefore = m_renderDevice->GetStagingPoolStatistics();
    const uint64_t                  stagedBefore  = m_renderDevice->GetStagedUploadBytes();
    if (RHIOptions::GetInstance().GPUMemoryStats())
    {
        m_renderDevice->ResetStagingPoolStatistics();
    }
    bool                                        loaded = false;
    UniquePtr<sg::Scene>                        scene  = std::move(candidate->scene);
    UniquePtr<sg::Camera>                       camera;
    UniquePtr<rc::RenderScene>                  renderScene;
    const std::string&                          path  = candidate->path;
    const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();

    // A fresh camera prevents an authored orthographic/infinite projection from
    // carrying over when the next model relies on automatic framing.
    camera                          = sg::Camera::CreateUnique(Vec3(0, 0, 2), Vec3(0), m_pWindow->GetAspect(), m_cameraType);

    const std::string resourceError = rc::RenderScene::GetResourceError(m_renderDevice->GetGPUInfo(), *scene,
                                                                        candidate->vertices.size(), candidate->indices.size());

    if (!resourceError.empty())
    {
        LOGE("Cannot load scene '{}': {}", path, resourceError);

#if defined(ZEN_RUNTIME_UI)
        m_modelState.error = resourceError;
#endif
    }
    else if (m_renderDevice->PrepareForResourceReconfiguration())
    {
        asset::PrepareScenePlacement(*scene, asset::ScenePlacement::CenterAndNormalize);

        rc::SceneData data{};

        data.pCamera              = camera.Get();

        data.pScene               = scene.Get();

        data.pVertices            = candidate->vertices.data();

        data.pIndices             = candidate->indices.data();

        data.numVertices          = candidate->vertices.size();

        data.numIndices           = candidate->indices.size();

        data.envTextureName       = platform::ConfigLoader::GetInstance().GetString("environment_texture", "papermill.ktx");

        renderScene               = MakeUnique<rc::RenderScene>(m_renderDevice.Get(), data);

        const bool resourcesReady = renderScene->Init();

        const zen::HeapVector<sg::SceneCamera*> cameras = scene->GetComponents<sg::SceneCamera>();

        if (!cameras.empty())
        {
            camera->SetupFromSceneCamera(*cameras.front(), m_pWindow->GetAspect());
        }
        else
        {
            camera->SetupOnAABB(scene->GetAABB());

            Vec3 position = camera->GetPos();

            if (configuredCameraPosition && platform::ConfigLoader::GetInstance().ReadVec3("camera_position", position)
                && glm::length(position - scene->GetAABB().GetCenter()) > 1e-4f)
            {
                camera->SetPosition(position);
            }
        }

        if (resourcesReady && m_renderDevice->PrepareForSceneReplacement())
        {
            m_renderDevice->GetRendererServer()->SetRenderScene(renderScene.Get());

            m_renderScene.Swap(renderScene);

            m_scene.Swap(scene);

            m_camera.Swap(camera);

            loaded = true;

            PrepareLighting(platform::ConfigLoader::GetInstance());

            const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

            LOGI("Scene {} published in {} seconds", m_scene->GetName(), seconds);
        }
    }

    if (renderScene)
    {
        m_renderDevice->PrepareForResourceReconfiguration();

        renderScene->Destroy();
    }

    if (!loaded && m_renderScene)
    {
        m_renderDevice->GetRendererServer()->SetRenderScene(m_renderScene.Get());
    }

    if (RHIOptions::GetInstance().GPUMemoryStats())
    {
        LogSceneUploadStatistics(*m_renderDevice, stagingBefore, stagedBefore, transferSerialBefore, loaded);
    }

    if (profileUploads)
    {
        m_renderDevice->GetRDGMetrics().SetSink({});
        // Existing publication waits drive native completion. Collection observes
        // those results without adding a GPU wait or changing upload routing.
        m_renderDevice->GetRDGMetrics().CollectGPUResults();
        LOGI("SCENE_UPLOAD_PROFILE {{\"event\":\"{}\"}}", "end");
    }

    return loaded;
}

void SceneRendererDemo::PrepareLighting(const platform::ConfigLoader& config)
{
    m_dynamicLight = 0;

    m_editableLightIds.fill(0);

    m_editableLightCount = 0;

    m_boundsPresetLights = false;

    m_modelLightCount    = 0;

    m_orbitCenter        = Vec3(0.0f, 1.0f, 0.0f);

    m_orbitRadius        = 1.0f;

    m_orbitSpeedDegrees  = 45.0f;

    // Inactive slots retain a useful point-light seed for the current model.
    const sg::AABB& bounds = m_scene->GetAABB();

    const Vec3 center      = bounds.GetCenter();

    const float margin     = std::max(bounds.GetMaxExtent() * 0.15f, 0.01f);

    for (uint32_t index = 0; index < rc::MaxSceneLights; ++index)
    {
        rc::SceneLight light;

        const uint32_t axis            = (index / 2) % 3;

        light.position                 = center;

        light.position[axis]           = (index & 1) != 0 ? bounds.GetMax()[axis] + margin : bounds.GetMin()[axis] - margin;

        light.direction                = glm::normalize(center - light.position);

        const float distance           = glm::distance(center, light.position);

        light.intensity                = distance * distance * 3.0f;

        light.range                    = 0.0f;

        m_editableLightDefaults[index] = light;
    }

    m_lightAngle            = 0;

    bool animate            = false;

    uint32_t animatedIndex  = 0;

    bool valid              = config.ReadBool("dynamic_light.enabled", animate);

    valid                  &= config.ReadNumber("dynamic_light.index", animatedIndex);

    valid                  &= config.ReadVec3("dynamic_light.orbit_center", m_orbitCenter);

    valid                  &= config.ReadNumber("dynamic_light.orbit_radius", m_orbitRadius);

    valid                  &= config.ReadNumber("dynamic_light.angular_speed_degrees", m_orbitSpeedDegrees);

    valid                   = valid && m_orbitRadius >= 0.0f && std::abs(m_orbitSpeedDegrees) <= 3600.0f;

    bool overrideLights     = false;

    config.ReadBool("scene_lighting_override", overrideLights);

    const bool useConfiguredLights = overrideLights && !m_scene->HasComponent(typeid(sg::Light));

    const HeapVector<rc::ConfiguredLight> lights =
        useConfiguredLights ? rc::LoadSceneLights(config) : HeapVector<rc::ConfiguredLight>();

    if (useConfiguredLights)
    {
        HeapVector<rc::LightEntry> empty;

        m_renderScene->ReplaceLights(empty);
    }

    m_animatedLightIndex = std::min(animatedIndex, rc::MaxSceneLights - 1);

    m_editableLightCount = useConfiguredLights ? 4 : 0;

    if (useConfiguredLights
        && (!config.ReadNumber("light_count", m_editableLightCount) || m_editableLightCount > rc::MaxSceneLights))
    {
        m_editableLightCount = 0;
    }

    for (const rc::ConfiguredLight& light : lights)
    {
        const rc::LightId id                       = m_renderScene->GetLights().Add(light.light);

        m_editableLightIds[light.configIndex]      = id;

        m_editableLightDefaults[light.configIndex] = light.light;

        if (valid && animate && light.configIndex == animatedIndex && light.light.type != rc::SceneLightType::eDirectional)
        {
            m_dynamicLight = id;
        }
    }

    if (useConfiguredLights && (!valid || (animate && m_dynamicLight == 0)))
    {
        LOGW("Invalid dynamic_light configuration or missing point/spot light; animation disabled");
    }

    if (!useConfiguredLights && !m_scene->HasComponent(typeid(sg::Light)))
    {
        m_boundsPresetLights = true;

        // Reconfiguration replaces the explicit preset, preserving monotonic runtime IDs.
        HeapVector<rc::LightEntry> preset;

        for (const rc::SceneLight& light : rc::BuildBoundsLightPreset(m_renderScene->GetAABB()))
        {
            preset.push_back({0, light});
        }

        m_renderScene->ReplaceLights(preset);

        for (const rc::LightEntry& entry : m_renderScene->GetLights().GetEntries())
        {
            m_editableLightIds[m_editableLightCount]        = entry.id;

            m_editableLightDefaults[m_editableLightCount++] = entry.light;
        }
    }
    else if (m_scene->HasComponent(typeid(sg::Light)))
    {
        // Animated imported lights remain owned by RenderScene's imported ID mapping.
        m_modelLightCount = static_cast<uint32_t>(m_renderScene->GetLights().GetEntries().size());
    }

    float intensity  = 1.0f;

    float rotation   = 0.0f;

    bool enabled     = true;

    bool visible     = true;

    valid            = config.ReadNumber("environment_intensity", intensity);

    valid           &= config.ReadNumber("environment_rotation_degrees", rotation);

    valid           &= config.ReadBool("environment_lighting", enabled);

    valid           &= config.ReadBool("skybox_visible", visible);

    enabled          = rc::ResolveEnvironmentLighting(enabled, m_renderScene->HasImportedLights(), config);

    if (!valid || !m_renderScene->SetEnvironmentLighting(intensity, rotation, enabled, visible))
    {
        LOGW("Invalid environment settings; using defaults");
    }
}

bool SceneRendererDemo::PrepareProfileWorkload()
{
    bool valid = true;
    if (!m_profileInitialized)
    {
        m_profileCamera = m_camera->GetPos();
        m_profileExtent = m_renderScene->GetAABB().GetMaxExtent();
        if (m_profileWorkload == "dynamic" || m_profileWorkload == "expansion")
        {
            const HeapVector<rc::RenderScene::InstanceTriangleRange>& ranges  = m_renderScene->GetInstanceTriangleRanges();
            const HeapVector<rc::RenderScene::ShadowCaster>&          casters = m_renderScene->GetShadowCasters();
            for (const sg::Node* node : m_scene->GetRenderableNodes())
            {
                const uint32_t index = node->GetRenderableIndex();
                const Vec3     size  = casters[index].bounds.GetMax() - casters[index].bounds.GetMin();
                if (m_profileInstance == UINT32_MAX && ranges[index].triangleCount >= 12 && ranges[index].triangleCount <= 4096
                    && std::max({size.x, size.y, size.z}) < m_profileExtent * 0.2f)
                {
                    m_profileInstance  = index;
                    m_profileTransform = node->GetData().modelMatrix;
                }
            }
            valid = m_profileInstance != UINT32_MAX
                 && m_renderScene->SetInstanceClass(m_profileInstance, m_profileWorkload == "dynamic" ? GI_DYNAMIC : GI_STATIC);
            LOGI("GI profile workload {}: instance={}, world extent={}", m_profileWorkload, m_profileInstance, m_profileExtent);
        }
        m_profileInitialized = valid;
    }
    return valid;
}

bool SceneRendererDemo::UpdateProfileWorkload(uint32_t frame)
{
    bool        valid = true;
    const float phase = static_cast<float>(frame % 300) * (2.0f * 3.14159265359f / 300.0f);
    if (m_profileWorkload == "camera")
    {
        m_camera->SetPosition(m_profileCamera
                              + m_profileExtent * Vec3(0.03f * std::sin(phase), 0, 0.01f * std::sin(phase * 2)));
    }
    else if (m_profileWorkload == "dynamic")
    {
        const Vec3 offset = m_profileExtent * Vec3(0.005f * std::sin(phase), 0, 0.002f * std::sin(phase * 2));
        valid = m_renderScene->SetInstanceTransform(m_profileInstance, glm::translate(Mat4(1), offset) * m_profileTransform);
    }
    else if (m_profileWorkload == "expansion" && frame == 0)
    {
        valid = m_renderScene->SetInstanceTransform(
            m_profileInstance, glm::translate(Mat4(1), Vec3(m_profileExtent * 1.5f, 0, 0)) * m_profileTransform);
    }
    else if (m_profileWorkload == "switch" && frame % 30 == 0)
    {
        rc::VoxelGIRenderer* gi       = m_renderDevice->GetRendererServer()->RequestVoxelGI();
        rc::VoxelGISettings  settings = gi->GetSettings();
        settings.SetAllFeatureProviders((frame / 30) % 2 == 0 ? rc::VoxelGISettings::RayProvider::Hardware
                                                              : rc::VoxelGISettings::RayProvider::Voxel);
        valid = gi->SetSettings(settings);
    }
    else if (m_profileWorkload == "scene-switch" && frame == 0)
    {
        valid = !m_profileTargetScene.empty() && LoadModel(m_profileTargetScene, true);
    }
    return valid;
}

bool SceneRendererDemo::GetGIMotionFixture(uint32_t& moving, Mat4& original) const
{
    // Fixed motion diagnostic fixture: four
    // independent six-vertex meshes, with the moving receiver last. Reject other assets.
    const HeapVector<asset::Vertex>& vertices = m_renderScene->GetVertices();

    bool valid                                = m_scene->GetRenderableNodes().size() == 4 && vertices.size() == 24;

    if (valid)
    {
        const sg::Node* node                   = m_scene->GetRenderableNodes()[3];

        const HeapVector<sg::SubMesh*>& meshes = node->GetComponent<sg::Mesh>()->GetSubMeshes();

        valid    = meshes.size() == 1 && meshes[0]->GetFirstIndex() == 18 && meshes[0]->GetIndexCount() == 6;

        moving   = node->GetRenderableIndex();

        original = node->GetData().modelMatrix;
    }

    return valid;
}

void SceneRendererDemo::UpdateDynamicLight(float frameTime)
{
    const rc::SceneLight* current = m_renderScene->GetLights().Find(m_dynamicLight);

    if (current != nullptr && frameTime > 0.0f)
    {
        m_lightAngle =
            std::fmod(m_lightAngle + glm::radians(static_cast<double>(m_orbitSpeedDegrees)) * static_cast<double>(frameTime),
                      glm::two_pi<double>());

        rc::SceneLight light = *current;

        light.position =
            m_orbitCenter
            + m_orbitRadius
                  * Vec3(static_cast<float>(std::cos(m_lightAngle)), 0.0f, static_cast<float>(std::sin(m_lightAngle)));

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

    format.width        = width;

    format.height       = height;

    format.depth        = 1;

    format.format       = source->GetFormat();

    RHITexture* sampled = m_renderDevice->CreateTextureSampled(format, {.copyUsage = true}, "capture_color");

    RHIBufferCreateInfo info;

    info.size         = width * height * 4;

    info.allocateType = RHIBufferAllocateType::eGPU;

    info.usageFlags.SetFlags(RHIBufferUsageFlagBits::eStorageBuffer, RHIBufferUsageFlagBits::eTransferSrcBuffer);

    info.tag          = "capture_packed";

    RHIBuffer* packed = m_renderDevice->CreateBuffer(info);

    info.allocateType = RHIBufferAllocateType::eCPURead;

    info.usageFlags   = 0;

    info.usageFlags.SetFlag(RHIBufferUsageFlagBits::eTransferDstBuffer);

    info.tag            = "capture_readback";

    RHIBuffer* readback = m_renderDevice->CreateBuffer(info);

    RHISampler* sampler = m_renderDevice->CreateSampler(RHISamplerCreateInfo::CreateLinearRepeat());

    bool succeeded      = sampled != nullptr && packed != nullptr && readback != nullptr && sampler != nullptr;

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

        graph.AddComputePass(std::move(pack)).RecordPassCommands([width, height](rc::RDGPassCmdEncoder& encoder) {
            encoder.Dispatch((width + 7) / 8, (height + 7) / 8, 1);
        });

        graph.AddTransferPass("ReadCaptureColor").CopyBuffer(packed, readback, {0, 0, info.size}).NeverCull();

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

void SceneRendererDemo::StopProfiling()
{
    if (m_profiling)
    {
        m_profiling->Stop(*m_renderDevice, m_renderScene.Get(), *m_pViewport);
    }
}

bool SceneRendererDemo::Destroy(bool runSucceeded)
{
#if defined(ZEN_RUNTIME_UI)
    CancelRuntimeModel();
    m_sceneLoader.Reset();
    m_modelLoadResult = {};
    m_modelState.pendingPath.clear();
#endif
    StopProfiling();

    if (m_profiling)
    {
        // Drain the same outstanding frames required by device shutdown, outside the capture.
        m_renderDevice->WaitForPreviousFrames();
    }

#if defined(ZEN_RUNTIME_UI)
    // UI resources retire through the live device; disconnect the platform adapter before window teardown.
    m_runtimeUI.Reset();
#endif

    if (m_renderScene && m_renderDevice->PrepareForResourceReconfiguration())
    {
        m_renderScene->Destroy();
    }

    rc::ShaderProgramManager::GetInstance().Destroy();

    m_renderDevice->Destroy();

    return !m_profiling || m_profiling->Export(m_renderDevice->GetRDGMetrics(), runSucceeded);
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
        m_pWindow->SetSize(960, 640);
    }
    else if (frame == 20)
    {
        m_pWindow->Minimize();

        platform::NativeWindow::PollEvents();

        m_pWindow->Restore();
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
    if (frame == 0 || frame == 2 || frame == 3 || frame == 8 || frame == 11 || frame == 12 || frame == 21 || frame == 28)
    {
        m_renderDevice->GetRendererServer()->RequestVoxelizer()->RequestVoxelization();

        m_renderDevice->GetRDGMetrics().RequestCapture();

        LOGI("Smoke voxel update: frame={}", frame);
    }
}

bool SceneRendererDemo::Run(uint32_t           frameLimit,
                            bool               smokeTest,
                            uint32_t           initialMode,
                            const std::string& frameTimesPath,
                            bool               fixedStep,
                            uint32_t           giStartFrame,
                            bool               motionFixture,
                            bool               profileWarmup,
                            uint32_t           backgroundTestSeconds)
{
    SceneRendererWindowTest windowTest(*m_pWindow, backgroundTestSeconds);

    HeapVector<double> frameTimes;

    if (!frameTimesPath.empty())
    {
        frameTimes.reserve(frameLimit);
    }

    if (smokeTest)
    {
        rc::RDGMetricsOptions options = m_renderDevice->GetRDGMetrics().GetOptions();

        options.includeTransferNodes  = true;

        options.maxNodeDetails        = m_profiling ? options.maxNodeDetails : 64;

        m_renderDevice->GetRDGMetrics().Configure(options);
    }

    m_renderDevice->GetRendererServer()->SetRenderOption(giStartFrame != 0 ? rc::RenderOption::ePBR
                                                                           : static_cast<rc::RenderOption>(initialMode - 1));

    bool succeeded  = PrepareProfileWorkload();

    uint32_t moving = 0;

    Mat4 motionOriginal(1);

    if (motionFixture)
    {
        succeeded = GetGIMotionFixture(moving, motionOriginal) && m_renderScene->SetInstanceClass(moving, GI_DYNAMIC);

        if (!succeeded)
        {
            LOGE("GI motion requires the four-mesh M4 lifecycle fixture");
        }
    }

    uint32_t frames              = 0;
    uint32_t workloadFrame       = UINT32_MAX;

    uint32_t consecutiveDeclines = 0;

    double renderThreadUs        = 0;

    double preparationUs         = 0;

    while (succeeded && !m_pWindow->ShouldClose() && (frameLimit == 0 || frames < frameLimit)
           && !m_renderDevice->AreSubmissionsBlocked() && !windowTest.Complete())
    {
        const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();

        windowTest.Update(frames);

        if (windowTest.Complete())
        {
            break;
        }

        if (giStartFrame != 0 && frames == giStartFrame
            && m_renderDevice->GetRendererServer()->GetRequestedRenderOption() != rc::RenderOption::eVoxelGI)
        {
            LOGI("GI cold-start trigger: frame={}", frames);

            m_renderDevice->GetRendererServer()->SetRenderOption(rc::RenderOption::eVoxelGI);
        }

        if (smokeTest)
        {
            RunSmokeStep(frames);
        }

        float frameTime = static_cast<float>(m_timer->Tick());

        // Frame-limited runs are scripted captures, profiles and smokes: a stray Escape or Tab
        // in their focused window must not end or alter the run.
        bool routeWindowShortcuts = frameLimit == 0;

#if defined(ZEN_RUNTIME_UI)
        routeWindowShortcuts = routeWindowShortcuts && !m_runtimeUI;
#endif

        m_pWindow->Update(routeWindowShortcuts);

        if (m_pWindow->ShouldClose())
        {
            break;
        }

        const platform::WindowExtent extent = m_pWindow->GetFramebufferExtent();

        if (extent.width == 0 || extent.height == 0)
        {
            // Wait for restore/close without submitting to an unavailable surface.
            if (windowTest.Enabled())
            {
                platform::NativeWindow::WaitEvents(0.05);
            }
            else if (smokeTest)
            {
                m_pWindow->Restore();

                platform::NativeWindow::WaitEvents(0.05);
            }
            else
            {
                platform::NativeWindow::WaitEvents(0.05);
            }

            m_timer->Tick();

            continue;
        }

#if defined(ZEN_RUNTIME_UI)
        ProcessPendingModel();

        if (m_runtimeUI)
        {
            m_runtimeUI->Update(frameTime, *m_pViewport);
        }
#endif

        m_camera->Update(frameTime);
        if (!profileWarmup && workloadFrame != frames)
        {
            succeeded     &= UpdateProfileWorkload(frames);
            workloadFrame  = frames;
        }

        succeeded &= m_renderScene->AdvanceAnimation(smokeTest ? 1.0f / 30.0f : fixedStep ? 1.0f / 60.0f : frameTime);

        UpdateDynamicLight(smokeTest ? 1.0f / 30.0f : fixedStep ? 1.0f / 60.0f : frameTime);

        if (motionFixture)
        {
            const float seconds = static_cast<float>(m_motionFrame++) / 60.0f;

            const Vec3 offset(-0.12f * std::sin(seconds), 0.03f * std::cos(seconds), 0.025f * std::sin(seconds * 2));

            succeeded = m_renderScene->SetInstanceTransform(moving, glm::translate(Mat4(1), offset) * motionOriginal);

            if (!succeeded)
            {
                break;
            }
        }

        if (platform::KeyboardMouseInput::GetInstance().WasKeyPressedOnce(platform::Key::Digit1)
            | platform::KeyboardMouseInput::GetInstance().WasKeyPressedOnce(platform::Key::Keypad1))
        {
            m_renderDevice->GetRendererServer()->SetRenderOption(rc::RenderOption::eVoxelGI);
        }

        if (platform::KeyboardMouseInput::GetInstance().WasKeyPressedOnce(platform::Key::Digit2)
            | platform::KeyboardMouseInput::GetInstance().WasKeyPressedOnce(platform::Key::Keypad2))
        {
            m_renderDevice->GetRendererServer()->SetRenderOption(rc::RenderOption::eVoxelize);
        }

        // Always consume R; geometry updates are meaningful in both voxel modes.
        if (platform::KeyboardMouseInput::GetInstance().WasKeyPressedOnce(platform::Key::R)
            && m_renderDevice->GetRendererServer()->GetRenderOption() != rc::RenderOption::ePBR)
        {
            m_renderDevice->GetRendererServer()->RequestVoxelizer()->RequestVoxelization();

            m_renderDevice->GetRDGMetrics().RequestCapture();
        }

        if (m_profiling)
        {
            m_profiling->BeginFrame(*m_renderDevice, frames, profileWarmup);
        }

        rc::RenderOverlay* overlay = nullptr;

#if defined(ZEN_RUNTIME_UI)
        overlay = m_runtimeUI.Get();
#endif

        rc::RenderView view                = rc::RenderView::FromViewport(*m_pViewport);
        view.historyId                     = m_captureHistoryId;

        const rc::RenderFrameResult result = m_renderDevice->GetRendererServer()->DispatchRenderWorkloads(view, overlay);

        const bool retryDeclines           = rc::TrackDeclinedFrames(result, consecutiveDeclines);

        succeeded &=
            (result == rc::RenderFrameResult::eSubmitted || result == rc::RenderFrameResult::eDeclined) && retryDeclines;

        if (smokeTest)
        {
            LOGI("Smoke lighting: frame={} mode={} geometry_revision={} lighting_revision={}", frames,
                 static_cast<uint32_t>(m_renderDevice->GetRendererServer()->GetRenderOption()) + 1,
                 m_renderDevice->GetRendererServer()->RequestVoxelizer()->GetGeometryRevision(),
                 m_renderScene->GetLights().GetRevision());
        }

        m_renderDevice->NextFrame();

        // Scripted runs count only frames that rendered the requested, complete GI. While its distance
        // field builds in the background, frames fall back to HW RT or PBR, and while its local volumes
        // build, lights near them use the coarser scene field: profiles record those frames as warm-up,
        // and captures wait for the build.
        const bool preparingGI =
            frameLimit != 0 && (profileWarmup || (m_profileWorkload != "dynamic" && m_profileWorkload != "switch"))
            && (m_renderDevice->GetRendererServer()->IsPreparingGI() || m_renderDevice->GetRendererServer()->IsRefiningGI());

        frames                 += preparingGI ? 0 : 1;

        const double elapsed    = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();

        renderThreadUs         += elapsed;

        preparationUs           = preparingGI ? preparationUs + elapsed : 0.0;

        if (preparationUs > kGIPreparationLimitSeconds * 1e6)
        {
            LOGE("GI scene preparation did not finish within {} s", kGIPreparationLimitSeconds);

            succeeded = false;
        }

        if (!frameTimesPath.empty() && !preparingGI)
        {
            frameTimes.push_back(elapsed);
        }

        if (m_profiling)
        {
            m_profiling->RecordFrame(*m_renderDevice, *m_pViewport, elapsed, succeeded);
        }
    }

    m_renderDevice->FlushRHIThread();

    if (frameLimit != 0 && frames < frameLimit && m_pWindow->ShouldClose())
    {
        LOGE("Run stopped after {} of {} frames because the window was closed", frames, frameLimit);

        succeeded = false;
    }

    if (smokeTest)
    {
        LOGI("Smoke queue submissions: graphics={} compute={} transfer={}",
             GDynamicRHI->GetLastSubmittedSerial(RHICommandContextType::eGraphics),
             GDynamicRHI->GetLastSubmittedSerial(RHICommandContextType::eAsyncCompute),
             GDynamicRHI->GetLastSubmittedSerial(RHICommandContextType::eTransfer));
    }

    const RHIThreadMetrics metrics = m_renderDevice->GetRHIThreadMetrics();

    LOGI(
        "RHI counters: draws={} dispatches={} submissions={} descriptor_hits={} descriptor_misses={} descriptor_inserts={} descriptor_retirements={} bindless_captures={}",
        metrics.native.draws, metrics.native.dispatches, metrics.native.submissions, metrics.native.descriptorHits,
        metrics.native.descriptorMisses, metrics.native.descriptorInserts, metrics.native.descriptorRetirements,
        metrics.native.bindlessCaptures);

    LOGI("Render threads: mode={} frames={} render_wall_us={} rhi_cpu_us={} queue_wait_us={} batches={} peak_pending={}",
         GetRHIThread().IsThreaded() ? "threaded" : "inline", frames, renderThreadUs, metrics.executionCPUUs,
         metrics.queueWaitUs, metrics.completedBatches, metrics.peakPendingBatches);

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

    return succeeded && !m_renderDevice->AreSubmissionsBlocked() && (!windowTest.Enabled() || windowTest.Succeeded());
}

} // namespace zen

namespace
{
struct DemoOptions
{
    std::string profileWorkload{"still"};
    std::string profileTargetScene;
    int      runtimeUI{-1}; // Interactive runs default on; diagnostic runs remain reproducible.
    uint32_t frames{0};
    uint32_t warmup{0};
    uint32_t sceneLoadRepeats{0};
    // Retain the diagnostic --mode IDs independently of interactive key bindings.
    uint32_t initialMode{3};
    uint32_t giStartFrame{0};
    uint32_t backgroundTestSeconds{0};
    bool     smokeTest{false};
    bool     disableRT{false};
    bool     validationPrintf{false};
    bool     gpuMarkers{false};
    bool     gpuMemoryStats{false};
    bool     motionFixture{false};
    bool     useAssetCache{true};

    bool fixedStep{false};
    bool deterministicGuideCapture{false};

    bool vsync{true};

    std::optional<bool> validation;

    zen::RHIPresentMode presentMode{zen::RHIPresentMode::eDefault};

    zen::RHIBindlessHeapCapacities bindlessHeaps;

    std::string profilePath;

    std::string frameTimesPath;

    std::string capturePath;
    std::string providerSwitchCapturePath;
    bool        individualFeatureSwitches{false};
    std::string originStressCapturePath;
    std::string stabilityCapturePath;
    std::string cameraMotionCapturePath;
    std::string bounceLightCapturePath;
    std::string reflectionMotionCapturePath;
    std::string geometryMotionCapturePath;
    std::string independentViewsCapturePath;
    std::string lightingCapturePath;
    std::string rayCountCapturePath;
    std::string voxelCapturePath;
    bool        voxelReference{false};
    bool        voxelLifecycle{false};
    bool        voxelGBuffer{false};
    uint32_t    voxelGridPercent{0};
    uint32_t    width{1280};
    uint32_t    height{720};
};

bool ParseDemoOptions(int argc, char** arguments, DemoOptions& options)
{
    // The command line overrides the configured present mode.
    const std::string configuredPresentMode = zen::platform::ConfigLoader::GetInstance().GetString("present_mode", "default");

    bool valid                              = zen::ParseRHIPresentMode(configuredPresentMode, options.presentMode);

    if (!valid)
    {
        LOGE("Invalid present_mode '{}' in the engine configuration", configuredPresentMode);
    }

    for (int i = 1; i < argc && valid; ++i)
    {
        const std::string_view argument(arguments[i]);

        if (argument == "--rhi-thread=0" || argument == "--rhi-thread=1")
        {
            zen::rc::RenderConfig::GetInstance().rhiExecutionMode =
                argument.back() == '1' ? zen::RHIExecutionMode::eThreaded : zen::RHIExecutionMode::eInline;
        }
        else if (argument.starts_with("--async-compute="))
        {
            valid = zen::rc::ParseAsyncComputeOverride(argument, zen::rc::RenderConfig::GetInstance().asyncComputeMode);
        }
        else if (argument.starts_with("--mode="))
        {
            valid = argument == "--mode=1" || argument == "--mode=2" || argument == "--mode=3";

            if (valid)
            {
                options.initialMode = static_cast<uint32_t>(argument.back() - '0');
            }
        }
        else if (argument == "--ui" || argument == "--no-ui")
        {
            options.runtimeUI = argument == "--ui" ? 1 : 0;
        }
        else if (argument == "--no-asset-cache")
        {
            options.useAssetCache = false;
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
        else if (argument.starts_with("--profile="))
        {
            options.profilePath = argument.substr(10);

            valid               = !options.profilePath.empty();
        }
        else if (argument == "--vsync=0" || argument == "--vsync=1")
        {
            options.vsync = argument.back() == '1';
        }
        else if (argument.starts_with("--present-mode="))
        {
            valid = zen::ParseRHIPresentMode(argument.substr(15), options.presentMode);
        }
        else if (argument == "--fixed-step")
        {
            options.fixedStep = true;
        }
        else if (argument == "--deterministic-gi-capture")
        {
            options.deterministicGuideCapture = true;
        }
        else if (argument.starts_with("--capture-voxels="))
        {
            options.voxelCapturePath = argument.substr(17);

            valid                    = !options.voxelCapturePath.empty();
        }
        else if (argument.starts_with("--capture-provider-switching=") || argument.starts_with("--capture-feature-switching="))
        {
            options.providerSwitchCapturePath = argument.substr(argument.find('=') + 1);
            options.individualFeatureSwitches = argument.starts_with("--capture-feature-switching=");
            valid                             = !options.providerSwitchCapturePath.empty();
        }
        else if (argument.starts_with("--capture-origin-stress="))
        {
            options.originStressCapturePath = argument.substr(24);
            valid                           = !options.originStressCapturePath.empty();
        }
        else if (argument.starts_with("--capture-stability="))
        {
            options.stabilityCapturePath = argument.substr(20);
            valid                        = !options.stabilityCapturePath.empty();
        }
        else if (argument.starts_with("--capture-camera-motion="))
        {
            options.cameraMotionCapturePath = argument.substr(24);
            valid                           = !options.cameraMotionCapturePath.empty();
        }
        else if (argument.starts_with("--capture-bounce-lights="))
        {
            options.bounceLightCapturePath = argument.substr(24);
            valid                          = !options.bounceLightCapturePath.empty();
        }
        else if (argument.starts_with("--capture-reflection-motion="))
        {
            options.reflectionMotionCapturePath = argument.substr(28);
            valid                               = !options.reflectionMotionCapturePath.empty();
        }
        else if (argument.starts_with("--capture-geometry-motion="))
        {
            options.geometryMotionCapturePath = argument.substr(sizeof("--capture-geometry-motion=") - 1);
            valid                             = !options.geometryMotionCapturePath.empty();
        }
        else if (argument.starts_with("--capture-independent-views="))
        {
            options.independentViewsCapturePath = argument.substr(sizeof("--capture-independent-views=") - 1);
            valid                               = !options.independentViewsCapturePath.empty();
        }
        else if (argument.starts_with("--capture-lighting="))
        {
            options.lightingCapturePath = argument.substr(19);

            valid                       = !options.lightingCapturePath.empty();
        }
        else if (argument.starts_with("--capture-ray-counts="))
        {
            options.rayCountCapturePath = argument.substr(sizeof("--capture-ray-counts=") - 1);
            valid                       = !options.rayCountCapturePath.empty();
        }

        else if (argument == "--voxel-reference")
        {
            options.voxelReference = true;
        }
        else if (argument == "--voxel-lifecycle")
        {
            options.voxelLifecycle = true;
        }

        else if (argument == "--voxel-gbuffer")
        {
            options.voxelGBuffer = true;
        }
        else if (argument.starts_with("--voxel-grid-percent="))
        {
            const std::string_view value = argument.substr(21);

            const std::from_chars_result parsed =
                std::from_chars(value.data(), value.data() + value.size(), options.voxelGridPercent);

            valid = parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() && options.voxelGridPercent != 0;
        }
        else if (argument == "--disable-rt")
        {
            options.disableRT = true;
        }
        else if (argument.starts_with("--gpu=") && argument.size() > 6)
        {
            zen::RHIOptions::GetInstance().SetPreferredDevice(argument.substr(6));
        }
        else if (argument == "--rhi-counters=0" || argument == "--rhi-counters=1")
        {
            zen::RHIOptions::GetInstance().SetExecutionCountersEnabled(argument == "--rhi-counters=1");
        }
        else if (argument == "--enable-validation" || argument == "--disable-validation" || argument == "--validation-printf")
        {
            const bool enabled = argument != "--disable-validation";

            valid              = !options.validation.has_value() || options.validation.value() == enabled;
            if (valid)
            {
                options.validation        = enabled;
                options.validationPrintf |= argument == "--validation-printf";
            }
        }
        else if (argument == "--gpu-markers")
        {
            options.gpuMarkers = true;
        }
        else if (argument == "--gpu-memory-stats")
        {
            options.gpuMemoryStats = true;
        }
        else if (argument == "--device-loss-diagnostics")
        {
            zen::RHIOptions::GetInstance().SetDeviceLossDiagnostics(true);
        }
        else if (argument.starts_with("--bindless-textures=") || argument.starts_with("--bindless-samplers="))
        {
            const std::string_view value        = argument.substr(argument.find('=') + 1);

            uint32_t count                      = 0;

            const std::from_chars_result parsed = std::from_chars(value.data(), value.data() + value.size(), count);

            valid = parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() && count != 0;

            options.bindlessHeaps.Set(argument.starts_with("--bindless-textures=") ? zen::RHIBindlessHeapType::eTexture2D
                                                                                   : zen::RHIBindlessHeapType::eSampler,
                                      count);
        }
        else if (argument.starts_with("--profile-workload="))
        {
            options.profileWorkload = argument.substr(sizeof("--profile-workload=") - 1);
            valid                   = options.profileWorkload == "still" || options.profileWorkload == "camera"
                                   || options.profileWorkload == "dynamic" || options.profileWorkload == "expansion"
                                   || options.profileWorkload == "switch" || options.profileWorkload == "scene-switch";
        }
        else if (argument.starts_with("--profile-target-scene="))
        {
            options.profileTargetScene = argument.substr(sizeof("--profile-target-scene=") - 1);
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
        else if (argument.starts_with("--background-test-seconds="))
        {
            const std::string_view value = argument.substr(26);

            const std::from_chars_result parsed =
                std::from_chars(value.data(), value.data() + value.size(), options.backgroundTestSeconds);

            valid = parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() && options.backgroundTestSeconds > 0
                 && options.backgroundTestSeconds <= 600;
        }
        else if (argument.starts_with("--width=") || argument.starts_with("--height="))
        {
            const size_t offset                 = argument.find('=') + 1;

            const std::string_view value        = argument.substr(offset);

            uint32_t dimension                  = 0;

            const std::from_chars_result parsed = std::from_chars(value.data(), value.data() + value.size(), dimension);

            valid = parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() && dimension > 0 && dimension <= 4096;

            if (valid)
            {
                if (argument.starts_with("--width="))
                {
                    options.width = dimension;
                }
                else
                {
                    options.height = dimension;
                }
            }
        }
        else if (argument.starts_with("--scene-load-repeats="))
        {
            const std::string_view       value = argument.substr(sizeof("--scene-load-repeats=") - 1);
            const std::from_chars_result parsed =
                std::from_chars(value.data(), value.data() + value.size(), options.sceneLoadRepeats);
            valid = parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() && options.sceneLoadRepeats <= 1000;
        }
        else if (argument.starts_with("--frames=") || argument.starts_with("--warmup="))
        {
            const std::string_view value        = argument.substr(9);

            uint32_t& count                     = argument.starts_with("--frames=") ? options.frames : options.warmup;

            const std::from_chars_result parsed = std::from_chars(value.data(), value.data() + value.size(), count);

            valid                               = parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size();
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

    return valid && (options.frameTimesPath.empty() || options.frames != 0)
        && (options.profilePath.empty() || options.frames != 0)
        && (options.backgroundTestSeconds == 0 || (options.frames == 0 && options.warmup == 0 && !options.smokeTest))
        && (options.giStartFrame == 0
            || (options.initialMode == 3 && options.warmup == 0 && !options.smokeTest
                && (options.frames == 0 || options.giStartFrame < options.frames)))
        && (!(options.voxelReference || options.voxelLifecycle || options.voxelGBuffer || options.voxelGridPercent != 0)
            || !options.voxelCapturePath.empty());
}
} // namespace

#if !defined(ZEN_SCENE_MODEL_TEST)
#    if defined(ZEN_WIN32)
static void ShowFatalErrorDialog(const char* message) noexcept
{
    MessageBoxA(nullptr, message, "ZenEngine could not continue", MB_OK | MB_ICONERROR | MB_TASKMODAL);
}
#    endif

int main(int argc, char** pArgv)
{
    using namespace zen;

    DemoOptions options;

    int result = 1;

    if (ParseDemoOptions(argc, pArgv, options))
    {
#    if defined(ZEN_WIN32)
        if (!options.smokeTest && options.frames == 0)
        {
            SetVerificationReporter(&ShowFatalErrorDialog);
        }
#    endif

        RHIOptions::GetInstance().SetRayTracingDisabled(options.disableRT);

        if (options.validation.has_value())
        {
            RHIOptions::GetInstance().SetValidationEnabled(options.validation.value());
        }

        RHIOptions::GetInstance().SetDebugPrintfEnabled(options.validationPrintf);

        RHIOptions::GetInstance().SetGPUProfilerMarkers(options.gpuMarkers);

        RHIOptions::GetInstance().SetGPUMemoryStats(options.gpuMemoryStats);

        RHIOptions::GetInstance().SetPresentMode(options.presentMode);

        // Parsing rejects zero counts, so the options always accept these capacities.
        RHIOptions::GetInstance().SetBindlessHeapCapacities(options.bindlessHeaps);

        platform::WindowConfig windowConfig{"scene_renderer_demo", true, options.width, options.height};

        DemoProfilingOptions profiling;

        profiling.prefix       = options.profilePath;
        profiling.workload     = options.profileWorkload;
        profiling.targetScene  = options.profileTargetScene;

        profiling.frames       = options.frames;

        profiling.warmup       = options.warmup;

        profiling.giStartFrame = options.giStartFrame;

        profiling.fixedStep    = options.fixedStep;

        profiling.vsync        = options.vsync;

        if (!profiling.prefix.empty())
        {
            for (int index = 0; index < argc; ++index)
            {
                profiling.arguments.emplace_back(pArgv[index]);
            }
        }

        SceneRendererDemo* pDemo =
            new SceneRendererDemo(windowConfig, sg::CameraType::eFirstPerson, profiling, options.useAssetCache);

        bool prepared = pDemo->Prepare(!options.voxelCapturePath.empty(), options.voxelGridPercent,
                                       options.deterministicGuideCapture, options.sceneLoadRepeats);

        bool enableUI = options.runtimeUI == 1;

#    if defined(ZEN_RUNTIME_UI)
        enableUI = enableUI
                || (options.runtimeUI < 0 && options.frames == 0 && !options.smokeTest && options.profilePath.empty()
                    && options.capturePath.empty() && options.voxelCapturePath.empty() && options.lightingCapturePath.empty());
#    endif

        if (prepared && enableUI)
        {
            prepared = pDemo->EnableRuntimeUI();
        }

        const bool warmed = prepared
                         && (options.warmup == 0
                             || pDemo->Run(options.warmup, false, options.initialMode, {}, options.fixedStep, 0,
                                           options.motionFixture, true));

        result            = warmed
                                 && pDemo->Run(options.frames, options.smokeTest, options.initialMode, options.frameTimesPath,
                                               options.fixedStep, options.giStartFrame, options.motionFixture, false,
                                               options.backgroundTestSeconds)
                              ? 0
                              : 1;

        // Extra diagnostic capture graphs are outside the requested profiling workload.
        pDemo->StopProfiling();

        if (result == 0 && !options.rayCountCapturePath.empty())
        {
            result = pDemo->CaptureRayCounts(options.rayCountCapturePath) ? 0 : 1;
        }

        if (result == 0 && !options.lightingCapturePath.empty())
        {
            result = pDemo->CaptureLighting(options.lightingCapturePath) ? 0 : 1;
        }

        if (result == 0 && !options.providerSwitchCapturePath.empty())
        {
            result =
                pDemo->CaptureProviderSwitching(options.providerSwitchCapturePath, options.individualFeatureSwitches) ? 0 : 1;
        }
        if (result == 0 && !options.originStressCapturePath.empty())
        {
            result = pDemo->CaptureOriginStress(options.originStressCapturePath) ? 0 : 1;
        }
        if (result == 0 && !options.stabilityCapturePath.empty())
        {
            result = pDemo->CaptureStability(options.stabilityCapturePath) ? 0 : 1;
        }
        if (result == 0 && !options.cameraMotionCapturePath.empty())
        {
            result = pDemo->CaptureCameraMotion(options.cameraMotionCapturePath) ? 0 : 1;
        }
        if (result == 0 && !options.bounceLightCapturePath.empty())
        {
            result = pDemo->CaptureBounceLightChanges(options.bounceLightCapturePath) ? 0 : 1;
        }
        if (result == 0 && !options.reflectionMotionCapturePath.empty())
        {
            result = pDemo->CaptureReflectionMotion(options.reflectionMotionCapturePath) ? 0 : 1;
        }
        if (result == 0 && !options.geometryMotionCapturePath.empty())
        {
            result = pDemo->CaptureGeometryMotion(options.geometryMotionCapturePath) ? 0 : 1;
        }
        if (result == 0 && !options.independentViewsCapturePath.empty())
        {
            result = pDemo->CaptureIndependentViews(options.independentViewsCapturePath) ? 0 : 1;
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

        if (result == 0 && options.voxelGBuffer)
        {
            result = pDemo->CaptureVoxelGBuffer(options.voxelCapturePath) ? 0 : 1;
        }

#    if defined(ZEN_WIN32)
        const RHIError terminalError = GetRHIThread().GetFailure();

        if (terminalError.IsFailure() && !options.smokeTest && options.frames == 0)
        {
            const std::string message =
                fmt::format("Rendering stopped: {} (error {}, native {}).",
                            terminalError.operation != nullptr ? terminalError.operation : "RHI failure",
                            static_cast<uint32_t>(terminalError.code), terminalError.nativeCode);

            ShowFatalErrorDialog(message.c_str());
        }
#    endif

        result = pDemo->Destroy(result == 0) ? result : 1;

        delete pDemo;
    }
    else
    {
        LOGE(
            "Usage: scene_renderer_demo [--ui|--no-ui] [--no-asset-cache] [--rhi-thread=0|1] [--async-compute=0|1] [--frames=N] [--warmup=N] [--scene-load-repeats=0..1000] [--frame-times=path.csv] [--profile=prefix] [--profile-workload=still|camera|dynamic|expansion|switch|scene-switch] [--profile-target-scene=path.gltf] [--vsync=0|1] [--present-mode=default|fifo|fifo_relaxed|mailbox|immediate] [--fixed-step] [--deterministic-gi-capture] [--mode=1|2|3] [--smoke-test] [--background-test-seconds=N] [--disable-rt] [--gpu=NAME] [--enable-validation|--disable-validation] [--validation-printf] [--rhi-counters=0|1] [--gpu-markers] [--gpu-memory-stats] [--device-loss-diagnostics] [--bindless-textures=N] [--bindless-samplers=N] [--gi-start-frame=N] [--gi-motion-fixture] [--capture=frame.ppm] [--capture-lighting=prefix] [--capture-ray-counts=path.json] [--capture-provider-switching=prefix] [--capture-feature-switching=prefix] [--capture-origin-stress=prefix] [--capture-stability=prefix] [--capture-camera-motion=prefix] [--capture-bounce-lights=prefix] [--capture-reflection-motion=prefix] [--capture-geometry-motion=prefix] [--capture-independent-views=prefix] [--capture-traversal=prefix] [--dynamic-gi-lifecycle] [--gi-method-switching] [--gi-contracts] [--capture-voxels=prefix] [--voxel-reference] [--voxel-lifecycle] [--voxel-classes] [--voxel-gbuffer] [--voxel-grid-percent=N] [--width=N] [--height=N]");
    }

    return result;
}
#endif
