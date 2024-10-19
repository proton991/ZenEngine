#pragma once
#include "../Application.h"

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

class HelloTriangle : public Application
{
public:
    explicit HelloTriangle(const platform::WindowConfig& windowConfig);

    void Prepare() final;

    void Run() final;

    void Destroy() final;

    ~HelloTriangle() override{};

private:
    void BuildRenderPipeline();

    void LoadResources() final;

    void BuildRenderGraph() final;

    rc::RenderPipeline m_mainRP;

    BufferHandle m_vertexBuffer;
    BufferHandle m_indexBuffer;

    TextureHandle m_texture;
    SamplerHandle m_sampler;
};