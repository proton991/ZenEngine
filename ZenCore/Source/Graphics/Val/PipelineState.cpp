#include "Graphics/Val/PipelineState.h"
#include "Graphics/Val/RenderPass.h"

namespace zen::val
{
void PipelineState::SetVertexInputState(VertexInputState&& state)
{
    m_vertexInputState = std::move(state);
}

void PipelineState::SetInputAssemblyState(InputAssemblyState&& state)
{
    m_inputAssemblyState = std::move(state);
}

void PipelineState::SetViewportState(ViewportState&& state)
{
    m_viewportState = std::move(state);
}

void PipelineState::SetRasterizationState(RasterizationState&& state)
{
    m_rasterizationState = std::move(state);
}

void PipelineState::SetMultiSampleState(MultisampleState&& state)
{
    m_multiSampleState = std::move(state);
}

void PipelineState::SetDepthStencilState(DepthStencilState&& state)
{
    m_depthStencilState = std::move(state);
}

void PipelineState::SetColorBlendState(ColorBlendState&& state)
{
    m_colorBlendState = std::move(state);
}

void PipelineState::SetDynamicState(std::vector<VkDynamicState>&& states)
{
    m_dynamicStates = std::move(states);
}

void PipelineState::SetRenderPass(VkRenderPass renderPass)
{
    m_renderPassHandle = renderPass;
}

void PipelineState::SetSubpassIndex(uint32_t index)
{
    m_subpassIndex = index;
}

VkPipelineVertexInputStateCreateInfo PipelineState::GetVIStateCI() const
{
    VkPipelineVertexInputStateCreateInfo info{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    info.pVertexAttributeDescriptions = m_vertexInputState.attributes.data();
    info.vertexAttributeDescriptionCount =
        static_cast<uint32_t>(m_vertexInputState.attributes.size());
    info.pVertexBindingDescriptions    = m_vertexInputState.bindings.data();
    info.vertexBindingDescriptionCount = static_cast<uint32_t>(m_vertexInputState.bindings.size());
    return info;
}

VkPipelineInputAssemblyStateCreateInfo PipelineState::GetIAStateCI() const
{
    VkPipelineInputAssemblyStateCreateInfo info{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    info.primitiveRestartEnable = m_inputAssemblyState.primitiveRestartEnable;
    info.topology               = m_inputAssemblyState.topology;
    return info;
}

VkPipelineViewportStateCreateInfo PipelineState::GetVPStateCI() const
{
    VkPipelineViewportStateCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    info.scissorCount  = m_viewportState.scissorCount;
    info.viewportCount = m_viewportState.viewportCount;
    return info;
}

VkPipelineRasterizationStateCreateInfo PipelineState::GetRasterizationStateCI() const
{
    VkPipelineRasterizationStateCreateInfo info{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    info.depthClampEnable        = m_rasterizationState.depthClampEnable;
    info.rasterizerDiscardEnable = m_rasterizationState.rasterizerDiscardEnable;
    info.polygonMode             = m_rasterizationState.polygonMode;
    info.cullMode                = m_rasterizationState.cullMode;
    info.frontFace               = m_rasterizationState.frontFace;
    info.depthBiasConstantFactor = m_rasterizationState.depthBiasConstantFactor;
    info.depthBiasClamp          = m_rasterizationState.depthBiasClamp;
    info.depthBiasSlopeFactor    = m_rasterizationState.depthBiasSlopeFactor;
    info.depthBiasEnable         = m_rasterizationState.depthBiasEnable;
    info.lineWidth               = m_rasterizationState.lineWidth;
    return info;
}

VkPipelineMultisampleStateCreateInfo PipelineState::GetMSStateCI() const
{
    VkPipelineMultisampleStateCreateInfo info{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    info.alphaToCoverageEnable = m_multiSampleState.alphaToCoverageEnable;
    info.alphaToOneEnable      = m_multiSampleState.alphaToOneEnable;
    info.minSampleShading      = m_multiSampleState.minSampleShading;
    info.rasterizationSamples  = m_multiSampleState.rasterizationSamples;
    info.sampleShadingEnable   = m_multiSampleState.sampleShadingEnable;
    return info;
}

VkPipelineDepthStencilStateCreateInfo PipelineState::GetDSStateCI() const
{
    VkPipelineDepthStencilStateCreateInfo info{
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    info.depthBoundsTestEnable = m_depthStencilState.depthBoundsTestEnable;
    info.depthTestEnable       = m_depthStencilState.depthTestEnable;
    info.depthWriteEnable      = m_depthStencilState.depthWriteEnable;
    info.depthCompareOp        = m_depthStencilState.depthCompareOp;
    info.stencilTestEnable     = m_depthStencilState.stencilTestEnable;
    info.minDepthBounds        = 0.0f;
    info.maxDepthBounds        = 1.0f;
    info.front                 = m_depthStencilState.front;
    info.back                  = m_depthStencilState.back;
    return info;
}

VkPipelineColorBlendStateCreateInfo PipelineState::GetCBStateCI() const
{
    VkPipelineColorBlendStateCreateInfo info{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    info.attachmentCount = static_cast<uint32_t>(m_colorBlendState.attachments.size());
    info.pAttachments    = m_colorBlendState.attachments.data();
    info.logicOp         = m_colorBlendState.logicOp;
    info.logicOpEnable   = m_colorBlendState.logicOpEnable;
    return info;
}

VkPipelineDynamicStateCreateInfo PipelineState::GetDynamicStateCI() const
{
    VkPipelineDynamicStateCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    info.dynamicStateCount = static_cast<uint32_t>(m_dynamicStates.size());
    info.pDynamicStates    = m_dynamicStates.data();
    return info;
}

VkRenderPass PipelineState::GetRPHandle() const
{
    return m_renderPassHandle;
}

uint32_t PipelineState::GetSubpassIndex() const
{
    return m_subpassIndex;
}
} // namespace zen::val