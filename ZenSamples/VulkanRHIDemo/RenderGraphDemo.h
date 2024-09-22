#pragma once
#include "Common/Queue.h"
#include "Graphics/RHI/ShaderUtil.h"
#include "Graphics/RenderCore/V2/RenderGraph.h"
#include "Graphics/VulkanRHI/VulkanCommands.h"
#include "Graphics/VulkanRHI/VulkanRHI.h"
#include <fstream>
namespace zen::rc
{
class RenderDevice;
}
using namespace zen;
using namespace zen::rhi;
struct Vertex
{
    Vec3 position{0.0f, 0.0f, 0.0f};
    Vec3 color{0.0f, 0.0f, 0.0f};
    Vec2 uv{0.0f, 0.0f};
};

class Application
{
public:
    Application();

    void Prepare();

    void Run();

    void Destroy();

    ~Application()
    {
        delete m_window;
    }

private:
    static std::vector<uint8_t> LoadSpirvCode(const std::string& name);

    void BuildPipeline();

    void BuildResources();

    void BuildRenderGraph();

    rc::RenderDevice* m_renderDevice{nullptr};
    DynamicRHI* m_RHI{nullptr};
    rc::RenderGraph m_rdg;
    platform::GlfwWindowImpl* m_window{nullptr};
    RHIViewport* m_viewport{nullptr};

    // TextureHandle m_offscreenRT;
    RenderPassHandle m_renderPass;
    FramebufferHandle m_framebuffer;
    PipelineHandle m_gfxPipeline;

    BufferHandle m_vertexBuffer;
    BufferHandle m_indexBuffer;

    DeletionQueue m_deletionQueue;
};