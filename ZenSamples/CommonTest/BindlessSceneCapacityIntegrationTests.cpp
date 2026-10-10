#include "Graphics/RenderCore/V2/RenderDevice.h"
#include "Graphics/RenderCore/V2/RenderScene.h"
#include "Graphics/RenderCore/V2/ShaderProgram.h"
#include "Graphics/RenderCore/V2/Renderer/RendererUtils.h"
#include "Graphics/RHI/RHIOptions.h"
#include "SceneGraph/Scene.h"
#include "SceneGraph/Texture.h"
#include <gtest/gtest.h>
#include <string>

namespace
{
using namespace zen;
using namespace zen::rc;

// A scene of single-texel textures. Scene textures occupy the 2D heap from slot zero.
struct TexelScene
{
    explicit TexelScene(uint32_t textureCount)
    {
        scene.GetAABB() = sg::AABB(Vec3(-1), Vec3(1));

        for (uint32_t texel = 0; texel < textureCount; ++texel)
        {
            UniquePtr<sg::Texture> texture = MakeUnique<sg::Texture>("capacity_texel");

            texture->width                 = 1;

            texture->height                = 1;

            texture->format                = asset::Format::R8G8B8A8_UNORM;

            texture->bytesData             = {255, 0, 0, 255};

            scene.AddComponent(std::move(texture));
        }

        data.pScene      = &scene;

        data.pVertices   = &vertex;

        data.pIndices    = &index;

        data.numVertices = 1;

        data.numIndices  = 1;
    }

    sg::Scene     scene;
    asset::Vertex vertex{};
    uint32_t      index{0};
    SceneData     data{};
};

// One texture more than the default 2D heap holds.
uint32_t GetOversizedTextureCount()
{
    return RHIBindlessHeapCapacities{}.Get(RHIBindlessHeapType::eTexture2D) + 1;
}

// Each device reads the heap request when its backend initializes.
class BindlessSceneCapacityIntegrationTest : public testing::Test
{
protected:
    void SetUp() override
    {
        m_previous = RHIOptions::GetInstance().BindlessHeapCapacities();

        RHIOptions::GetInstance().SetRayTracingDisabled(true);
    }

    void TearDown() override
    {
        DestroyDevice();

        RHIOptions::GetInstance().SetBindlessHeapCapacities(m_previous);

        RHIOptions::GetInstance().SetRayTracingDisabled(false);
    }

    void CreateDevice(const RHIBindlessHeapCapacities& capacities)
    {
        ASSERT_TRUE(RHIOptions::GetInstance().SetBindlessHeapCapacities(capacities));

        m_device = ZEN_NEW() RenderDevice(RHIAPIType::eVulkan, 2, RHIExecutionMode::eThreaded, AsyncComputeMode::eDisabled);

        m_device->Init(nullptr);

        ShaderProgramManager::GetInstance().BuildShaderPrograms(m_device);
    }

    void DestroyDevice()
    {
        if (m_device != nullptr)
        {
            m_device->FlushRHIThread();

            m_device->WaitForIdle();

            ShaderProgramManager::GetInstance().Destroy();

            m_device->Destroy();

            ZEN_DELETE(m_device);

            m_device = nullptr;
        }
    }

    // Binds every scene texture and sampler through the global heaps in one compute pass.
    bool ExecuteSceneBinding(const RenderScene& scene)
    {
        RHIBufferCreateInfo info{};

        info.size         = 3 * sizeof(uint32_t);

        info.allocateType = RHIBufferAllocateType::eGPU;

        info.usageFlags.SetFlag(RHIBufferUsageFlagBits::eStorageBuffer);

        RHIBuffer* indirect = m_device->CreateBuffer(info);

        RenderGraph& graph  = *m_device->GetCurrentFrameRDG();

        bool executed       = indirect != nullptr && graph.Begin();

        if (executed)
        {
            RDGComputePassDesc pass;

            pass.SetShaderProgramName("ResetComputeIndirectSP");

            pass.BindStorageBuffer("IndirectBuffer", indirect, RDGContentGuarantee::eFullWrite);

            BindSceneTextureArray(pass, m_device->CreateSampler({}), scene.GetSceneTextures(), scene.GetSceneSamplers());

            graph.AddComputePass(std::move(pass)).RecordPassCommands(DispatchOnce);

            executed = graph.End() && m_device->ExecuteRenderGraph(graph);

            EXPECT_TRUE(executed) << graph.GetResult().message;

            m_device->FlushRHIThread();

            m_device->WaitForIdle();
        }

        m_device->DestroyBuffer(indirect);

        return executed && !m_device->AreSubmissionsBlocked();
    }

    static void DispatchOnce(RDGPassCmdEncoder& encoder)
    {
        encoder.Dispatch(1, 1, 1);
    }

    RenderDevice*             m_device{nullptr};
    RHIBindlessHeapCapacities m_previous;
};

using BindlessSceneCapacityDeathTest = BindlessSceneCapacityIntegrationTest;

TEST_F(BindlessSceneCapacityDeathTest, SceneLoadAbortsWhenTexturesExceedTheHeap)
{
    const uint32_t textureCount = GetOversizedTextureCount();

    const std::string message =
        std::to_string(textureCount) + " textures, but the bindless texture heap holds " + std::to_string(textureCount - 1);

    // Only the death-test child creates a device; it stops before changing the scene or allocating.
    EXPECT_DEATH(
        {
            TexelScene source(textureCount);

            CreateDevice(RHIBindlessHeapCapacities{});

            RenderScene rejected(m_device, source.data);
        },
        message);
}

TEST_F(BindlessSceneCapacityIntegrationTest, ConfiguredHeapLoadsAndBindsTexturesBeyondTheDefault)
{
    const uint32_t textureCount = GetOversizedTextureCount();

    TexelScene source(textureCount);

    RHIBindlessHeapCapacities configured;

    configured.Set(RHIBindlessHeapType::eTexture2D, textureCount);

    CreateDevice(configured);

    ASSERT_EQ(m_device->GetGPUInfo().bindlessHeapCapacities.Get(RHIBindlessHeapType::eTexture2D), textureCount);

    RenderScene loaded(m_device, source.data);

    loaded.Init();

    EXPECT_EQ(loaded.GetSceneTextures().size(), textureCount);

    EXPECT_TRUE(ExecuteSceneBinding(loaded));

    loaded.Destroy();
}
} // namespace
