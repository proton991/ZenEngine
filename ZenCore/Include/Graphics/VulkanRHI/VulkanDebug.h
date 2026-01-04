#pragma once
#include "Graphics/RHI/RHIDebug.h"

namespace zen
{
class VulkanDebug : public RHIDebug
{
public:
    explicit VulkanDebug();

    ~VulkanDebug() final = default;

    void SetPipelineDebugName(RHIPipeline* pipelineHandle, const std::string& debugName) final;

    void SetTextureDebugName(RHITexture* texture, const std::string& debugName) final;

    // void SetRenderPassDebugName(RenderPassHandle renderPassHandle,
    //                             const std::string& debugName) final;

    void SetDescriptorSetDebugName(RHIDescriptorSet* descriptorSetHandle,
                                   const std::string& debugName) final;
};

} // namespace zen