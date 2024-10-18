#pragma once
#include "Common/Queue.h"
#include "Graphics/RHI/ShaderUtil.h"
#include "Graphics/RenderCore/V2/RenderDevice.h"
#include "Graphics/RenderCore/V2/RenderGraph.h"
#include "Graphics/VulkanRHI/VulkanRHI.h"
#include "Platform/Timer.h"
#include "Systems/Camera.h"

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
    void BuildPipeline();

    void BuildResources();

    void BuildRenderGraph();

    rc::RenderDevice* m_renderDevice{nullptr};
    UniquePtr<rc::RenderGraph> m_rdg;
    platform::GlfwWindowImpl* m_window{nullptr};
    RHIViewport* m_viewport{nullptr};

    rc::RenderPipeline m_mainRP;

    BufferHandle m_vertexBuffer;
    BufferHandle m_indexBuffer;

    TextureHandle m_texture;
    SamplerHandle m_sampler;

    BufferHandle m_cameraUBO;

    UniquePtr<sys::Camera> m_camera;
    UniquePtr<platform::Timer> m_timer;
};