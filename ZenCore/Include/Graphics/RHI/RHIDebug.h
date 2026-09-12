#pragma once
#include "DynamicRHI.h"
#include "Templates/NameID.h"

namespace zen
{
class RHIDebug
{
public:
    static RHIDebug* Create();

    virtual ~RHIDebug() = default;

    virtual void SetPipelineDebugName(RHIPipeline* pPipelineHandle, NameID debugName) = 0;

    virtual void SetTextureDebugName(RHITexture* pTexture, NameID debugName) = 0;

protected:
    // explicit RHIDebug(DynamicRHI* RHI) : m_RHI(RHI) {}
    RHIDebug() {}

    // DynamicRHI* m_RHI{nullptr};
};
} // namespace zen