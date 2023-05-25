#include "Graphics/Vulkan/PipelineVK.h"
#include "Common/Error.h"
#include "Graphics/Vulkan/PipelineLayoutVK.h"
#include "Graphics/Vulkan/PipelineState.h"
#include "Graphics/Vulkan/RenderPassVK.h"
#include "Graphics/Vulkan/ShaderModuleVK.h"

namespace zen::vulkan {

Pipeline::Pipeline(const Device& device) : DeviceResource(device, nullptr) {}

GraphicsPipeline::GraphicsPipeline(const Device& device, vk::PipelineCache pipelineCache,
                                   const PipelineState& pipelineState,
                                   const PipelineLayout* pipelineLayout)
    : Pipeline(device) {
  std::vector<uint8_t> data;
  std::vector<vk::SpecializationMapEntry> mapEntries;
  for (auto& [id, value] : pipelineState.GetSpecializationTable()) {
    mapEntries.emplace_back(id, ToU32(data.size()), value.size());
    data.insert(data.end(), value.begin(), value.end());
  }
  auto specializationInfo = vk::SpecializationInfo()
                                .setMapEntries(mapEntries)
                                .setDataSize(data.size())
                                .setPData(data.data());
  std::vector<vk::PipelineShaderStageCreateInfo> shaderStageCIs;
  for (const auto* shaderModule : pipelineLayout->GetShaderModules()) {
    auto info = vk::PipelineShaderStageCreateInfo()
                    .setStage(shaderModule->GetStage())
                    .setModule(shaderModule->GetHandle())
                    .setPSpecializationInfo(&specializationInfo)
                    .setPName(shaderModule->GetEntryPoint().c_str());
    shaderStageCIs.emplace_back(info);
  }
  auto inputAssemblyState = pipelineState.GetInputAssemblyStateCI();
  auto viewPortState      = pipelineState.GetViewportStateCI();
  auto rasterizationState = pipelineState.GetRasterizationStateCI();
  auto multisampleState   = pipelineState.GetMultiSampleStateCI();
  auto depthStencilState  = pipelineState.GetDepthStencilCI();
  auto colorBlendState    = pipelineState.GetColorBlendStateCI();
  auto dynamicState       = pipelineState.GetDynamicStateCI();
  auto pipelineCI         = vk::GraphicsPipelineCreateInfo()
                        .setStages(shaderStageCIs)
                        .setPInputAssemblyState(&inputAssemblyState)
                        .setPViewportState(&viewPortState)
                        .setPRasterizationState(&rasterizationState)
                        .setPMultisampleState(&multisampleState)
                        .setPDepthStencilState(&depthStencilState)
                        .setPColorBlendState(&colorBlendState)
                        .setPDynamicState(&dynamicState)
                        .setRenderPass(pipelineState.GetRenderPass()->GetHandle())
                        .setSubpass(pipelineState.GetSubpassIndex())
                        .setLayout(pipelineLayout->GetHandle());
  auto resultValue = GetDeviceHandle().createGraphicsPipeline(pipelineCache, pipelineCI, nullptr);
  VK_CHECK(resultValue.result, "create graphics pipeline");
  SetHandle(resultValue.value);
}
}  // namespace zen::vulkan