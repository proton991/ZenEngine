#pragma once
#include "Graphics/RHI/RHIDebug.h"

namespace zen::rhi
{
class VulkanDebug : public RHIDebug
{
public:
    explicit VulkanDebug(DynamicRHI* RHI);

    ~VulkanDebug() final = default;

    void SetPipelineDebugName(PipelineHandle pipelineHandle, const std::string& debugName) final;

    void SetTextureDebugName(TextureHandle textureHandle, const std::string& debugName) final;

    void SetRenderPassDebugName(RenderPassHandle renderPassHandle,
                                const std::string& debugName) final;

    void SetDescriptorSetDebugName(DescriptorSetHandle descriptorSetHandle,
                                   const std::string& debugName) final;
};

} // namespace zen::rhi