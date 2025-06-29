#pragma once
#include "DynamicRHI.h"

namespace zen::rhi
{
class RHIDebug
{
public:
    static RHIDebug* Create(DynamicRHI* RHI);

    virtual ~RHIDebug() = default;

    virtual void SetPipelineDebugName(PipelineHandle pipelineHandle,
                                      const std::string& debugName) = 0;

    virtual void SetTextureDebugName(TextureHandle textureHandle, const std::string& debugName) = 0;

    virtual void SetRenderPassDebugName(RenderPassHandle renderPassHandle,
                                        const std::string& debugName) = 0;

    virtual void SetDescriptorSetDebugName(DescriptorSetHandle descriptorSetHandle,
                                           const std::string& debugName) = 0;

protected:
    explicit RHIDebug(DynamicRHI* RHI) : m_RHI(RHI) {}

    DynamicRHI* m_RHI{nullptr};
};
} // namespace zen::rhi