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

struct CameraUniformData
{
    Mat4 projViewMatrix{1.0f};
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
    rc::RenderGraph m_rdg;
    platform::GlfwWindowImpl* m_window{nullptr};
    RHIViewport* m_viewport{nullptr};

    rc::RenderPipeline m_mainRP;

    BufferHandle m_vertexBuffer;
    BufferHandle m_indexBuffer;

    BufferHandle m_cameraUBO;
    CameraUniformData m_cameraData;

    UniquePtr<sys::Camera> m_camera;
    UniquePtr<platform::Timer> m_timer;
};