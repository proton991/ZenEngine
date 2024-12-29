#pragma once
#include "DynamicRHI.h"

namespace zen::rhi
{
class RHIDebug
{
public:
    explicit RHIDebug(DynamicRHI* RHI) : m_RHI(RHI) {}

    virtual ~RHIDebug() = default;

    virtual void SetPipelineDebugName(PipelineHandle pipelineHandle,
                                      const std::string& debugName) = 0;

    virtual void SetTextureDebugName(TextureHandle textureHandle, const std::string& debugName) = 0;

    virtual void SetRenderPassDebugName(RenderPassHandle renderPassHandle,
                                        const std::string& debugName) = 0;

protected:
    DynamicRHI* m_RHI{nullptr};
};
} // namespace zen::rhi