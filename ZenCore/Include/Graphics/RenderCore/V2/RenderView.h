#pragma once
#include "Graphics/RHI/RHIResource.h"

namespace zen::rc
{
// Borrowed targets for graph declaration. RDG retains declared resources. Extent
// belongs to the scene view and is independent of the native presentation window.
struct RenderView
{
    RHITexture* color{nullptr};
    RHITexture* depth{nullptr};
    uint32_t    width{0};
    uint32_t    height{0};

    // Snapshot the current window buffers for this frame; do not cache across frames or resize.
    static RenderView FromViewport(RHIViewport& viewport)
    {
        return {viewport.GetColorBackBuffer(), viewport.GetDepthStencilBackBuffer(), viewport.GetWidth(), viewport.GetHeight()};
    }

    uint32_t GetWidth() const
    {
        return width;
    }

    uint32_t GetHeight() const
    {
        return height;
    }

    RHITexture* GetColorTarget() const
    {
        return color;
    }

    RHITexture* GetDepthTarget() const
    {
        return depth;
    }

    DataFormat GetDepthStencilFormat() const
    {
        return depth->GetFormat();
    }
};
} // namespace zen::rc
