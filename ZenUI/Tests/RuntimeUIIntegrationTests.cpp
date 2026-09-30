#include "UI/RuntimeDebugUI.h"
#include "Graphics/RenderCore/V2/Renderer/RendererServer.h"
#include "Graphics/RenderCore/V2/Renderer/VoxelizerBase.h"
#include "Graphics/RenderCore/V2/RenderScene.h"
#include "Graphics/RenderCore/V2/ShaderProgram.h"
#include "Graphics/RHI/RHIOptions.h"
#include "Platform/GlfwWindow.h"
#include "imgui_internal.h"
#include <gtest/gtest.h>

namespace zen::ui
{
struct RuntimeDebugUITestAccess
{
    static void Reload(RuntimeDebugUI& ui)
    {
        ui.ReloadSettings();
    }

    static void SetGI(RuntimeDebugUI& ui, const rc::VoxelGIRuntimeSettings& settings)
    {
        ui.m_draft = settings;

        ui.MarkGIEdit(true);
    }

    static void SetScene(RuntimeDebugUI& ui, const RuntimeSceneSettings& settings)
    {
        ui.m_sceneDraft = settings;

        ui.MarkSceneEdit(true);
    }

    static void SetAutomatic(RuntimeDebugUI& ui, bool enabled)
    {
        ui.m_autoApply = enabled;
    }

    static void Apply(RuntimeDebugUI& ui, bool manual = false)
    {
        ui.ApplyPendingSettings(manual);
    }

    static void EnsureReflectanceBudget(RuntimeDebugUI& ui)
    {
        ui.EnsureReflectanceBudget();
    }

    static const char* Status(const RuntimeDebugUI& ui)
    {
        return ui.m_status;
    }

    static uint64_t ReflectanceBudget(const RuntimeDebugUI& ui)
    {
        return ui.m_draft.reflectanceBudgetBytes;
    }
};

namespace
{
TEST(RuntimeSceneControls, PendingLightEditsPreserveOrbitUnlessPositionWasEdited)
{
    rc::SceneLight previous;

    rc::SceneLight current = previous;

    current.position = Vec3(1, 2, 3);

    rc::SceneLight requested = previous;

    requested.intensity = 10;

    rc::SceneLight merged = MergeRuntimeLightEdit(current, previous, requested);

    EXPECT_EQ(merged.position, current.position);

    EXPECT_FLOAT_EQ(merged.intensity, 10);

    requested.position = Vec3(4, 5, 6);

    merged = MergeRuntimeLightEdit(current, previous, requested);

    EXPECT_EQ(merged.position, requested.position);
}

TEST(RuntimeSceneControls, AnimationNeedsAnExistingPointOrSpotAndSpotAnglesStayOrdered)
{
    RuntimeSceneSettings settings;

    settings.animationEnabled = true;

    EXPECT_FALSE(ValidateRuntimeSceneSettings(settings));

    settings.lightCount = 1;

    EXPECT_TRUE(ValidateRuntimeSceneSettings(settings));

    settings.lights[0].type = rc::SceneLightType::eDirectional;

    EXPECT_FALSE(ValidateRuntimeSceneSettings(settings));

    settings.animationEnabled = false;

    EXPECT_TRUE(ValidateRuntimeSceneSettings(settings));

    settings.lights[0].innerAngleDegrees = settings.lights[0].outerAngleDegrees;

    EXPECT_FALSE(ValidateRuntimeSceneSettings(settings));
}

class SceneControls : public RuntimeSceneControls
{
public:
    RuntimeSceneSettings settings;

    uint32_t applied{0};

    RuntimeSceneSettings GetRuntimeSceneSettings() const override
    {
        return settings;
    }

    bool ApplyRuntimeSceneSettings(const RuntimeSceneSettings&,
                                   const RuntimeSceneSettings& next) override
    {
        settings = next;

        ++applied;

        return true;
    }
};

TEST(RuntimeUIIntegration, AutoApplyDefersResourcesButUpdatesLiveControlsAndSupportsManualMode)
{
    platform::GlfwWindowImpl window({"Runtime UI tests", false, 64, 64});

    glfwHideWindow(window.GetHandle());

    RHIOptions::GetInstance().SetRayTracingEnabled(false);

    rc::RenderDevice device(RHIAPIType::eVulkan, 2);

    RHIViewport* viewport = device.CreateViewport(&window, 64, 64, false);

    rc::ShaderProgramManager::GetInstance().BuildShaderPrograms(&device);

    device.Init(viewport);

    // Live UI counters must work without the optional --gpu-memory-stats log flag.
    EXPECT_FALSE(RHIOptions::GetInstance().GPUMemoryStats());

    const RHIGPUMemoryStats memory = device.GetGPUMemoryStats();

    EXPECT_TRUE(memory.available);

    EXPECT_GT(memory.committedBytes, 0u);

    EXPECT_GT(memory.deviceLocalBytes, 0u);

    EXPECT_GE(memory.peakCommittedBytes, memory.committedBytes);

    EXPECT_GE(memory.peakDeviceLocalBytes, memory.deviceLocalBytes);

    ImGuiContext* context = ImGui::CreateContext();

    {
        SceneControls scene;

        RuntimeDebugUI ui(device, window, scene);

        RuntimeDebugUITestAccess::Reload(ui);

        rc::RendererServer& server = *device.GetRendererServer();

        rc::VoxelGIRuntimeSettings initial = server.GetVoxelGISettings();

        initial.resolution = 64;

        EXPECT_TRUE(server.ApplyVoxelGISettings(initial));

        RuntimeDebugUITestAccess::Reload(ui);

        rc::VoxelGIRuntimeSettings next = server.GetVoxelGISettings();

        // A rejected averaged budget must not destroy the current owner volumes,
        // change queues or resize shadows before discovering the rejection.
        rc::VoxelizerBase* owner = server.RequestVoxelizer();

        ASSERT_TRUE(owner->EnsureReady());

        RHITexture* ownerTexture = owner->GetVoxelTextures().pOwner;

        rc::VoxelGIRuntimeSettings insufficient = next;

        insufficient.resolution = 256;

        insufficient.averagedReflectance = true;

        insufficient.reflectanceBudgetBytes = 64ull * 1024 * 1024;

        insufficient.asyncCompute = next.asyncCompute == platform::AsyncComputeMode::eAuto ?
            platform::AsyncComputeMode::eDisabled :
            platform::AsyncComputeMode::eAuto;

        insufficient.shadowMapResolution = 512;

        uint64_t required = 0;

        const bool supportsLargeBuffer =
            device.GetGPUInfo().maxStorageBufferRange >= 256ull * 1024 * 1024;

        EXPECT_EQ(server.ValidateVoxelGIResources(insufficient, required),
                  supportsLargeBuffer ? rc::GIResourceStatus::eBudget :
                                        rc::GIResourceStatus::eDescriptorRange);

        EXPECT_EQ(required, supportsLargeBuffer ? 320ull * 1024 * 1024 : 0);

        EXPECT_FALSE(server.ApplyVoxelGISettings(insufficient));

        EXPECT_EQ(server.RequestVoxelizer(), owner);

        EXPECT_EQ(server.RequestVoxelizer()->GetVoxelTextures().pOwner, ownerTexture);

        EXPECT_EQ(server.GetVoxelGISettings().resolution, next.resolution);

        EXPECT_EQ(server.GetVoxelGISettings().asyncCompute, next.asyncCompute);

        EXPECT_EQ(server.GetVoxelGISettings().shadowMapResolution, next.shadowMapResolution);

        EXPECT_FALSE(server.GetVoxelGISettings().averagedReflectance);

        RuntimeDebugUITestAccess::SetGI(ui, insufficient);

        RuntimeDebugUITestAccess::Apply(ui);

        EXPECT_EQ(server.RequestVoxelizer(), owner);

        EXPECT_STREQ(
            RuntimeDebugUITestAccess::Status(ui),
            supportsLargeBuffer ?
                "Reflectance budget is below the required minimum. Current settings are unchanged." :
                "Averaged reflectance exceeds the GPU buffer limit. Use a lower voxel resolution.");

        RuntimeDebugUITestAccess::EnsureReflectanceBudget(ui);

        EXPECT_EQ(RuntimeDebugUITestAccess::ReflectanceBudget(ui), 320ull * 1024 * 1024);

        // Filling in a future budget while owner reflectance is selected is a
        // settings-only edit, including while an input field remains active.
        next.reflectanceBudgetBytes = 320ull * 1024 * 1024;

        RuntimeDebugUITestAccess::SetGI(ui, next);

        ImGui::SetActiveID(122, nullptr);

        RuntimeDebugUITestAccess::Apply(ui);

        EXPECT_EQ(server.RequestVoxelizer(), owner);

        EXPECT_EQ(server.RequestVoxelizer()->GetVoxelTextures().pOwner, ownerTexture);

        EXPECT_EQ(server.GetVoxelGISettings().reflectanceBudgetBytes, next.reflectanceBudgetBytes);

        ImGui::ClearActiveID();

        next.cone.indirectIntensity = 2.5f;

        // Simulate a slider still being held: lightweight settings must update now.
        ImGui::SetActiveID(123, nullptr);

        RuntimeDebugUITestAccess::SetGI(ui, next);

        RuntimeDebugUITestAccess::Apply(ui);

        EXPECT_FLOAT_EQ(server.GetVoxelGISettings().cone.indirectIntensity, 2.5f);

        next.resolution = 128;

        RuntimeDebugUITestAccess::SetGI(ui, next);

        RuntimeDebugUITestAccess::Apply(ui);

        EXPECT_EQ(server.GetVoxelGISettings().resolution, 64u);

        ImGui::ClearActiveID();

        RuntimeDebugUITestAccess::Apply(ui);

        EXPECT_EQ(server.GetVoxelGISettings().resolution, 128u);

        next.cone.analyticLighting = false;

        next.cone.environmentLighting = false;

        next.cone.emissiveLighting = false;

        RuntimeDebugUITestAccess::SetGI(ui, next);

        RuntimeDebugUITestAccess::Apply(ui);

        EXPECT_FALSE(server.GetVoxelGISettings().cone.analyticLighting);

        EXPECT_FALSE(server.GetVoxelGISettings().cone.environmentLighting);

        EXPECT_FALSE(server.GetVoxelGISettings().cone.emissiveLighting);

        RuntimeDebugUITestAccess::Reload(ui);

        RuntimeSceneSettings sceneEdit = scene.settings;

        sceneEdit.environmentIntensity = 2;

        ImGui::SetActiveID(456, nullptr);

        RuntimeDebugUITestAccess::SetScene(ui, sceneEdit);

        RuntimeDebugUITestAccess::Apply(ui);

        EXPECT_EQ(scene.applied, 1u);

        EXPECT_FLOAT_EQ(scene.settings.environmentIntensity, 2);

        ImGui::ClearActiveID();

        RuntimeDebugUITestAccess::SetAutomatic(ui, false);

        sceneEdit.environmentIntensity = 3;

        RuntimeDebugUITestAccess::SetScene(ui, sceneEdit);

        RuntimeDebugUITestAccess::Apply(ui);

        EXPECT_EQ(scene.applied, 1u);

        RuntimeDebugUITestAccess::Apply(ui, true);

        EXPECT_EQ(scene.applied, 2u);

        RuntimeDebugUITestAccess::SetAutomatic(ui, true);

        sceneEdit.lightCount = 1;

        sceneEdit.lights[0].direction = Vec3(0);

        RuntimeDebugUITestAccess::SetScene(ui, sceneEdit);

        RuntimeDebugUITestAccess::Apply(ui);

        EXPECT_EQ(scene.applied, 2u);

        sceneEdit.lights[0].direction = Vec3(0, -1, 0);

        RuntimeDebugUITestAccess::SetScene(ui, sceneEdit);

        RuntimeDebugUITestAccess::Apply(ui);

        EXPECT_EQ(scene.applied, 3u);
    }

    ImGui::DestroyContext(context);

    device.FlushRHIThread();

    device.WaitForIdle();

    rc::ShaderProgramManager::GetInstance().Destroy();

    device.Destroy();

    RHIOptions::GetInstance().SetRayTracingEnabled(true);
}

bool ExecuteVoxelGeneration(rc::RenderDevice& device, rc::RendererServer& server)
{
    rc::VoxelizerBase* voxelizer = server.RequestVoxelizer();

    rc::RenderGraph& graph = *device.GetCurrentFrameRDG();

    bool succeeded = graph.Begin();

    if (succeeded)
    {
        voxelizer->RequestVoxelization();

        voxelizer->BuildVoxelizationGraph();

        succeeded = graph.End() && device.ExecuteRenderGraph(graph);

        voxelizer->OnRenderGraphExecuted(succeeded);

        device.NextFrame();
    }

    return succeeded && !device.AreSubmissionsBlocked();
}

class ReflectanceRuntimeIntegration : public testing::TestWithParam<uint32_t>
{};

TEST_P(ReflectanceRuntimeIntegration, RejectsSmallBudgetThenAppliesAndRendersBothPolicies)
{
    platform::GlfwWindowImpl window({"Reflectance reconfiguration tests", false, 64, 64});

    glfwHideWindow(window.GetHandle());

    RHIOptions::GetInstance().SetRayTracingEnabled(false);

    rc::RenderDevice device(RHIAPIType::eVulkan, 2,
                            (GetParam() & 1) ? RHIExecutionMode::eThreaded :
                                               RHIExecutionMode::eInline,
                            platform::AsyncComputeMode::eAuto);

    RHIViewport* viewport = device.CreateViewport(&window, 64, 64, false);

    rc::ShaderProgramManager::GetInstance().BuildShaderPrograms(&device);

    device.Init(viewport);

    sg::Scene source;

    source.GetAABB() = sg::AABB(Vec3(-1), Vec3(1));

    const asset::Vertex vertex{};

    const uint32_t index = 0;

    rc::SceneData data{};

    data.pScene = &source;

    data.pVertices = &vertex;

    data.pIndices = &index;

    data.numVertices = 1;

    data.numIndices = 1;

    rc::RenderScene renderScene(&device, data);

    rc::RendererServer& server = *device.GetRendererServer();

    server.SetRenderScene(&renderScene);

    ImGuiContext* context = ImGui::CreateContext();

    {
        SceneControls scene;

        RuntimeDebugUI ui(device, window, scene);

        for (uint32_t resolution : {64u, 128u, 256u})
        {
            SCOPED_TRACE(resolution);

            rc::VoxelGIRuntimeSettings settings = server.GetVoxelGISettings();

            settings.resolution = resolution;

            settings.voxelizer = (GetParam() & 2) ? platform::VoxelizerMode::eGeometry :
                                                    platform::VoxelizerMode::eCompute;

            settings.averagedReflectance = false;

            settings.reflectanceBudgetBytes = 0;

            ASSERT_TRUE(server.ApplyVoxelGISettings(settings));

            ASSERT_TRUE(ExecuteVoxelGeneration(device, server));

            rc::VoxelizerBase* owner = server.RequestVoxelizer();

            RHITexture* ownerTexture = owner->GetVoxelTextures().pOwner;

            settings.averagedReflectance = true;

            settings.reflectanceBudgetBytes = rc::GetVoxelReflectanceRequiredBytes(resolution) - 1;

            RuntimeDebugUITestAccess::Reload(ui);

            RuntimeDebugUITestAccess::SetGI(ui, settings);

            RuntimeDebugUITestAccess::Apply(ui);

            EXPECT_EQ(server.RequestVoxelizer(), owner);

            EXPECT_EQ(server.RequestVoxelizer()->GetVoxelTextures().pOwner, ownerTexture);

            EXPECT_FALSE(server.GetVoxelGISettings().averagedReflectance);

            ASSERT_TRUE(ExecuteVoxelGeneration(device, server));

            RuntimeDebugUITestAccess::EnsureReflectanceBudget(ui);

            RuntimeDebugUITestAccess::Apply(ui);

            if (device.GetGPUInfo().maxStorageBufferRange >=
                uint64_t(resolution) * resolution * resolution * 16)
            {
                EXPECT_TRUE(server.GetVoxelGISettings().averagedReflectance);

                ASSERT_TRUE(ExecuteVoxelGeneration(device, server));

                EXPECT_TRUE(server.RequestVoxelizer()->UsesAveragedReflectance());

                EXPECT_NE(server.RequestVoxelizer()->GetReflectanceSums(), nullptr);

                EXPECT_NE(server.RequestVoxelizer()->GetVoxelTextures().pReflectance, nullptr);
            }
            else
            {
                EXPECT_FALSE(server.GetVoxelGISettings().averagedReflectance);
            }

            settings = server.GetVoxelGISettings();

            settings.averagedReflectance = false;

            RuntimeDebugUITestAccess::SetGI(ui, settings);

            RuntimeDebugUITestAccess::Apply(ui);

            ASSERT_TRUE(ExecuteVoxelGeneration(device, server));

            EXPECT_FALSE(server.RequestVoxelizer()->UsesAveragedReflectance());
        }
    }

    ImGui::DestroyContext(context);

    device.FlushRHIThread();

    device.WaitForIdle();

    renderScene.Destroy();

    rc::ShaderProgramManager::GetInstance().Destroy();

    device.Destroy();

    RHIOptions::GetInstance().SetRayTracingEnabled(true);
}

INSTANTIATE_TEST_SUITE_P(SubmissionModes,
                         ReflectanceRuntimeIntegration,
                         testing::Values(0u, 1u, 2u, 3u));

} // namespace
} // namespace zen::ui
