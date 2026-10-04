#pragma once

#include "UI/UIDrawPacket.h"
#include "Graphics/RenderCore/V2/RenderDevice.h"

namespace zen::ui
{
struct UIRenderTarget
{
    RHITexture* color{nullptr};
    uint32_t    width{0};
    uint32_t    height{0};
};

// Only GPU resources and draw translation; owns no context, input, or panels.
class UIRenderer
{
public:
    explicit UIRenderer(rc::RenderDevice& device);

    UIRenderer(const UIRenderer&)            = delete;

    UIRenderer& operator=(const UIRenderer&) = delete;

    bool Init();

    void Destroy();

    UITextureHandle RegisterTexture(RHITexture* texture, RHISampler* sampler);

    void UnregisterTexture(UITextureHandle handle);

    bool IsTextureValid(UITextureHandle handle) const;

    bool BuildRenderGraph(rc::RenderGraph& graph, const UIRenderTarget& target, const UIDrawPacket& packet);

private:
    struct FrameBuffers
    {
        RHIBuffer* vertices{nullptr};
        RHIBuffer* indices{nullptr};
        uint32_t   vertexCapacity{0};
        uint32_t   indexCapacity{0};
    };

    struct TextureSlot
    {
        RHIResourcePtr<RHITexture> texture;
        RHIResourcePtr<RHISampler> sampler;
        uint32_t                   generation{1};
    };

    bool GrowBuffer(RHIBuffer*& buffer, uint32_t& capacity, uint32_t required, RHIBufferUsageFlagBits usage);

    rc::RenderDevice&        m_device;
    HeapVector<FrameBuffers> m_frames;
    HeapVector<TextureSlot>  m_textures;
};
} // namespace zen::ui
