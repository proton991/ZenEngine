#pragma once
#include "DynamicRHI.h"

namespace zen
{
class RHIDebug
{
public:
    static RHIDebug* Create();

    virtual ~RHIDebug() = default;

    virtual void SetPipelineDebugName(RHIPipeline* pPipelineHandle,
                                      const std::string& debugName) = 0;

    virtual void SetTextureDebugName(RHITexture* pTexture, const std::string& debugName) = 0;

    // virtual void SetRenderPassDebugName(RenderPassHandle renderPassHandle,
    //                                     const std::string& debugName) = 0;

    virtual void SetDescriptorSetDebugName(RHIDescriptorSet* pDescriptorSetHandle,
                                           const std::string& debugName) = 0;

protected:
    // explicit RHIDebug(DynamicRHI* RHI) : m_RHI(RHI) {}
    RHIDebug() {}

    // DynamicRHI* m_RHI{nullptr};
};
} // namespace zen