#pragma once
#include "Graphics/RHI/RHIDebug.h"

namespace zen
{
class VulkanDebug : public RHIDebug
{
public:
    explicit VulkanDebug();

    ~VulkanDebug() = default;

    void SetPipelineDebugName(RHIPipeline* pPipelineHandle, NameID debugName) final;

    void SetTextureDebugName(RHITexture* pTexture, NameID debugName) final;
};

} // namespace zen