#pragma once
#include "Common/Helpers.h"
#include "Graphics/Val/Shader.h"
#include "Graphics/Val/PipelineLayout.h"

namespace std
{
template <>
struct hash<VkAttachmentDescription>
{
    std::size_t operator()(const VkAttachmentDescription& attachment) const
    {
        std::size_t result = 0;
        zen::util::HashCombine(result, attachment.format);
        zen::util::HashCombine(result, attachment.samples);
        zen::util::HashCombine(result, attachment.initialLayout);
        zen::util::HashCombine(result, attachment.finalLayout);
        zen::util::HashCombine(result, attachment.storeOp);
        zen::util::HashCombine(result, attachment.loadOp);
        zen::util::HashCombine(result, attachment.stencilStoreOp);
        zen::util::HashCombine(result, attachment.stencilLoadOp);
        return result;
    }
};

template <>
struct hash<zen::val::SubpassInfo>
{
    std::size_t operator()(const zen::val::SubpassInfo& subpassInfo) const
    {
        std::size_t result = 0;

        for (auto& colorRef : subpassInfo.colorRefs)
        {
            zen::util::HashCombine(result, colorRef.attachment);
        }
        for (auto& inputRef : subpassInfo.inputRefs)
        {
            zen::util::HashCombine(result, inputRef.attachment);
        }
        zen::util::HashCombine(result, subpassInfo.hasDepthStencilRef);
        zen::util::HashCombine(result, subpassInfo.depthStencilRef.attachment);
        return result;
    }
};

template <>
struct hash<zen::val::ShaderModule>
{
    std::size_t operator()(const zen::val::ShaderModule& shaderModule) const
    {
        std::size_t result = 0;

        zen::util::HashCombine(result, shaderModule.GetId());

        return result;
    }
};

template <>
struct hash<zen::val::PipelineLayout>
{
    std::size_t operator()(const zen::val::PipelineLayout& pipelineLayout) const
    {
        std::size_t result = 0;

        zen::util::HashCombine(result, pipelineLayout.GetHandle());

        return result;
    }
};

template <>
struct hash<zen::val::SpecializationState>
{
    std::size_t operator()(const zen::val::SpecializationState& state) const
    {
        std::size_t result = 0;

        for (auto& constants : state.GetConstantTable())
        {
            zen::util::HashCombine(result, constants.first);
            for (const auto data : constants.second)
            {
                zen::util::HashCombine(result, data);
            }
        }

        return result;
    }
};

template <>
struct hash<VkVertexInputAttributeDescription>
{
    std::size_t operator()(const VkVertexInputAttributeDescription& attribute) const
    {
        std::size_t result = 0;

        zen::util::HashCombine(result, attribute.binding);
        zen::util::HashCombine(result, static_cast<std::underlying_type<VkFormat>::type>(attribute.format));
        zen::util::HashCombine(result, attribute.location);
        zen::util::HashCombine(result, attribute.offset);

        return result;
    }
};

template <>
struct hash<VkVertexInputBindingDescription>
{
    std::size_t operator()(const VkVertexInputBindingDescription& vertexBinding) const
    {
        std::size_t result = 0;

        zen::util::HashCombine(result, vertexBinding.binding);
        zen::util::HashCombine(result, static_cast<std::underlying_type<VkVertexInputRate>::type>(vertexBinding.inputRate));
        zen::util::HashCombine(result, vertexBinding.stride);

        return result;
    }
};

template <>
struct hash<VkStencilOpState>
{
    std::size_t operator()(const VkStencilOpState& stencil) const
    {
        std::size_t result = 0;

        zen::util::HashCombine(result, static_cast<std::underlying_type<VkCompareOp>::type>(stencil.compareOp));
        zen::util::HashCombine(result, static_cast<std::underlying_type<VkStencilOp>::type>(stencil.depthFailOp));
        zen::util::HashCombine(result, static_cast<std::underlying_type<VkStencilOp>::type>(stencil.failOp));
        zen::util::HashCombine(result, static_cast<std::underlying_type<VkStencilOp>::type>(stencil.passOp));

        return result;
    }
};

template <>
struct hash<VkPipelineColorBlendAttachmentState>
{
    std::size_t operator()(const VkPipelineColorBlendAttachmentState& color_blend_attachment) const
    {
        std::size_t result = 0;

        zen::util::HashCombine(result, static_cast<std::underlying_type<VkBlendOp>::type>(color_blend_attachment.alphaBlendOp));
        zen::util::HashCombine(result, color_blend_attachment.blendEnable);
        zen::util::HashCombine(result, static_cast<std::underlying_type<VkBlendOp>::type>(color_blend_attachment.colorBlendOp));
        zen::util::HashCombine(result, color_blend_attachment.colorWriteMask);
        zen::util::HashCombine(result, static_cast<std::underlying_type<VkBlendFactor>::type>(color_blend_attachment.dstAlphaBlendFactor));
        zen::util::HashCombine(result, static_cast<std::underlying_type<VkBlendFactor>::type>(color_blend_attachment.dstColorBlendFactor));
        zen::util::HashCombine(result, static_cast<std::underlying_type<VkBlendFactor>::type>(color_blend_attachment.srcAlphaBlendFactor));
        zen::util::HashCombine(result, static_cast<std::underlying_type<VkBlendFactor>::type>(color_blend_attachment.srcColorBlendFactor));

        return result;
    }
};

template <>
struct hash<zen::val::PipelineState>
{
    std::size_t operator()(const zen::val::PipelineState& pipelineState) const
    {
        std::size_t result = 0;

        // For graphics only
        if (auto rpHandle = pipelineState.GetRPHandle())
        {
            zen::util::HashCombine(result, rpHandle);
        }

        zen::util::HashCombine(result, pipelineState.GetSpecializationState());

        zen::util::HashCombine(result, pipelineState.GetSubpassIndex());

        // VkPipelineVertexInputStateCreateInfo
        for (auto& attribute : pipelineState.GetVertexInputState().attributes)
        {
            zen::util::HashCombine(result, attribute);
        }

        for (auto& binding : pipelineState.GetVertexInputState().bindings)
        {
            zen::util::HashCombine(result, binding);
        }

        // VkPipelineInputAssemblyStateCreateInfo
        zen::util::HashCombine(result, pipelineState.GetInputAssemblyState().primitiveRestartEnable);
        zen::util::HashCombine(result, static_cast<std::underlying_type<VkPrimitiveTopology>::type>(pipelineState.GetInputAssemblyState().topology));

        //VkPipelineViewportStateCreateInfo
        zen::util::HashCombine(result, pipelineState.GetViewportState().viewportCount);
        zen::util::HashCombine(result, pipelineState.GetViewportState().scissorCount);

        // VkPipelineRasterizationStateCreateInfo
        zen::util::HashCombine(result, pipelineState.GetRasterizationState().cullMode);
        zen::util::HashCombine(result, pipelineState.GetRasterizationState().depthBiasEnable);
        zen::util::HashCombine(result, pipelineState.GetRasterizationState().depthClampEnable);
        zen::util::HashCombine(result, static_cast<std::underlying_type<VkFrontFace>::type>(pipelineState.GetRasterizationState().frontFace));
        zen::util::HashCombine(result, static_cast<std::underlying_type<VkPolygonMode>::type>(pipelineState.GetRasterizationState().polygonMode));
        zen::util::HashCombine(result, pipelineState.GetRasterizationState().rasterizerDiscardEnable);

        // VkPipelineMultisampleStateCreateInfo
        zen::util::HashCombine(result, pipelineState.GetMultiSampleState().alphaToCoverageEnable);
        zen::util::HashCombine(result, pipelineState.GetMultiSampleState().alphaToOneEnable);
        zen::util::HashCombine(result, pipelineState.GetMultiSampleState().minSampleShading);
        zen::util::HashCombine(result, static_cast<std::underlying_type<VkSampleCountFlagBits>::type>(pipelineState.GetMultiSampleState().rasterizationSamples));
        zen::util::HashCombine(result, pipelineState.GetMultiSampleState().sampleShadingEnable);

        // VkPipelineDepthStencilStateCreateInfo
        zen::util::HashCombine(result, pipelineState.GetDepthStencilState().back);
        zen::util::HashCombine(result, pipelineState.GetDepthStencilState().depthBoundsTestEnable);
        zen::util::HashCombine(result, static_cast<std::underlying_type<VkCompareOp>::type>(pipelineState.GetDepthStencilState().depthCompareOp));
        zen::util::HashCombine(result, pipelineState.GetDepthStencilState().depthTestEnable);
        zen::util::HashCombine(result, pipelineState.GetDepthStencilState().depthWriteEnable);
        zen::util::HashCombine(result, pipelineState.GetDepthStencilState().front);
        zen::util::HashCombine(result, pipelineState.GetDepthStencilState().stencilTestEnable);

        // VkPipelineColorBlendStateCreateInfo
        zen::util::HashCombine(result, static_cast<std::underlying_type<VkLogicOp>::type>(pipelineState.GetColorBlendState().logicOp));
        zen::util::HashCombine(result, pipelineState.GetColorBlendState().logicOpEnable);

        for (auto& attachment : pipelineState.GetColorBlendState().attachments)
        {
            zen::util::HashCombine(result, attachment);
        }

        return result;
    }
};
} // namespace std

namespace zen
{
template <typename T>
inline void HashParam(size_t& seed, const T& value)
{
    util::HashCombine(seed, value);
}

template <typename T, typename... Args>
inline void HashParam(size_t& seed, const T& firstArg, const Args&... args)
{
    HashParam(seed, firstArg);
    HashParam(seed, args...);
}

template <>
inline void HashParam<std::vector<val::ShaderModule*>>(
    size_t&                                seed,
    const std::vector<val::ShaderModule*>& value)
{
    for (auto& shaderModule : value)
    {
        util::HashCombine(seed, shaderModule->GetId());
    }
}

template <>
inline void HashParam<std::vector<VkAttachmentDescription>>(
    size_t&                                     seed,
    const std::vector<VkAttachmentDescription>& value)
{
    for (auto& attachment : value)
    {
        util::HashCombine(seed, attachment);
    }
}

template <>
inline void HashParam<std::vector<val::SubpassInfo>>(
    size_t&                              seed,
    const std::vector<val::SubpassInfo>& value)
{
    for (auto& attachment : value)
    {
        util::HashCombine(seed, attachment);
    }
}


template <class T, class... A>
T& RequestResourceNoLock(const val::Device& device, std::unordered_map<std::size_t, T>& resources, A&... args)
{
    std::size_t hash{0U};
    HashParam(hash, args...);

    auto resIt = resources.find(hash);

    if (resIt != resources.end())
    {
        return resIt->second;
    }

    // If we do not have it already, create and cache it
    const char* resType = typeid(T).name();
    size_t      resId   = resources.size();

    LOGD("Building #{} cache object ({})", resId, resType);

// Only error handle in release
#ifndef ZEN_DEBUG
    try
    {
#endif
        T resource(device, args...);
        auto [insertIt, inserted] = resources.emplace(hash, std::move(resource));

        if (!inserted)
        {
            throw std::runtime_error{std::string{"Insertion error for #"} + std::to_string(resId) +
                                     "cache object (" + resType + ")"};
        }

#ifndef ZEN_DEBUG
    }
    catch (const std::exception& e)
    {
        LOGE("Creation error for #{} cache object ({})", res_id, res_type);
        throw e;
    }
#endif

    return insertIt->second;
}

template <class T, class... A>
T& RequestResource(const val::Device& device, std::mutex& resourceMutex, std::unordered_map<std::size_t, T>& resources, A&... args)
{
    std::lock_guard<std::mutex> guard(resourceMutex);

    auto& res = RequestResourceNoLock(device, resources, args...);

    return res;
}

class ResourceCache
{
public:
    explicit ResourceCache(const val::Device& device) :
        m_valDevice(device) {}

    val::RenderPass* RequestRenderPass(const std::vector<VkAttachmentDescription>& attachments, const val::SubpassInfo& subpassInfo)
    {
        auto& res = RequestResource<val::RenderPass>(m_valDevice, m_mutexTable.renderPass, m_renderPasses, attachments, subpassInfo);

        return &res;
    }

    val::PipelineLayout* RequestPipelineLayout(const std::vector<val::ShaderModule*>& shaderModules)
    {
        auto& res = RequestResource<val::PipelineLayout>(m_valDevice, m_mutexTable.pipelineLayout, m_pipelineLayouts, shaderModules);
        return &res;
    }

    val::GraphicsPipeline* RequestGraphicsPipeline(const val::PipelineLayout& pipelineLayout, val::PipelineState& pipelineState)
    {
        auto& res = RequestResource<val::GraphicsPipeline>(m_valDevice, m_mutexTable.graphicsPipeline, m_graphicPipelines, pipelineLayout, pipelineState);
        return &res;
    }


private:
    const val::Device& m_valDevice;
    struct MutexTable
    {
        std::mutex descriptorSetLayout;
        std::mutex renderPass;
        std::mutex pipelineLayout;
        std::mutex graphicsPipeline;
    } m_mutexTable;

    std::unordered_map<size_t, val::RenderPass>       m_renderPasses;
    std::unordered_map<size_t, val::PipelineLayout>   m_pipelineLayouts;
    std::unordered_map<size_t, val::GraphicsPipeline> m_graphicPipelines;
};
} // namespace zen