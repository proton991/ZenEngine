#pragma once
#include <unordered_map>
#include <vulkan/vulkan.hpp>
#include "Common/Helpers.h"

namespace zen::vulkan {
class RenderPass;
struct VertexInputState {
  std::vector<vk::VertexInputBindingDescription> bindings;
  std::vector<vk::VertexInputAttributeDescription> attributes;
  friend bool operator!=(const VertexInputState& lhs, const VertexInputState& rhs) {
    return lhs.bindings != rhs.bindings || lhs.attributes != rhs.attributes;
  }
};

struct InputAssemblyState {
  vk::PrimitiveTopology topology{vk::PrimitiveTopology::eTriangleList};
  vk::Bool32 primitiveRestartEnable{false};
  friend bool operator!=(const InputAssemblyState& lhs, const InputAssemblyState& rhs) {
    return lhs.topology != rhs.topology || lhs.primitiveRestartEnable != rhs.primitiveRestartEnable;
  }
};

struct ViewportState {
  uint32_t viewPortCount{1};
  uint32_t scissorCount{1};
  friend bool operator!=(const ViewportState& lhs, const ViewportState& rhs) {
    return lhs.viewPortCount != rhs.viewPortCount || lhs.scissorCount != rhs.scissorCount;
  }
};

struct RasterizationState {
  vk::Bool32 depthClampEnable{false};
  vk::Bool32 rasterizerDiscardEnable{false};
  vk::PolygonMode polygonMode{vk::PolygonMode::eFill};
  vk::CullModeFlags cullMode{vk::CullModeFlagBits::eBack};
  vk::FrontFace frontFace{vk::FrontFace::eCounterClockwise};
  vk::Bool32 depthBiasEnable{false};
  float depthBiasConstantFactor{1.0f};
  float depthBiasClamp{1.0f};
  float depthBiasSlopeFactor{1.0f};
  float lineWidth{1.0f};
  friend bool operator!=(const RasterizationState& lhs, const RasterizationState& rhs) {
    return std::tie(lhs.depthClampEnable, lhs.rasterizerDiscardEnable, lhs.polygonMode,
                    lhs.cullMode, lhs.frontFace, lhs.depthBiasEnable) !=
           std::tie(rhs.depthClampEnable, rhs.rasterizerDiscardEnable, rhs.polygonMode,
                    rhs.cullMode, rhs.frontFace, rhs.depthBiasEnable);
  }
};

struct MultisampleState {
  vk::SampleCountFlagBits rasterizationSamples{vk::SampleCountFlagBits::e1};
  vk::Bool32 sampleShadingEnable{false};
  float minSampleShading{0.0f};
  vk::SampleMask sampleMask{0};
  vk::Bool32 alphaToCoverageEnable{false};
  vk::Bool32 alphaToOneEnable{false};
  friend bool operator!=(const MultisampleState& lhs, const MultisampleState& rhs) {
    return std::tie(lhs.rasterizationSamples, lhs.sampleShadingEnable, lhs.minSampleShading,
                    lhs.sampleMask, lhs.alphaToCoverageEnable, lhs.alphaToOneEnable) !=
           std::tie(rhs.rasterizationSamples, rhs.sampleShadingEnable, rhs.minSampleShading,
                    rhs.sampleMask, rhs.alphaToCoverageEnable, rhs.alphaToOneEnable);
  }
};

vk::StencilOpState DefaultStencilOpState() {
  vk::StencilOpState state{};
  state.failOp      = vk::StencilOp::eReplace;
  state.passOp      = vk::StencilOp::eReplace;
  state.depthFailOp = vk::StencilOp::eReplace;
  state.compareOp   = vk::CompareOp::eNever;
  state.compareMask = ~0u;
  state.writeMask   = ~0U;
  state.reference   = ~0U;
  return state;
}
struct DepthStencilState {
  vk::Bool32 depthTestEnable{true};
  vk::Bool32 depthWriteEnable{true};
  // Note: Using Reversed depth-buffer for increased precision, so Greater depth values are kept
  vk::CompareOp depthCompareOp{vk::CompareOp::eGreater};
  vk::Bool32 depthBoundsTestEnable{false};
  vk::Bool32 stencilTestEnable{false};
  vk::StencilOpState front{DefaultStencilOpState()};
  vk::StencilOpState back{DefaultStencilOpState()};
  friend bool operator!=(const DepthStencilState& lhs, const DepthStencilState& rhs) {
    return std::tie(lhs.depthTestEnable, lhs.depthWriteEnable, lhs.depthCompareOp,
                    lhs.depthBoundsTestEnable, lhs.stencilTestEnable) !=
               std::tie(rhs.depthTestEnable, rhs.depthWriteEnable, rhs.depthCompareOp,
                        rhs.depthBoundsTestEnable, rhs.stencilTestEnable) ||
           lhs.back != rhs.back || lhs.front != rhs.front;
  }
};
vk::PipelineColorBlendAttachmentState DefaultPipelineColorBlendAttachmentState() {
  auto state =
      vk::PipelineColorBlendAttachmentState()
          .setBlendEnable(false)
          .setSrcColorBlendFactor(vk::BlendFactor::eOne)
          .setDstColorBlendFactor(vk::BlendFactor::eZero)
          .setColorBlendOp(vk::BlendOp::eAdd)
          .setSrcAlphaBlendFactor(vk::BlendFactor::eOne)
          .setDstAlphaBlendFactor(vk::BlendFactor::eZero)
          .setAlphaBlendOp(vk::BlendOp::eAdd)
          .setColorWriteMask(vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                             vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA);
  return state;
}

struct ColorBlendState {
  vk::Bool32 logicOpEnable{false};
  vk::LogicOp logicOp{vk::LogicOp::eClear};
  std::vector<vk::PipelineColorBlendAttachmentState> attachments;
  friend bool operator!=(const ColorBlendState& lhs, const ColorBlendState& rhs) {
    return std::tie(lhs.logicOp, lhs.logicOpEnable) != std::tie(rhs.logicOp, rhs.logicOpEnable) ||
           lhs.attachments.size() != rhs.attachments.size() ||
           !std::equal(lhs.attachments.begin(), lhs.attachments.end(), rhs.attachments.begin());
  }
};

struct DynamicState {
  std::array<vk::DynamicState, 9> dynamicStates{
      vk::DynamicState::eViewport,           vk::DynamicState::eScissor,
      vk::DynamicState::eLineWidth,          vk::DynamicState::eDepthBias,
      vk::DynamicState::eBlendConstants,     vk::DynamicState::eDepthBounds,
      vk::DynamicState::eStencilCompareMask, vk::DynamicState::eStencilWriteMask,
      vk::DynamicState::eStencilReference,
  };
};

class SpecializationState {
public:
  void Reset() {
    if (m_dirty) {
      m_constantTable.clear();
    }
    m_dirty = false;
  }
  [[nodiscard]] auto IsDirty() const { return m_dirty; }

  template <class T>
  void SetConstant(uint32_t id, const T& value) {
    auto it    = m_constantTable.find(id);
    auto bytes = ToBytes(value);
    if (it != m_constantTable.end() && it->second == bytes) {
      return;
    }
    m_constantTable[id] = bytes;
    m_dirty             = true;
  }

  [[nodiscard]] auto& GetConstantTable() const { return m_constantTable; }

private:
  bool m_dirty{false};
  std::unordered_map<uint32_t, std::vector<uint8_t>> m_constantTable;
};
class PipelineState {
public:
  [[nodiscard]] vk::PipelineVertexInputStateCreateInfo GetPipelineVertexInputCI() const {
    auto createInfo = vk::PipelineVertexInputStateCreateInfo()
                          .setVertexAttributeDescriptions(m_vertexInputState.attributes)
                          .setVertexBindingDescriptions(m_vertexInputState.bindings);
    return createInfo;
  }
  PipelineState& SetVertexInput(const VertexInputState& vertexInputState) {
    if (vertexInputState != m_vertexInputState) {
      m_vertexInputState = vertexInputState;
      m_dirty            = true;
    }
    return *this;
  }

  [[nodiscard]] vk::PipelineInputAssemblyStateCreateInfo GetInputAssemblyStateCI() const {
    auto createInfo = vk::PipelineInputAssemblyStateCreateInfo()
                          .setTopology(m_inputAssemblyState.topology)
                          .setPrimitiveRestartEnable(m_inputAssemblyState.primitiveRestartEnable);
    return createInfo;
  }
  PipelineState& SetInputAssemblyState(const InputAssemblyState& inputAssemblyState) {
    if (inputAssemblyState != m_inputAssemblyState) {
      m_inputAssemblyState = inputAssemblyState;
      m_dirty              = true;
    }
    return *this;
  }

  [[nodiscard]] vk::PipelineViewportStateCreateInfo GetViewportStateCI() const {
    // use dynamic viewport and scissor
    auto createInfo = vk::PipelineViewportStateCreateInfo()
                          .setViewportCount(m_viewportState.viewPortCount)
                          .setScissorCount(m_viewportState.scissorCount);
    return createInfo;
  }
  PipelineState& SetViewportState(const ViewportState& viewportState) {
    if (viewportState != m_viewportState) {
      m_viewportState = viewportState;
      m_dirty         = true;
    }
    return *this;
  }

  [[nodiscard]] vk::PipelineRasterizationStateCreateInfo GetRasterizationStateCI() const {
    // use dynamic viewport and scissor
    auto createInfo = vk::PipelineRasterizationStateCreateInfo()
                          .setDepthClampEnable(m_rasterizationState.depthClampEnable)
                          .setRasterizerDiscardEnable(m_rasterizationState.rasterizerDiscardEnable)
                          .setPolygonMode(m_rasterizationState.polygonMode)
                          .setCullMode(m_rasterizationState.cullMode)
                          .setFrontFace(m_rasterizationState.frontFace)
                          .setDepthBiasEnable(m_rasterizationState.depthBiasEnable)
                          .setDepthBiasClamp(m_rasterizationState.depthBiasClamp)
                          .setDepthBiasSlopeFactor(m_rasterizationState.depthBiasSlopeFactor)
                          .setDepthBiasConstantFactor(m_rasterizationState.depthBiasConstantFactor);
    return createInfo;
  }
  PipelineState& SetRasterizationState(const RasterizationState& rasterizationState) {
    if (rasterizationState != m_rasterizationState) {
      m_rasterizationState = rasterizationState;
      m_dirty              = true;
    }
    return *this;
  }

  [[nodiscard]] vk::PipelineMultisampleStateCreateInfo GetMultiSampleStateCI() const {
    auto createInfo = vk::PipelineMultisampleStateCreateInfo()
                          .setRasterizationSamples(m_multiSampleState.rasterizationSamples)
                          .setSampleShadingEnable(m_multiSampleState.sampleShadingEnable)
                          .setMinSampleShading(m_multiSampleState.minSampleShading)
                          .setPSampleMask(&m_multiSampleState.sampleMask)
                          .setAlphaToCoverageEnable(m_multiSampleState.alphaToCoverageEnable)
                          .setAlphaToOneEnable(m_multiSampleState.alphaToOneEnable);
    return createInfo;
  }
  PipelineState& SetMultiSampleState(const MultisampleState& multisampleState) {
    if (multisampleState != m_multiSampleState) {
      m_multiSampleState = multisampleState;
      m_dirty            = true;
    }
    return *this;
  }

  [[nodiscard]] vk::PipelineDepthStencilStateCreateInfo GetDepthStencilCI() const {
    auto createInfo = vk::PipelineDepthStencilStateCreateInfo()
                          .setDepthTestEnable(m_depthStencilState.depthTestEnable)
                          .setDepthWriteEnable(m_depthStencilState.depthWriteEnable)
                          .setDepthCompareOp(m_depthStencilState.depthCompareOp)
                          .setDepthBoundsTestEnable(m_depthStencilState.depthBoundsTestEnable)
                          .setStencilTestEnable(m_depthStencilState.stencilTestEnable)
                          .setFront(m_depthStencilState.front)
                          .setBack(m_depthStencilState.back);
    return createInfo;
  }
  PipelineState& SetDepthStencilState(const DepthStencilState& depthStencilState) {
    if (depthStencilState != m_depthStencilState) {
      m_depthStencilState = depthStencilState;
      m_dirty             = true;
    }
    return *this;
  }

  [[nodiscard]] vk::PipelineColorBlendStateCreateInfo GetColorBlendStateCI() const {
    auto createInfo = vk::PipelineColorBlendStateCreateInfo()
                          .setAttachments(m_colorBlendState.attachments)
                          .setBlendConstants({1.0f, 1.0f, 1.0f, 1.0f})
                          .setLogicOpEnable(m_colorBlendState.logicOpEnable)
                          .setLogicOp(m_colorBlendState.logicOp);

    return createInfo;
  }
  PipelineState& SetColorBlendState(const ColorBlendState& colorBlendState) {
    if (colorBlendState != m_colorBlendState) {
      m_colorBlendState = colorBlendState;
      m_dirty           = true;
    }
    return *this;
  }

  [[nodiscard]] vk::PipelineDynamicStateCreateInfo GetDynamicStateCI() const {
    auto createInfo = vk::PipelineDynamicStateCreateInfo().setDynamicStates(m_defaultDynamicStates);
    return createInfo;
  }

  [[nodiscard]] auto& GetSpecializationTable() const {
    return m_specializationState.GetConstantTable();
  }

  void SetRenderPass(RenderPass* renderPass) {
    if (renderPass != m_renderPass) {
      m_renderPass = renderPass;
      m_dirty      = true;
    }
  }

  auto GetRenderPass() const { return m_renderPass; }

  [[nodiscard]] auto GetSubpassIndex() const { return m_subpassIndex; }
  void SetSubpassIndex(uint32_t index) {
    if (m_subpassIndex != index) {
      m_subpassIndex = index;
      m_dirty        = true;
    }
  }

  [[nodiscard]] auto IsDirty() const { return m_dirty; }
  void ClearDirty() { m_dirty = false; }

  void Reset() {
    m_dirty = false;
    m_specializationState.Reset();
    m_vertexInputState   = {};
    m_inputAssemblyState = {};
    m_viewportState      = {};
    m_rasterizationState = {};
    m_multiSampleState   = {};
    m_depthStencilState  = {};
    m_colorBlendState    = {};
    m_subpassIndex       = 0;
    m_renderPass         = nullptr;
  }

private:
  bool m_dirty{false};
  SpecializationState m_specializationState{};
  VertexInputState m_vertexInputState{};
  InputAssemblyState m_inputAssemblyState{};
  ViewportState m_viewportState{};
  RasterizationState m_rasterizationState{};
  MultisampleState m_multiSampleState{};
  DepthStencilState m_depthStencilState{};
  ColorBlendState m_colorBlendState{};
  std::array<vk::DynamicState, 9> m_defaultDynamicStates{
      vk::DynamicState::eViewport,           vk::DynamicState::eScissor,
      vk::DynamicState::eLineWidth,          vk::DynamicState::eDepthBias,
      vk::DynamicState::eBlendConstants,     vk::DynamicState::eDepthBounds,
      vk::DynamicState::eStencilCompareMask, vk::DynamicState::eStencilWriteMask,
      vk::DynamicState::eStencilReference,
  };
  uint32_t m_subpassIndex{0};
  RenderPass* m_renderPass{nullptr};
};

}  // namespace zen::vulkan