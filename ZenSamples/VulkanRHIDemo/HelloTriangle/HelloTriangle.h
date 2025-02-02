#pragma once
#include "../Application.h"

namespace zen::rc
{
class RenderDevice;
}
using namespace zen;
using namespace zen::rhi;


class HelloTriangleApp : public Application
{
public:
    struct Vertex
    {
        Vec3 position{0.0f, 0.0f, 0.0f};
        Vec3 color{0.0f, 0.0f, 0.0f};
        Vec2 uv{0.0f, 0.0f};
    };

    explicit HelloTriangleApp(const platform::WindowConfig& windowConfig);

    void Prepare() final;

    void Run() final;

    void Destroy() final;

    ~HelloTriangleApp() override {};

private:
    void BuildGraphicsPasses();

    void LoadResources() final;

    void BuildRenderGraph() final;

    rc::GraphicsPass m_gfxPass;

    BufferHandle m_vertexBuffer;
    BufferHandle m_indexBuffer;

    TextureHandle m_texture;
    SamplerHandle m_sampler;
};