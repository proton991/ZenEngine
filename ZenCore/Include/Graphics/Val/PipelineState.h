#pragma once
#include <vulkan/vulkan.h>
#include <vector>
#include <unordered_map>
#include "Common/Helpers.h"

namespace zen::val
{
class RenderPass;
struct VertexInputState
{
    std::vector<VkVertexInputBindingDescription>   bindings;
    std::vector<VkVertexInputAttributeDescription> attributes;
};

struct InputAssemblyState
{
    VkPrimitiveTopology topology{VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkBool32            primitiveRestartEnable{VK_FALSE};
};

struct RasterizationState
{
    VkBool32        depthClampEnable{VK_FALSE};
    VkBool32        rasterizerDiscardEnable{VK_FALSE};
    VkPolygonMode   polygonMode{VK_POLYGON_MODE_FILL};
    VkCullModeFlags cullMode{VK_CULL_MODE_BACK_BIT};
    VkFrontFace     frontFace{VK_FRONT_FACE_COUNTER_CLOCKWISE};
    VkBool32        depthBiasEnable{VK_FALSE};
    float           depthBiasConstantFactor{1.0f};
    float           depthBiasClamp{1.0f};
    float           depthBiasSlopeFactor{1.0f};
};

struct ViewportState
{
    uint32_t viewportCount{1};
    uint32_t scissorCount{1};
};

struct MultisampleState
{
    VkSampleCountFlagBits rasterizationSamples{VK_SAMPLE_COUNT_1_BIT};
    VkBool32              sampleShadingEnable{VK_FALSE};
    float                 minSampleShading{0.0f};
    VkSampleMask          sampleMask{0};
    VkBool32              alphaToCoverageEnable{VK_FALSE};
    VkBool32              alphaToOneEnable{VK_FALSE};
};

struct StencilOpState
{
    VkStencilOp failOp{VK_STENCIL_OP_REPLACE};
    VkStencilOp passOp{VK_STENCIL_OP_REPLACE};
    VkStencilOp depthFailOp{VK_STENCIL_OP_REPLACE};
    VkCompareOp compareOp{VK_COMPARE_OP_NEVER};
};

inline VkStencilOpState DefaultStencilOpState()
{
    static VkStencilOpState state{};
    state.failOp      = VK_STENCIL_OP_REPLACE;
    state.passOp      = VK_STENCIL_OP_REPLACE;
    state.depthFailOp = VK_STENCIL_OP_REPLACE;
    state.compareOp   = VK_COMPARE_OP_NEVER;
    state.compareMask = ~0u;
    state.writeMask   = ~0U;
    state.reference   = ~0U;
    return state;
}

struct DepthStencilState
{
    VkBool32 depthTestEnable{VK_TRUE};
    VkBool32 depthWriteEnable{VK_TRUE};
    // Note: Using reversed depth-buffer for increased precision, so Greater depth values are kept
    VkCompareOp      depthCompareOp{VK_COMPARE_OP_GREATER};
    VkBool32         depthBoundsTestEnable{VK_FALSE};
    VkBool32         stencilTestEnable{VK_FALSE};
    VkStencilOpState front{DefaultStencilOpState()};
    VkStencilOpState back{DefaultStencilOpState()};
};

struct ColorBlendAttachmentState
{
    VkBool32              blendEnable{VK_FALSE};
    VkBlendFactor         srcColorBlendFactor{VK_BLEND_FACTOR_ONE};
    VkBlendFactor         dstColorBlendFactor{VK_BLEND_FACTOR_ZERO};
    VkBlendOp             colorBlendOp{VK_BLEND_OP_ADD};
    VkBlendFactor         srcAlphaBlendFactor{VK_BLEND_FACTOR_ONE};
    VkBlendFactor         dstAlphaBlendFactor{VK_BLEND_FACTOR_ZERO};
    VkBlendOp             alphaBlendOp{VK_BLEND_OP_ADD};
    VkColorComponentFlags colorWriteMask{VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT};
};

struct ColorBlendState
{
    VkBool32  logicOpEnable{VK_FALSE};
    VkLogicOp logicOp{VK_LOGIC_OP_CLEAR};

    std::vector<VkPipelineColorBlendAttachmentState> attachments;
};

class SpecializationState
{
public:
    void Reset()
    {
        if (m_dirty)
        {
            m_constantTable.clear();
        }
        m_dirty = false;
    }

    bool IsDirty() const { return m_dirty; }

    template <class T>
    void SetConstant(uint32_t id, const T& value)
    {
        auto it    = m_constantTable.find(id);
        auto bytes = util::ToBytes(value);
        if (it != m_constantTable.end() && it->second == bytes)
        {
            return;
        }
        m_constantTable[id] = bytes;
        m_dirty             = true;
    }

    auto& GetConstantTable() const { return m_constantTable; }

private:
    bool m_dirty{false};

    std::unordered_map<uint32_t, std::vector<uint8_t>> m_constantTable;
};

class PipelineState
{
public:
    void SetVertexInputState(VertexInputState&& state);
    void SetInputAssemblyState(InputAssemblyState&& state);
    void SetViewportState(ViewportState&& state);
    void SetRasterizationState(RasterizationState&& state);
    void SetMultiSampleState(MultisampleState&& state);
    void SetDepthStencilState(DepthStencilState&& state);
    void SetColorBlendState(ColorBlendState&& state);
    void SetDynamicState(std::vector<VkDynamicState>&& states);
    void SetRenderPass(VkRenderPass state);
    void SetSubpassIndex(uint32_t index);

    auto& GetSpecializationState() const { return m_specializationState; }

    VkPipelineVertexInputStateCreateInfo   GetVIStateCI() const;
    VkPipelineInputAssemblyStateCreateInfo GetIAStateCI() const;
    VkPipelineViewportStateCreateInfo      GetVPStateCI() const;
    VkPipelineRasterizationStateCreateInfo GetRasterizationStateCI() const;
    VkPipelineMultisampleStateCreateInfo   GetMSStateCI() const;
    VkPipelineDepthStencilStateCreateInfo  GetDSStateCI() const;
    VkPipelineColorBlendStateCreateInfo    GetCBStateCI() const;
    VkPipelineDynamicStateCreateInfo       GetDynamicStateCI() const;
    VkRenderPass                           GetRPHandle() const;
    uint32_t                               GetSubpassIndex() const;

private:
    VertexInputState            m_vertexInputState{}; // TODO: Move this part to shader reflection
    InputAssemblyState          m_inputAssemblyState{};
    ViewportState               m_viewportState{};
    RasterizationState          m_rasterizationState{};
    MultisampleState            m_multiSampleState{};
    DepthStencilState           m_depthStencilState{};
    ColorBlendState             m_colorBlendState{};
    SpecializationState         m_specializationState{};
    std::vector<VkDynamicState> m_dynamicStates;

    uint32_t     m_subpassIndex{0};
    VkRenderPass m_renderPassHandle{VK_NULL_HANDLE};
};
} // namespace zen::val