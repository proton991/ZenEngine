#include "UI/UIRenderer.h"
#include "Graphics/RHI/RHIOptions.h"
#include <gtest/gtest.h>
#include <cstring>

namespace zen::ui
{
namespace
{
UIDrawPacket Triangle(UITextureHandle texture)
{
    UIDrawPacket packet;

    packet.vertices.push_back({{0, 0}, {0, 0}, 0xffffffff});

    packet.vertices.push_back({{64, 0}, {1, 0}, 0xffffffff});

    packet.vertices.push_back({{0, 64}, {0, 1}, 0xffffffff});

    for (uint32_t index = 0; index < 3; ++index)
    {
        packet.indices.push_back(index);
    }

    packet.commands.push_back({texture, 0, 0, 64, 64, 3, 0, 0});

    packet.projection[0] = packet.projection[1] = 2.0f / 64.0f;

    packet.projection[2] = packet.projection[3] = -1.0f;

    return packet;
}

TEST(UIRenderingContract, RejectsOutOfBoundsIndexAndClipWithoutImGui)
{
    UIDrawPacket packet = Triangle({1});

    EXPECT_TRUE(ValidateUIDrawPacket(packet, 64, 64));

    packet.commands[0].vertexOffset = 1;

    EXPECT_FALSE(ValidateUIDrawPacket(packet, 64, 64));

    packet.commands[0].vertexOffset = 0;

    packet.commands[0].maxX         = 65;

    EXPECT_FALSE(ValidateUIDrawPacket(packet, 64, 64));
}

class UIRenderingNative : public testing::TestWithParam<RHIExecutionMode>
{};

TEST_P(UIRenderingNative, TextureGenerationsAndQueuedResourcesWithoutImGuiOrWindow)
{
    RHIOptions::GetInstance().SetRayTracingEnabled(false);

    rc::RenderDevice device(RHIAPIType::eVulkan, 2, GetParam());

    device.Init(nullptr);

    UIRenderer renderer(device);

    ASSERT_TRUE(renderer.Init());

    rc::TextureFormat format;

    format.width = format.height = 64;

    format.depth                 = 1;

    format.format                = DataFormat::eR8G8B8A8UNORM;

    RHITexture* target           = device.CreateTextureColorRT(format, {.copyUsage = true}, "UITestTarget");

    format.width = format.height = 1;

    RHITexture* texture          = device.CreateTextureSampled(format, {.copyUsage = true}, "UITestImage");

    RHISampler* sampler          = device.CreateSampler(RHISamplerCreateInfo::CreateLinearRepeat());

    const UITextureHandle old    = renderer.RegisterTexture(texture, sampler);

    renderer.UnregisterTexture(old);

    const UITextureHandle image = renderer.RegisterTexture(texture, sampler);

    EXPECT_NE(image, old);

    EXPECT_FALSE(renderer.IsTextureValid(old));

    const uint8_t white[4] = {255, 255, 255, 255};

    RHIBufferTextureCopyRegion region{};

    region.textureSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);

    region.textureSubresources.layerCount = 1;

    region.textureSize                    = {1, 1, 1};

    device.UpdateTexture(texture, MakeVecView(&region, 1), sizeof(white), white);

    {
        rc::RenderGraph graph("NeutralUI");

        ASSERT_TRUE(graph.Begin());

        graph.AddTransferPass("ClearUI").ClearTexture(target, Color(0, 0, 0, 1));

        EXPECT_FALSE(renderer.BuildRenderGraph(graph, {target, 64, 64}, Triangle(old)));

        ASSERT_TRUE(renderer.BuildRenderGraph(graph, {target, 64, 64}, Triangle(image)));

        // Already declared graph bindings retain the image independently of registry slots.
        renderer.UnregisterTexture(image);

        device.DestroyTexture(texture);

        ASSERT_TRUE(graph.End());

        EXPECT_TRUE(device.ExecuteRenderGraph(graph));

        device.NextFrame();

        device.WaitForPreviousFrames();
    }

    renderer.Destroy();

    device.DestroyTexture(target);

    rc::ShaderProgramManager::GetInstance().Destroy();

    device.Destroy();
}

// Draws keep their order and share a pass until a new image would exceed the shader's
// texture slots, instead of starting a pass for every change of image.
TEST_P(UIRenderingNative, BatchesDrawsAcrossImagesWithinTextureSlots)
{
    RHIOptions::GetInstance().SetRayTracingEnabled(false);

    rc::RenderDevice device(RHIAPIType::eVulkan, 2, GetParam());

    device.Init(nullptr);

    UIRenderer renderer(device);

    ASSERT_TRUE(renderer.Init());

    rc::TextureFormat format;

    format.width = format.height = 64;

    format.depth                 = 1;

    format.format                = DataFormat::eR8G8B8A8UNORM;

    RHITexture* target           = device.CreateTextureColorRT(format, {.copyUsage = true}, "UIBatchTarget");

    format.width = format.height = 1;

    RHISampler* sampler          = device.CreateSampler(RHISamplerCreateInfo::CreateLinearRepeat());

    HeapVector<RHITexture*> textures;

    HeapVector<UITextureHandle> handles;

    const uint8_t white[4] = {255, 255, 255, 255};

    RHIBufferTextureCopyRegion region{};

    region.textureSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);

    region.textureSubresources.layerCount = 1;

    region.textureSize                    = {1, 1, 1};

    for (uint32_t index = 0; index < 20; ++index)
    {
        textures.push_back(device.CreateTextureSampled(format, {.copyUsage = true}, "UIBatchImage"));

        device.UpdateTexture(textures.back(), MakeVecView(&region, 1), sizeof(white), white);

        handles.push_back(renderer.RegisterTexture(textures.back(), sampler));
    }

    // Twenty images, then the first again: 16 commands fit the first pass, 5 the second.
    UIDrawPacket packet = Triangle(handles[0]);

    packet.commands.clear();

    for (uint32_t index = 0; index < 21; ++index)
    {
        packet.commands.push_back({handles[index % 20], 0, 0, 64, 64, 3, 0, 0});
    }

    {
        rc::RenderGraph graph("BatchedUI");

        ASSERT_TRUE(graph.Begin());

        graph.AddTransferPass("ClearUI").ClearTexture(target, Color(0, 0, 0, 1));

        ASSERT_TRUE(renderer.BuildRenderGraph(graph, {target, 64, 64}, packet));

        ASSERT_TRUE(graph.End());

        EXPECT_TRUE(device.ExecuteRenderGraph(graph));

        // The clear and two UI passes; compile statistics exist once the graph has executed.
        EXPECT_EQ(graph.GetCompileStats().passCount, 3u);

        device.NextFrame();

        device.WaitForPreviousFrames();
    }

    for (uint32_t index = 0; index < textures.size(); ++index)
    {
        renderer.UnregisterTexture(handles[index]);

        device.DestroyTexture(textures[index]);
    }

    renderer.Destroy();

    device.DestroyTexture(target);

    rc::ShaderProgramManager::GetInstance().Destroy();

    device.Destroy();
}

INSTANTIATE_TEST_SUITE_P(Execution, UIRenderingNative, testing::Values(RHIExecutionMode::eInline, RHIExecutionMode::eThreaded));
} // namespace
} // namespace zen::ui
