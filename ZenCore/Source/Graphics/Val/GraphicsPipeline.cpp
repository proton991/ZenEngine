#include "Graphics/Val/GraphicsPipeline.h"
#include "Graphics/Val/PipelineLayout.h"
#include "Graphics/Val/PipelineState.h"
#include "Graphics/Val/Shader.h"
#include "Common/Errors.h"
#include "Graphics/Val/VulkanDebug.h"

namespace zen::val
{
GraphicsPipeline::GraphicsPipeline(Device& device, const PipelineLayout& pipelineLayout, const PipelineState& pipelineState, const std::string& debugName, VkPipelineCache pipelineCache) :
    DeviceObject(device)
{
    std::vector<VkPipelineShaderStageCreateInfo> shaderStageCIs;

    // Create specialization info from tracked state. This is shared by all shaders.
    std::vector<uint8_t>                  data{};
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
        VkPipelineShaderStageCreateInfo shaderStageCI{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        shaderStageCI.stage               = shaderModule->GetStage();
        shaderStageCI.pName               = shaderModule->GetEntryPoint().c_str();
        shaderStageCI.module              = shaderModule->GetHandle();
        shaderStageCI.pSpecializationInfo = &specializationInfo;
        shaderStageCIs.push_back(shaderStageCI);
    }

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

    CHECK_VK_ERROR_AND_THROW(vkCreateGraphicsPipelines(m_device.GetHandle(), pipelineCache, 1, &pipelineCI, nullptr, &m_handle), "Failed to create graphics pipeline");

    SetPipelineName(m_device.GetHandle(), m_handle, debugName.c_str());
}

GraphicsPipeline::~GraphicsPipeline()
{
    if (m_handle != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(m_device.GetHandle(), m_handle, nullptr);
    }
}
} // namespace zen::val