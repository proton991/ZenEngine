#include "Math/Math.h"
#include "Graphics/RenderCore/V2/Renderer/RendererServer.h"
#include "Graphics/RenderCore/V2/Renderer/VoxelizerBase.h"
#include "Graphics/RenderCore/V2/RenderScene.h"
#include "Graphics/RenderCore/V2/ShaderProgram.h"
#include "Graphics/RenderCore/V2/VoxelResourcePlanning.h"
#include "Graphics/RenderCore/V2/RenderConfig.h"
#include "Platform/NativeWindow.h"
#include <gtest/gtest.h>

namespace
{
using namespace zen;

bool ExecuteVoxelGeneration(rc::RenderDevice& device, rc::RendererServer& server)
{
    rc::VoxelizerBase* voxelizer = server.RequestVoxelizer();

    rc::RenderGraph& graph       = *device.GetCurrentFrameRDG();

    bool succeeded               = graph.Begin();

    if (succeeded)
    {
        voxelizer->RequestVoxelization();

        voxelizer->BuildVoxelizationGraph();

        succeeded = graph.End() && device.ExecuteRenderGraph(graph);

        voxelizer->OnRenderGraphExecuted(succeeded ? rc::RenderFrameResult::eSubmitted : rc::RenderFrameResult::eFailed);

        device.NextFrame();
    }

    return succeeded && !device.AreSubmissionsBlocked();
}

class VoxelReflectanceIntegrationTest : public testing::TestWithParam<uint32_t>
{};

TEST_P(VoxelReflectanceIntegrationTest, RejectsSmallBudgetThenAppliesAndRendersBothPolicies)
{
    platform::NativeWindow window({"Reflectance reconfiguration tests", false, 64, 64});

    window.Hide();

    zen::rc::RenderConfig::GetInstance().rhiDevice.SetRayTracingDisabled(true);

    rc::RenderDevice device(RHIAPIType::eVulkan, 2, (GetParam() & 1) ? RHIExecutionMode::eThreaded : RHIExecutionMode::eInline,
                            (GetParam() & 2) ? platform::AsyncComputeMode::eAuto : platform::AsyncComputeMode::eDisabled);

    RHIViewport* viewport = device.CreateViewport(window.GetRHIWindowHandle(), 64, 64, false);

    rc::ShaderProgramManager::GetInstance().BuildShaderPrograms(&device);

    device.Init(viewport);

    device.InitializeRendererServer();

    sg::Scene source;

    source.GetAABB() = sg::AABB(Vec3(-1), Vec3(1));

    const asset::Vertex vertex{};

    const uint32_t index = 0;

    rc::SceneData data{};

    data.pScene      = &source;

    data.pVertices   = &vertex;

    data.pIndices    = &index;

    data.numVertices = 1;

    data.numIndices  = 1;

    rc::RenderScene renderScene(&device, data);

    rc::RendererServer& server = *device.GetRendererServer();

    server.SetRenderScene(&renderScene);

    for (uint32_t resolution : {64u, 128u, 256u})
    {
        SCOPED_TRACE(resolution);

        rc::VoxelGIRuntimeSettings settings = server.GetVoxelGISettings();

        settings.resolution                 = resolution;


        settings.averagedReflectance        = false;

        settings.reflectanceBudgetBytes     = 0;

        ASSERT_TRUE(server.ApplyVoxelGISettings(settings));

        ASSERT_TRUE(ExecuteVoxelGeneration(device, server));

        rc::VoxelizerBase* owner = server.RequestVoxelizer();

        RHITexture* ownerTexture = owner->GetVoxelTextures().pOwner;

        // A budget one byte short must leave the live voxelizer and its resources in place.
        settings.averagedReflectance    = true;

        settings.reflectanceBudgetBytes = rc::GetVoxelReflectanceRequiredBytes(resolution) - 1;

        EXPECT_FALSE(server.ApplyVoxelGISettings(settings));

        EXPECT_EQ(server.RequestVoxelizer(), owner);

        EXPECT_EQ(server.RequestVoxelizer()->GetVoxelTextures().pOwner, ownerTexture);

        EXPECT_FALSE(server.GetVoxelGISettings().averagedReflectance);

        ASSERT_TRUE(ExecuteVoxelGeneration(device, server));

        settings.reflectanceBudgetBytes = rc::GetVoxelReflectanceRequiredBytes(resolution);

        const bool applied              = server.ApplyVoxelGISettings(settings);

        if (device.GetGPUInfo().maxStorageBufferRange >= uint64_t(resolution) * resolution * resolution * 16)
        {
            EXPECT_TRUE(applied);

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

        settings                     = server.GetVoxelGISettings();

        settings.averagedReflectance = false;

        ASSERT_TRUE(server.ApplyVoxelGISettings(settings));

        ASSERT_TRUE(ExecuteVoxelGeneration(device, server));

        EXPECT_FALSE(server.RequestVoxelizer()->UsesAveragedReflectance());
    }

    device.FlushRHIThread();

    device.WaitForIdle();

    renderScene.Destroy();

    rc::ShaderProgramManager::GetInstance().Destroy();

    device.Destroy();

    zen::rc::RenderConfig::GetInstance().rhiDevice.SetRayTracingDisabled(false);
}

INSTANTIATE_TEST_SUITE_P(SubmissionModes, VoxelReflectanceIntegrationTest, testing::Values(0u, 1u, 2u, 3u));
} // namespace
