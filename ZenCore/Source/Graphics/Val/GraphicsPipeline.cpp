#include "Graphics/Val/GraphicsPipeline.h"
#include "Graphics/Val/PipelineLayout.h"
#include "Graphics/Val/PipelineState.h"
#include "Graphics/Val/Shader.h"
#include "Common/Errors.h"
#include "Graphics/Val/VulkanDebug.h"

namespace zen::val
{
GraphicsPipeline::GraphicsPipeline(const Device& device,
                                   const PipelineLayout& pipelineLayout,
                                   PipelineState& pipelineState,
                                   VkPipelineCache pipelineCache) :
    DeviceObject(device)
{
    std::vector<VkPipelineShaderStageCreateInfo> shaderStageCIs;

    // Create specialization info from tracked state. This is shared by all shaders.
    std::vector<uint8_t> data{};
    std::vector<VkSpecializationMapEntry> mapEntries{};

    const auto scState = pipelineState.GetSpecializationState().GetConstantTable();

    for (const auto& sc : scState)
    {
        mapEntries.push_back({sc.first, static_cast<uint32_t>(data.size()), sc.second.size()});
        data.insert(data.end(), sc.second.begin(), sc.second.end());
    }

    VkSpecializationInfo specializationInfo{};
    specializationInfo.mapEntryCount = static_cast<uint32_t>(mapEntries.size());
    specializationInfo.pMapEntries   = mapEntries.data();
    specializationInfo.dataSize      = data.size();
    specializationInfo.pData         = data.data();

    for (const ShaderModule* shaderModule : pipelineLayout.GetShaderModules())
    {
        VkPipelineShaderStageCreateInfo shaderStageCI{
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        shaderStageCI.stage               = shaderModule->GetStage();
        shaderStageCI.pName               = shaderModule->GetEntryPoint().c_str();
        shaderStageCI.module              = shaderModule->GetHandle();
        shaderStageCI.pSpecializationInfo = &specializationInfo;
        shaderStageCIs.push_back(shaderStageCI);
    }

    auto vertexInputResources =
        pipelineLayout.GetResources(ShaderResourceType::Input, VK_SHADER_STAGE_VERTEX_BIT);
    std::sort(vertexInputResources.begin(), vertexInputResources.end(),
              [](const ShaderResource& lhs, const ShaderResource& rhs) {
                  return lhs.binding == rhs.binding ? lhs.location < rhs.location :
                                                      lhs.binding < rhs.binding;
              });

    std::vector<VkVertexInputBindingDescription> vertexBindingDescriptions;
    std::vector<VkVertexInputAttributeDescription> vertexAttributeDescriptions;

    uint32_t vertexAttributeOffset = 0;
    for (auto& input : vertexInputResources)
    {
        VkVertexInputAttributeDescription attributeDesc{};
        attributeDesc.format   = input.format;
        attributeDesc.binding  = input.binding;
        attributeDesc.location = input.location;
        attributeDesc.offset   = vertexAttributeOffset;
        vertexAttributeOffset += input.size;
        vertexAttributeDescriptions.push_back(attributeDesc);
        if (vertexBindingDescriptions.empty())
        {
            VkVertexInputBindingDescription desc{};
            desc.binding   = input.binding;
            desc.inputRate = VK_VERTEX_INPUT_RATE_VERTEX; // TODO: Support instance draw
            vertexBindingDescriptions.push_back(desc);
        }
        // add new binding
        if (attributeDesc.binding != vertexAttributeDescriptions.back().binding)
        {
            vertexBindingDescriptions.back().stride = vertexAttributeOffset;
            // reset offset to 0, add new binding
            vertexAttributeOffset = 0;
        }
    }
    if (!vertexBindingDescriptions.empty())
    {
        vertexBindingDescriptions.back().stride = vertexAttributeOffset;
        pipelineState.SetVertexInputState({vertexBindingDescriptions, vertexAttributeDescriptions});
    }

    VkPipelineColorBlendAttachmentState pcbAtt{};
    pcbAtt.blendEnable         = true;
    pcbAtt.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    pcbAtt.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    pcbAtt.colorBlendOp        = VK_BLEND_OP_ADD;
    pcbAtt.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    pcbAtt.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    pcbAtt.alphaBlendOp        = VK_BLEND_OP_ADD;
    pcbAtt.colorWriteMask      = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    val::ColorBlendState colorBlendState{};
    colorBlendState.attachments = {pcbAtt};
    pipelineState.SetColorBlendState(std::move(colorBlendState));

    pipelineState.SetDynamicState({VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR});

    auto vertexInputStateCI   = pipelineState.GetVIStateCI();
    auto inputAssemblyStateCI = pipelineState.GetIAStateCI();
    auto rasterizationStateCI = pipelineState.GetRasterizationStateCI();
    auto viewportStateCI      = pipelineState.GetVPStateCI();
    auto multisampleStateCI   = pipelineState.GetMSStateCI();
    auto depthStencilStateCI  = pipelineState.GetDSStateCI();
    auto colorBlendStateCI    = pipelineState.GetCBStateCI();
    auto dynamicStateCI       = pipelineState.GetDynamicStateCI();

    VkGraphicsPipelineCreateInfo pipelineCI{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};

    pipelineCI.stageCount          = static_cast<uint32_t>(shaderStageCIs.size());
    pipelineCI.pStages             = shaderStageCIs.data();
    pipelineCI.pVertexInputState   = &vertexInputStateCI;
    pipelineCI.pInputAssemblyState = &inputAssemblyStateCI;
    pipelineCI.pViewportState      = &viewportStateCI;
    pipelineCI.pRasterizationState = &rasterizationStateCI;
    pipelineCI.pMultisampleState   = &multisampleStateCI;
    pipelineCI.pDepthStencilState  = &depthStencilStateCI;
    pipelineCI.pColorBlendState    = &colorBlendStateCI;
    pipelineCI.pDynamicState       = &dynamicStateCI;
    pipelineCI.layout              = pipelineLayout.GetHandle();
    pipelineCI.renderPass          = pipelineState.GetRPHandle();
    pipelineCI.subpass             = pipelineState.GetSubpassIndex();

    CHECK_VK_ERROR_AND_THROW(vkCreateGraphicsPipelines(m_device.GetHandle(), pipelineCache, 1,
                                                       &pipelineCI, nullptr, &m_handle),
                             "Failed to create graphics pipeline");
}

GraphicsPipeline::GraphicsPipeline(GraphicsPipeline&& other) noexcept :
    DeviceObject(std::move(other))
{}

GraphicsPipeline::~GraphicsPipeline()
{
    if (m_handle != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(m_device.GetHandle(), m_handle, nullptr);
    }
}
} // namespace zen::val