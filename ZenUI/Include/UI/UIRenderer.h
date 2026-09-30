#pragma once

#include "UI/UIDrawPacket.h"
#include "Graphics/RenderCore/V2/RenderDevice.h"

namespace zen::ui
{
// Only GPU resources and draw translation; owns no context, input, or panels.
class UIRenderer
{
public:
    explicit UIRenderer(rc::RenderDevice& device);

    UIRenderer(const UIRenderer&) = delete;

    UIRenderer& operator=(const UIRenderer&) = delete;

    bool Init(ImFontAtlas& fonts);

    void Destroy();

    bool BuildRenderGraph(rc::RenderGraph& graph, RHIViewport& viewport, const ImDrawData& data);

private:
    struct FrameBuffers
    {
        RHIBuffer* vertices{nullptr};
        RHIBuffer* indices{nullptr};
        uint32_t vertexCapacity{0};
        uint32_t indexCapacity{0};
    };

    bool GrowBuffer(RHIBuffer*& buffer,
                    uint32_t& capacity,
                    uint32_t required,
                    RHIBufferUsageFlagBits usage);

    rc::RenderDevice& m_device;
    RHITexture* m_fontTexture{nullptr};
    RHISampler* m_sampler{nullptr};
    HeapVector<FrameBuffers> m_frames;
    ImTextureID m_fontId{ImTextureID_Invalid};
};
} // namespace zen::ui
