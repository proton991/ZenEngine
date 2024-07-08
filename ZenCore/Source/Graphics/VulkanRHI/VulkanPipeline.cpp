#include "Graphics/VulkanRHI/VulkanRHI.h"
#include "Graphics/VulkanRHI/VulkanPipeline.h"
#include "Graphics/VulkanRHI/VulkanCommon.h"
#include "Graphics/VulkanRHI/VulkanDevice.h"
#include "Graphics/VulkanRHI/VulkanTypes.h"

namespace zen::rhi
{
static uint32_t DecideDescriptorCount(
    VkDescriptorType type,
    uint32_t count,
    const VkPhysicalDeviceDescriptorIndexingProperties& properties)
{
    switch (type)
    {
        case VK_DESCRIPTOR_TYPE_SAMPLER:
            return std::min(count, properties.maxPerStageDescriptorUpdateAfterBindSamplers);
        case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
        case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
        case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
            return std::min(count, properties.maxPerStageDescriptorUpdateAfterBindSampledImages);
        case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
        case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
            return std::min(count, properties.maxPerStageDescriptorUpdateAfterBindStorageImages);
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
            return std::min(count, properties.maxPerStageDescriptorUpdateAfterBindUniformBuffers);
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
            return std::min(count, properties.maxPerStageDescriptorUpdateAfterBindStorageBuffers);
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
            return std::min(count, properties.maxPerStageDescriptorUpdateAfterBindUniformBuffers);
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
            return std::min(count, properties.maxPerStageDescriptorUpdateAfterBindStorageBuffers);
        case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
            return std::min(count, properties.maxPerStageDescriptorUpdateAfterBindInputAttachments);
        default:
            // If the descriptor type is unknown, return the original count
            return count;
    }
}


ShaderHandle VulkanRHI::CreateShader(const ShaderGroupInfo& sgInfo)
{
    VulkanShader* shader = m_shaderAllocator.Alloc();

    // Create specialization info from tracked state. This is shared by all shaders.
    const auto& specConstants = sgInfo.specializationConstants;
    if (!specConstants.empty())
    {
        shader->entries.resize(specConstants.size());
        for (uint32_t i = 0; i < specConstants.size(); i++)
        {
            VkSpecializationMapEntry& entry = shader->entries[i];
            // fill in data
            entry.constantID = specConstants[i].constantId;
            entry.size       = sizeof(uint32_t);
            entry.offset     = reinterpret_cast<const char*>(&specConstants[i].intValue) -
                reinterpret_cast<const char*>(specConstants.data());
        }
    }
    shader->specializationInfo.mapEntryCount = static_cast<uint32_t>(shader->entries.size());
    shader->specializationInfo.pMapEntries =
        shader->entries.empty() ? nullptr : shader->entries.data();
    shader->specializationInfo.dataSize =
        specConstants.size() * sizeof(ShaderSpecializationConstant);
    shader->specializationInfo.pData = specConstants.empty() ? nullptr : specConstants.data();

    for (uint32_t i = 0; i < sgInfo.shaderStages.size(); i++)
    {
        VkShaderModuleCreateInfo shaderModuleCI;
        InitVkStruct(shaderModuleCI, VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
        shaderModuleCI.codeSize = sgInfo.sprivCode[i].size();
        shaderModuleCI.pCode    = reinterpret_cast<const uint32_t*>(sgInfo.sprivCode[i].data());
        VkShaderModule module{VK_NULL_HANDLE};
        VKCHECK(vkCreateShaderModule(GetVkDevice(), &shaderModuleCI, nullptr, &module));

        VkPipelineShaderStageCreateInfo pipelineShaderStageCI;
        InitVkStruct(pipelineShaderStageCI, VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO);
        pipelineShaderStageCI.stage  = ShaderStageToVkShaderStageFlagBits(sgInfo.shaderStages[i]);
        pipelineShaderStageCI.pName  = "main";
        pipelineShaderStageCI.module = module;
        pipelineShaderStageCI.pSpecializationInfo = &shader->specializationInfo;

        shader->stageCreateInfos.push_back(pipelineShaderStageCI);
    }
    // Create descriptorSetLayouts
    std::vector<std::vector<VkDescriptorSetLayoutBinding>> dsBindings;
    const auto setCount = sgInfo.SRDs.size();
    dsBindings.resize(setCount);
    for (uint32_t i = 0; i < setCount; i++)
    {
        std::vector<VkDescriptorBindingFlags> bindingFlags;
        // collect bindings for set i
        for (uint32_t j = 0; j < sgInfo.SRDs[i].size(); j++)
        {
            const ShaderResourceDescriptor& srd = sgInfo.SRDs[i][j];
            VkDescriptorSetLayoutBinding binding{};
            binding.binding         = srd.binding;
            binding.descriptorType  = ShaderResourceTypeToVkDescriptorType(srd.type);
            binding.descriptorCount = DecideDescriptorCount(
                binding.descriptorType, srd.arraySize, m_device->GetDescriptorIndexingProperties());
            binding.stageFlags = ShaderStageFlagsBitsToVkShaderStageFlags(srd.stageFlags);
            if (binding.descriptorCount > 1)
            {
                bindingFlags.push_back(VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT);
            }
            else
            {
                bindingFlags.push_back(0);
            }
            dsBindings[i].push_back(binding);
        }
        if (dsBindings[i].empty())
            continue;
        // create descriptor set layout for set i
        VkDescriptorSetLayoutCreateInfo layoutCI;
        InitVkStruct(layoutCI, VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);
        layoutCI.bindingCount = dsBindings[i].size();
        layoutCI.pBindings    = dsBindings[i].data();
        layoutCI.flags        = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
        // set binding flags
        VkDescriptorSetLayoutBindingFlagsCreateInfo bindingFlagsCreateInfo;
        InitVkStruct(bindingFlagsCreateInfo,
                     VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO);
        bindingFlagsCreateInfo.bindingCount  = bindingFlags.size();
        bindingFlagsCreateInfo.pBindingFlags = bindingFlags.data();
        layoutCI.pNext                       = &bindingFlagsCreateInfo;

        VkDescriptorSetLayout descriptorSetLayout{VK_NULL_HANDLE};
        VKCHECK(
            vkCreateDescriptorSetLayout(GetVkDevice(), &layoutCI, nullptr, &descriptorSetLayout));
        shader->descriptorSetLayouts.push_back(descriptorSetLayout);
    }
    // vertex input state
    const auto vertexInputCount = sgInfo.vertexInputAttributes.size();
    shader->vertexInputInfo.vkAttributes.resize(vertexInputCount);
    // packed data for vertex inputs, only 1 binding
    VkVertexInputBindingDescription vkBinding{};
    vkBinding.binding   = 0;
    vkBinding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    vkBinding.stride    = sgInfo.vertexBindingStride;
    shader->vertexInputInfo.vkBindings.emplace_back(vkBinding);
    // populate attributes
    for (uint32_t i = 0; i < vertexInputCount; i++)
    {
        shader->vertexInputInfo.vkAttributes[i].binding  = sgInfo.vertexInputAttributes[i].binding;
        shader->vertexInputInfo.vkAttributes[i].location = sgInfo.vertexInputAttributes[i].location;
        shader->vertexInputInfo.vkAttributes[i].offset   = sgInfo.vertexInputAttributes[i].offset;
        shader->vertexInputInfo.vkAttributes[i].format =
            static_cast<VkFormat>(sgInfo.vertexInputAttributes[i].format);
    }
    // Create pipeline layout
    VkPipelineLayoutCreateInfo pipelineLayoutCI;
    InitVkStruct(pipelineLayoutCI, VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
    pipelineLayoutCI.pSetLayouts    = shader->descriptorSetLayouts.data();
    pipelineLayoutCI.setLayoutCount = setCount;
    VkPushConstantRange prc{};
    const auto& pc = sgInfo.pushConstants;
    if (pc.size > 0)
    {
        prc.stageFlags = ShaderStageFlagsBitsToVkShaderStageFlags(pc.stageFlags);
        prc.size       = pc.size;
        pipelineLayoutCI.pushConstantRangeCount = 1;
        pipelineLayoutCI.pPushConstantRanges    = &prc;
    }
    vkCreatePipelineLayout(GetVkDevice(), &pipelineLayoutCI, nullptr, &shader->pipelineLayout);

    return ShaderHandle(shader);
}

void VulkanRHI::DestroyShader(ShaderHandle shaderHandle)
{
    VulkanShader* shader = reinterpret_cast<VulkanShader*>(shaderHandle.value);

    for (auto& dsLayout : shader->descriptorSetLayouts)
    {
        vkDestroyDescriptorSetLayout(GetVkDevice(), dsLayout, nullptr);
    }

    vkDestroyPipelineLayout(GetVkDevice(), shader->pipelineLayout, nullptr);

    for (auto& stageCreateInfo : shader->stageCreateInfos)
    {
        vkDestroyShaderModule(GetVkDevice(), stageCreateInfo.module, nullptr);
    }

    m_shaderAllocator.Free(shader);
}

PipelineHandle VulkanRHI::CreateGfxPipeline(ShaderHandle shaderHandle,
                                            const GfxPipelineStates& states,
                                            RenderPassHandle renderPassHandle,
                                            uint32_t subpass)
{
    // Input Assembly
    VkPipelineInputAssemblyStateCreateInfo IAStateCI;
    InitVkStruct(IAStateCI, VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO);
    IAStateCI.topology               = ToVkPrimitiveTopology(states.primitiveType);
    IAStateCI.primitiveRestartEnable = VK_FALSE;

    // Viewport State
    VkPipelineViewportStateCreateInfo VPStateCI;
    InitVkStruct(VPStateCI, VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO);
    VPStateCI.scissorCount  = 1;
    VPStateCI.viewportCount = 1;

    // Rasterization State
    const auto& rasterizationState = states.rasterizationState;
    VkPipelineRasterizationStateCreateInfo rasterizationCI;
    InitVkStruct(rasterizationCI, VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO);
    rasterizationCI.cullMode                = ToVkCullModeFlags(rasterizationState.cullMode);
    rasterizationCI.frontFace               = ToVkFrontFace(rasterizationState.frontFace);
    rasterizationCI.depthClampEnable        = rasterizationState.enableDepthClamp;
    rasterizationCI.rasterizerDiscardEnable = rasterizationState.discardPrimitives;
    rasterizationCI.polygonMode =
        rasterizationState.wireframe ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
    rasterizationCI.depthBiasEnable         = rasterizationState.enableDepthBias;
    rasterizationCI.depthBiasClamp          = rasterizationState.depthBiasClamp;
    rasterizationCI.depthBiasConstantFactor = rasterizationState.depthBiasConstantFactor;
    rasterizationCI.depthBiasSlopeFactor    = rasterizationState.depthBiasSlopeFactor;
    rasterizationCI.lineWidth               = rasterizationState.lineWidth;

    // Multisample state
    const auto& multisampleState = states.multiSampleState;
    VkPipelineMultisampleStateCreateInfo MSStateCI;
    InitVkStruct(MSStateCI, VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO);
    MSStateCI.rasterizationSamples  = ToVkSampleCountFlagBits(multisampleState.sampleCount);
    MSStateCI.sampleShadingEnable   = multisampleState.enableSampleShading;
    MSStateCI.minSampleShading      = multisampleState.minSampleShading;
    MSStateCI.alphaToCoverageEnable = multisampleState.enableAlphaToCoverage;
    MSStateCI.alphaToOneEnable      = multisampleState.enbaleAlphaToOne;
    MSStateCI.pSampleMask =
        multisampleState.sampleMasks.empty() ? nullptr : multisampleState.sampleMasks.data();

    // Depth Stencil
    const auto& depthStencilState = states.depthStencilState;
    VkPipelineDepthStencilStateCreateInfo DSStateCI;
    InitVkStruct(DSStateCI, VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO);
    DSStateCI.depthTestEnable       = depthStencilState.enableDepthTest;
    DSStateCI.depthWriteEnable      = depthStencilState.enableDepthWrite;
    DSStateCI.depthCompareOp        = ToVkCompareOp(depthStencilState.depthCompareOp);
    DSStateCI.depthBoundsTestEnable = depthStencilState.enableDepthBoundsTest;
    DSStateCI.stencilTestEnable     = depthStencilState.enableStencilTest;

    DSStateCI.front.failOp      = ToVkStencilOp(depthStencilState.frontOp.fail);
    DSStateCI.front.passOp      = ToVkStencilOp(depthStencilState.frontOp.pass);
    DSStateCI.front.depthFailOp = ToVkStencilOp(depthStencilState.frontOp.depthFail);
    DSStateCI.front.compareOp   = ToVkCompareOp(depthStencilState.frontOp.compare);
    DSStateCI.front.compareMask = depthStencilState.frontOp.compareMask;
    DSStateCI.front.writeMask   = depthStencilState.frontOp.writeMask;
    DSStateCI.front.reference   = depthStencilState.frontOp.reference;

    DSStateCI.back.failOp      = ToVkStencilOp(depthStencilState.backOp.fail);
    DSStateCI.back.passOp      = ToVkStencilOp(depthStencilState.backOp.pass);
    DSStateCI.back.depthFailOp = ToVkStencilOp(depthStencilState.backOp.depthFail);
    DSStateCI.back.compareOp   = ToVkCompareOp(depthStencilState.backOp.compare);
    DSStateCI.back.compareMask = depthStencilState.backOp.compareMask;
    DSStateCI.back.writeMask   = depthStencilState.backOp.writeMask;
    DSStateCI.back.reference   = depthStencilState.backOp.reference;

    DSStateCI.minDepthBounds = depthStencilState.minDepthBounds;
    DSStateCI.maxDepthBounds = depthStencilState.maxDepthBounds;

    // Color Blend State
    const auto& colorBlendState = states.colorBlendState;
    std::vector<VkPipelineColorBlendAttachmentState> vkCBAttStates;
    vkCBAttStates.resize(colorBlendState.attachments.size());
    for (uint32_t i = 0; i < colorBlendState.attachments.size(); i++)
    {
        vkCBAttStates[i].blendEnable = colorBlendState.attachments[i].enableBlend;

        vkCBAttStates[i].srcColorBlendFactor =
            ToVkBlendFactor(colorBlendState.attachments[i].srcAlphaBlendFactor);
        vkCBAttStates[i].dstColorBlendFactor =
            ToVkBlendFactor(colorBlendState.attachments[i].dstColorBlendFactor);
        vkCBAttStates[i].colorBlendOp = ToVkBlendOp(colorBlendState.attachments[i].colorBlendOp);

        vkCBAttStates[i].srcAlphaBlendFactor =
            ToVkBlendFactor(colorBlendState.attachments[i].srcAlphaBlendFactor);
        vkCBAttStates[i].dstAlphaBlendFactor =
            ToVkBlendFactor(colorBlendState.attachments[i].dstAlphaBlendFactor);
        vkCBAttStates[i].alphaBlendOp = ToVkBlendOp(colorBlendState.attachments[i].alphaBlendOp);

        if (colorBlendState.attachments[i].writeR)
        {
            vkCBAttStates[i].colorWriteMask |= VK_COLOR_COMPONENT_R_BIT;
        }
        if (colorBlendState.attachments[i].writeG)
        {
            vkCBAttStates[i].colorWriteMask |= VK_COLOR_COMPONENT_G_BIT;
        }
        if (colorBlendState.attachments[i].writeB)
        {
            vkCBAttStates[i].colorWriteMask |= VK_COLOR_COMPONENT_B_BIT;
        }
        if (colorBlendState.attachments[i].writeA)
        {
            vkCBAttStates[i].colorWriteMask |= VK_COLOR_COMPONENT_A_BIT;
        }
    }
    VkPipelineColorBlendStateCreateInfo CBStateCI;
    InitVkStruct(CBStateCI, VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO);
    CBStateCI.logicOpEnable     = colorBlendState.enableLogicOp;
    CBStateCI.logicOp           = ToVkLogicOp(colorBlendState.logicOp);
    CBStateCI.attachmentCount   = static_cast<uint32_t>(vkCBAttStates.size());
    CBStateCI.pAttachments      = vkCBAttStates.empty() ? nullptr : vkCBAttStates.data();
    CBStateCI.blendConstants[0] = colorBlendState.blendConstants[0];
    CBStateCI.blendConstants[1] = colorBlendState.blendConstants[1];
    CBStateCI.blendConstants[2] = colorBlendState.blendConstants[2];
    CBStateCI.blendConstants[3] = colorBlendState.blendConstants[3];

    // Dynamic States
    VkPipelineDynamicStateCreateInfo dynamicStateCI;
    InitVkStruct(dynamicStateCI, VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO);
    std::vector<VkDynamicState> vkDynamicStates(states.dynamicStates.size());
    for (uint32_t i = 0; i < states.dynamicStates.size(); i++)
    {
        vkDynamicStates[i] = ToVkDynamicState(states.dynamicStates[i]);
    }
    dynamicStateCI.dynamicStateCount = static_cast<uint32_t>(states.dynamicStates.size());
    dynamicStateCI.pDynamicStates    = vkDynamicStates.empty() ? nullptr : vkDynamicStates.data();

    VulkanShader* shader = reinterpret_cast<VulkanShader*>(shaderHandle.value);

    VkGraphicsPipelineCreateInfo pipelineCI;
    InitVkStruct(pipelineCI, VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO);
    pipelineCI.stageCount          = shader->stageCreateInfos.size();
    pipelineCI.pStages             = shader->stageCreateInfos.data();
    pipelineCI.pVertexInputState   = &shader->vertexInputInfo.stateCI;
    pipelineCI.pInputAssemblyState = &IAStateCI;
    pipelineCI.pViewportState      = &VPStateCI;
    pipelineCI.pRasterizationState = &rasterizationCI;
    pipelineCI.pMultisampleState   = &MSStateCI;
    pipelineCI.pDepthStencilState  = &DSStateCI;
    pipelineCI.pColorBlendState    = &CBStateCI;
    pipelineCI.pDynamicState       = &dynamicStateCI;
    pipelineCI.layout              = shader->pipelineLayout;
    pipelineCI.renderPass          = reinterpret_cast<VkRenderPass>(renderPassHandle.value);
    pipelineCI.subpass             = subpass;

    VkPipeline gfxPipeline{VK_NULL_HANDLE};
    VKCHECK(
        vkCreateGraphicsPipelines(GetVkDevice(), nullptr, 1, &pipelineCI, nullptr, &gfxPipeline));

    return PipelineHandle(gfxPipeline);
}

void VulkanRHI::DestroyPipeline(PipelineHandle pipelineHandle)
{
    VkPipeline pipeline = reinterpret_cast<VkPipeline>(pipelineHandle.value);
    vkDestroyPipeline(GetVkDevice(), pipeline, nullptr);
}

} // namespace zen::rhi