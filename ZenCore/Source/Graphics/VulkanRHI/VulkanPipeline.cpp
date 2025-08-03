#include "Graphics/VulkanRHI/VulkanRHI.h"
#include "Graphics/VulkanRHI/VulkanPipeline.h"

#include "Graphics/RHI/RHIOptions.h"
#include "Graphics/VulkanRHI/VulkanCommon.h"
#include "Graphics/VulkanRHI/VulkanDevice.h"
#include "Graphics/VulkanRHI/VulkanResourceAllocator.h"
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
    VulkanShader* shader = VersatileResource::Alloc<VulkanShader>(m_resourceAllocator);

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
            switch (specConstants[i].type)
            {

                case ShaderSpecializationConstantType::eBool:
                {
                    entry.size   = sizeof(bool);
                    entry.offset = reinterpret_cast<const char*>(&specConstants[i].boolValue) -
                        reinterpret_cast<const char*>(specConstants.data());
                }
                break;
                case ShaderSpecializationConstantType::eInt:
                {
                    entry.size   = sizeof(int);
                    entry.offset = reinterpret_cast<const char*>(&specConstants[i].intValue) -
                        reinterpret_cast<const char*>(specConstants.data());
                }
                break;
                case ShaderSpecializationConstantType::eFloat:
                {
                    entry.size   = sizeof(float);
                    entry.offset = reinterpret_cast<const char*>(&specConstants[i].floatValue) -
                        reinterpret_cast<const char*>(specConstants.data());
                }
                break;
                default: break;
            }
        }
    }
    shader->specializationInfo.mapEntryCount = static_cast<uint32_t>(shader->entries.size());
    shader->specializationInfo.pMapEntries =
        shader->entries.empty() ? nullptr : shader->entries.data();
    shader->specializationInfo.dataSize =
        specConstants.size() * sizeof(ShaderSpecializationConstant);
    shader->specializationInfo.pData = specConstants.empty() ? nullptr : specConstants.data();

    for (auto& kv : sgInfo.sprivCode)
    {
        VkShaderModuleCreateInfo shaderModuleCI;
        InitVkStruct(shaderModuleCI, VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
        shaderModuleCI.codeSize = kv.second.size();
        shaderModuleCI.pCode    = reinterpret_cast<const uint32_t*>(kv.second.data());
        VkShaderModule module{VK_NULL_HANDLE};
        VKCHECK(vkCreateShaderModule(GetVkDevice(), &shaderModuleCI, nullptr, &module));

        VkPipelineShaderStageCreateInfo pipelineShaderStageCI;
        InitVkStruct(pipelineShaderStageCI, VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO);
        pipelineShaderStageCI.stage               = ShaderStageToVkShaderStageFlagBits(kv.first);
        pipelineShaderStageCI.pName               = "main";
        pipelineShaderStageCI.module              = module;
        pipelineShaderStageCI.pSpecializationInfo = &shader->specializationInfo;

        shader->stageCreateInfos.push_back(pipelineShaderStageCI);
    }
    // Create descriptor pool key while createing descriptor set layouts
    VulkanDescriptorPoolKey descriptorPoolKey{};
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
            descriptorPoolKey.descriptorCount[ToUnderlying(srd.type)] += binding.descriptorCount;
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
    if (vertexInputCount > 0)
    {
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
            shader->vertexInputInfo.vkAttributes[i].binding =
                sgInfo.vertexInputAttributes[i].binding;
            shader->vertexInputInfo.vkAttributes[i].location =
                sgInfo.vertexInputAttributes[i].location;
            shader->vertexInputInfo.vkAttributes[i].offset = sgInfo.vertexInputAttributes[i].offset;
            shader->vertexInputInfo.vkAttributes[i].format =
                static_cast<VkFormat>(sgInfo.vertexInputAttributes[i].format);
        }
        InitVkStruct(shader->vertexInputInfo.stateCI,
                     VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO);
        shader->vertexInputInfo.stateCI.vertexAttributeDescriptionCount =
            shader->vertexInputInfo.vkAttributes.size();
        shader->vertexInputInfo.stateCI.pVertexAttributeDescriptions =
            shader->vertexInputInfo.vkAttributes.data();
        shader->vertexInputInfo.stateCI.vertexBindingDescriptionCount =
            shader->vertexInputInfo.vkBindings.size();
        shader->vertexInputInfo.stateCI.pVertexBindingDescriptions =
            shader->vertexInputInfo.vkBindings.data();
    }
    else
    {
        InitVkStruct(shader->vertexInputInfo.stateCI,
                     VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO);
        shader->vertexInputInfo.stateCI.vertexAttributeDescriptionCount = 0;
        shader->vertexInputInfo.stateCI.pVertexAttributeDescriptions    = nullptr;
        shader->vertexInputInfo.stateCI.vertexBindingDescriptionCount   = 0;
        shader->vertexInputInfo.stateCI.pVertexBindingDescriptions      = nullptr;
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

    shader->descriptorPoolKey = descriptorPoolKey;
    shader->pushConstantsStageFlags =
        ShaderStageFlagsBitsToVkShaderStageFlags(sgInfo.pushConstants.stageFlags);

    auto debugName = sgInfo.name + "_PipelineLayout";
    m_device->SetObjectName(VK_OBJECT_TYPE_PIPELINE_LAYOUT,
                            reinterpret_cast<uint64_t>(shader->pipelineLayout), debugName.c_str());

    return ShaderHandle(shader);
}

void VulkanRHI::DestroyShader(ShaderHandle shaderHandle)
{
    VulkanShader* shader = TO_VK_SHADER(shaderHandle);

    for (auto& dsLayout : shader->descriptorSetLayouts)
    {
        vkDestroyDescriptorSetLayout(GetVkDevice(), dsLayout, nullptr);
    }

    for (auto& stageCreateInfo : shader->stageCreateInfos)
    {
        vkDestroyShaderModule(GetVkDevice(), stageCreateInfo.module, nullptr);
    }
    if (shader->pipelineLayout != VK_NULL_HANDLE)
    {
        vkDestroyPipelineLayout(GetVkDevice(), shader->pipelineLayout, nullptr);
    }
    VersatileResource::Free(m_resourceAllocator, shader);
}

PipelineHandle VulkanRHI::CreateGfxPipeline(ShaderHandle shaderHandle,
                                            const GfxPipelineStates& states,
                                            RenderPassHandle renderPassHandle,
                                            uint32_t subpass)
{
    // Input Assembly
    VkPipelineInputAssemblyStateCreateInfo IAStateCI;
    InitVkStruct(IAStateCI, VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO);
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
    CBStateCI.blendConstants[0] = colorBlendState.blendConstants.r;
    CBStateCI.blendConstants[1] = colorBlendState.blendConstants.g;
    CBStateCI.blendConstants[2] = colorBlendState.blendConstants.b;
    CBStateCI.blendConstants[3] = colorBlendState.blendConstants.a;

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

    VulkanShader* shader = TO_VK_SHADER(shaderHandle);

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
    pipelineCI.renderPass          = TO_VK_RENDER_PASS(renderPassHandle);
    pipelineCI.subpass             = subpass;

    VkPipeline gfxPipeline{VK_NULL_HANDLE};
    VKCHECK(
        vkCreateGraphicsPipelines(GetVkDevice(), nullptr, 1, &pipelineCI, nullptr, &gfxPipeline));

    VulkanPipeline* pipeline     = VersatileResource::Alloc<VulkanPipeline>(m_resourceAllocator);
    pipeline->pipeline           = gfxPipeline;
    pipeline->pipelineLayout     = shader->pipelineLayout;
    pipeline->descriptorSetCount = static_cast<uint32_t>(shader->descriptorSetLayouts.size());
    pipeline->pushConstantsStageFlags = shader->pushConstantsStageFlags;
    //    pipeline->descriptorSets.resize(pipeline->descriptorSetCount);

    m_shaderPipelines[shaderHandle] = pipeline;

    return PipelineHandle(pipeline);
}

PipelineHandle VulkanRHI::CreateGfxPipeline(ShaderHandle shaderHandle,
                                            const GfxPipelineStates& states,
                                            const RenderPassLayout& renderPassLayout,
                                            uint32_t subpass)

{
    // Input Assembly
    VkPipelineInputAssemblyStateCreateInfo IAStateCI;
    InitVkStruct(IAStateCI, VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO);
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
    CBStateCI.blendConstants[0] = colorBlendState.blendConstants.r;
    CBStateCI.blendConstants[1] = colorBlendState.blendConstants.g;
    CBStateCI.blendConstants[2] = colorBlendState.blendConstants.b;
    CBStateCI.blendConstants[3] = colorBlendState.blendConstants.a;

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

    VulkanShader* shader = TO_VK_SHADER(shaderHandle);

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
    pipelineCI.renderPass          = VK_NULL_HANDLE;
    pipelineCI.subpass             = subpass;

    std::vector<VkFormat> colorAttachmentFormats;
    colorAttachmentFormats.reserve(renderPassLayout.GetNumColorRenderTargets());
    for (uint32_t i = 0; i < renderPassLayout.GetNumColorRenderTargets(); i++)
    {
        VkFormat colorFormat = ToVkFormat(renderPassLayout.GetColorRenderTargets()[i].format);
        colorAttachmentFormats.emplace_back(colorFormat);
    }
    VkPipelineRenderingCreateInfoKHR renderingCI;
    InitVkStruct(renderingCI, VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR);
    renderingCI.colorAttachmentCount    = static_cast<uint32_t>(colorAttachmentFormats.size());
    renderingCI.pColorAttachmentFormats = colorAttachmentFormats.data();

    if (renderPassLayout.HasDepthStencilRenderTarget())
    {
        VkFormat depthFormat = ToVkFormat(renderPassLayout.GetDepthStencilRenderTarget().format);
        renderingCI.depthAttachmentFormat   = depthFormat;
        renderingCI.stencilAttachmentFormat = depthFormat;
    }
    pipelineCI.pNext = &renderingCI;

    VkPipeline gfxPipeline{VK_NULL_HANDLE};
    VKCHECK(
        vkCreateGraphicsPipelines(GetVkDevice(), nullptr, 1, &pipelineCI, nullptr, &gfxPipeline));

    VulkanPipeline* pipeline     = VersatileResource::Alloc<VulkanPipeline>(m_resourceAllocator);
    pipeline->pipeline           = gfxPipeline;
    pipeline->pipelineLayout     = shader->pipelineLayout;
    pipeline->descriptorSetCount = static_cast<uint32_t>(shader->descriptorSetLayouts.size());
    pipeline->pushConstantsStageFlags = shader->pushConstantsStageFlags;
    //    pipeline->descriptorSets.resize(pipeline->descriptorSetCount);

    m_shaderPipelines[shaderHandle] = pipeline;

    return PipelineHandle(pipeline);
}

PipelineHandle VulkanRHI::CreateComputePipeline(ShaderHandle shaderHandle)
{
    VulkanShader* shader = TO_VK_SHADER(shaderHandle);

    VkComputePipelineCreateInfo pipelineCI{};
    pipelineCI.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineCI.stage  = shader->stageCreateInfos[0];
    pipelineCI.layout = shader->pipelineLayout;

    VkPipeline computePipeline{VK_NULL_HANDLE};
    VKCHECK(vkCreateComputePipelines(GetVkDevice(), nullptr, 1, &pipelineCI, nullptr,
                                     &computePipeline));

    VulkanPipeline* pipeline     = VersatileResource::Alloc<VulkanPipeline>(m_resourceAllocator);
    pipeline->pipeline           = computePipeline;
    pipeline->pipelineLayout     = shader->pipelineLayout;
    pipeline->descriptorSetCount = static_cast<uint32_t>(shader->descriptorSetLayouts.size());
    pipeline->pushConstantsStageFlags = shader->pushConstantsStageFlags;

    m_shaderPipelines[shaderHandle] = pipeline;

    return PipelineHandle(pipeline);
}

void VulkanRHI::DestroyPipeline(PipelineHandle pipelineHandle)
{
    VulkanPipeline* pipeline = TO_VK_PIPELINE(pipelineHandle);
    vkDestroyPipeline(GetVkDevice(), pipeline->pipeline, nullptr);
}

VkDescriptorPool VulkanDescriptorPoolManager::GetOrCreateDescriptorPool(
    const VulkanDescriptorPoolKey& poolKey,
    VulkanDescriptorPoolsIt* iter)
{
    auto existed = m_pools.find(poolKey);
    if (existed != m_pools.end())
    {
        for (auto& kv : existed->second)
        {
            uint32_t descriptorCount = kv.second;
            if (descriptorCount < RHIOptions::GetInstance().MaxDescriptorSetPerPool())
            {
                *iter = existed;
                (*iter)->second[kv.first]++;
                return kv.first;
            }
        }
    }
    // create a new descriptor pool
    VkDescriptorPoolSize poolSizes[ToUnderlying(ShaderResourceType::eMax)];
    uint32_t poolSizeCount = 0;
    {
        const auto MaxDescriptorPerPool = RHIOptions::GetInstance().MaxDescriptorSetPerPool();
        auto* currPoolSize              = poolSizes;
        if (poolKey.descriptorCount[ToUnderlying(ShaderResourceType::eSampler)])
        {
            *currPoolSize      = {};
            currPoolSize->type = VK_DESCRIPTOR_TYPE_SAMPLER;
            currPoolSize->descriptorCount =
                poolKey.descriptorCount[ToUnderlying(ShaderResourceType::eSampler)] *
                MaxDescriptorPerPool;
            currPoolSize++;
            poolSizeCount++;
        }
        if (poolKey.descriptorCount[ToUnderlying(ShaderResourceType::eTexture)])
        {
            *currPoolSize      = {};
            currPoolSize->type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            currPoolSize->descriptorCount =
                poolKey.descriptorCount[ToUnderlying(ShaderResourceType::eTexture)] *
                MaxDescriptorPerPool;
            currPoolSize++;
            poolSizeCount++;
        }
        if (poolKey.descriptorCount[ToUnderlying(ShaderResourceType::eSamplerWithTexture)])
        {
            *currPoolSize      = {};
            currPoolSize->type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            currPoolSize->descriptorCount =
                poolKey.descriptorCount[ToUnderlying(ShaderResourceType::eSamplerWithTexture)] *
                MaxDescriptorPerPool;
            currPoolSize++;
            poolSizeCount++;
        }
        if (poolKey.descriptorCount[ToUnderlying(ShaderResourceType::eImage)])
        {
            *currPoolSize      = {};
            currPoolSize->type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            currPoolSize->descriptorCount =
                poolKey.descriptorCount[ToUnderlying(ShaderResourceType::eImage)] *
                MaxDescriptorPerPool;
            currPoolSize++;
            poolSizeCount++;
        }
        if (poolKey.descriptorCount[ToUnderlying(ShaderResourceType::eTextureBuffer)] ||
            poolKey.descriptorCount[ToUnderlying(ShaderResourceType::eSamplerWithTextureBuffer)])
        {
            *currPoolSize      = {};
            currPoolSize->type = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
            currPoolSize->descriptorCount =
                (poolKey.descriptorCount[ToUnderlying(ShaderResourceType::eTextureBuffer)] +
                 poolKey.descriptorCount[ToUnderlying(
                     ShaderResourceType::eSamplerWithTextureBuffer)]) *
                MaxDescriptorPerPool;
            currPoolSize++;
            poolSizeCount++;
        }
        if (poolKey.descriptorCount[ToUnderlying(ShaderResourceType::eImageBuffer)])
        {
            *currPoolSize      = {};
            currPoolSize->type = VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
            currPoolSize->descriptorCount =
                poolKey.descriptorCount[ToUnderlying(ShaderResourceType::eImageBuffer)] *
                MaxDescriptorPerPool;
            currPoolSize++;
            poolSizeCount++;
        }
        if (poolKey.descriptorCount[ToUnderlying(ShaderResourceType::eUniformBuffer)])
        {
            *currPoolSize      = {};
            currPoolSize->type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            currPoolSize->descriptorCount =
                poolKey.descriptorCount[ToUnderlying(ShaderResourceType::eUniformBuffer)] *
                MaxDescriptorPerPool;
            currPoolSize++;
            poolSizeCount++;
        }
        if (poolKey.descriptorCount[ToUnderlying(ShaderResourceType::eStorageBuffer)])
        {
            *currPoolSize      = {};
            currPoolSize->type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            currPoolSize->descriptorCount =
                poolKey.descriptorCount[ToUnderlying(ShaderResourceType::eStorageBuffer)] *
                MaxDescriptorPerPool;
            currPoolSize++;
            poolSizeCount++;
        }
        if (poolKey.descriptorCount[ToUnderlying(ShaderResourceType::eInputAttachment)])
        {
            *currPoolSize      = {};
            currPoolSize->type = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
            currPoolSize->descriptorCount =
                poolKey.descriptorCount[ToUnderlying(ShaderResourceType::eInputAttachment)] *
                MaxDescriptorPerPool;
            currPoolSize++;
            poolSizeCount++;
        }
        VERIFY_EXPR(poolSizeCount <= ToUnderlying(ShaderResourceType::eMax));
    }

    VkDescriptorPoolCreateInfo poolCI{};
    InitVkStruct(poolCI, VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO);
    // allow free descriptor set
    poolCI.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT |
        VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    poolCI.maxSets       = RHIOptions::GetInstance().MaxDescriptorSetPerPool();
    poolCI.pPoolSizes    = poolSizes;
    poolCI.poolSizeCount = poolSizeCount;

    VkDescriptorPool descriptorPool{VK_NULL_HANDLE};
    VKCHECK(vkCreateDescriptorPool(m_device->GetVkHandle(), &poolCI, nullptr, &descriptorPool));

    if (existed == m_pools.end())
    {
        HashMap<VkDescriptorPool, uint32_t> value{};
        existed = m_pools.insert({poolKey, value}).first;
    }
    HashMap<VkDescriptorPool, uint32_t>& poolDescriptorCount = existed->second;
    poolDescriptorCount[descriptorPool]++;
    *iter = existed;
    return descriptorPool;
}

void VulkanDescriptorPoolManager::UnRefDescriptorPool(VulkanDescriptorPoolsIt poolsIter,
                                                      VkDescriptorPool pool)
{
    auto poolDescriptorIter = poolsIter->second.find(pool);
    poolDescriptorIter->second--;
    if (poolDescriptorIter->second == 0)
    {
        vkDestroyDescriptorPool(m_device->GetVkHandle(), pool, nullptr);
        poolsIter->second.erase(pool);
        if (poolsIter->second.empty())
        {
            m_pools.erase(poolsIter);
        }
    }
}

DescriptorSetHandle VulkanRHI::CreateDescriptorSet(ShaderHandle shaderHandle, uint32_t setIndex)
{
    if (!m_shaderPipelines.contains(shaderHandle))
    {
        LOG_FATAL_ERROR("Pipeline should be created before allocating descriptorSets");
    }

    VulkanShader* shader = TO_VK_SHADER(shaderHandle);
    VulkanDescriptorPoolsIt iter{};
    VkDescriptorPool pool =
        m_descriptorPoolManager->GetOrCreateDescriptorPool(shader->descriptorPoolKey, &iter);
    VERIFY_EXPR(pool != VK_NULL_HANDLE);

    VkDescriptorSetAllocateInfo descriptorSetAllocateInfo{};
    InitVkStruct(descriptorSetAllocateInfo, VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO);
    descriptorSetAllocateInfo.descriptorPool     = pool;
    descriptorSetAllocateInfo.descriptorSetCount = 1;
    descriptorSetAllocateInfo.pSetLayouts        = &shader->descriptorSetLayouts[setIndex];

    VkDescriptorSet vkDescriptorSet{VK_NULL_HANDLE};
    VkResult result = vkAllocateDescriptorSets(m_device->GetVkHandle(), &descriptorSetAllocateInfo,
                                               &vkDescriptorSet);
    if (result != VK_SUCCESS)
    {
        m_descriptorPoolManager->UnRefDescriptorPool(iter, pool);
        LOGE("Failed to allocate descriptor set");
    }

    VulkanDescriptorSet* descriptorSet =
        VersatileResource::Alloc<VulkanDescriptorSet>(m_resourceAllocator);
    descriptorSet->iter           = iter;
    descriptorSet->descriptorPool = pool;
    descriptorSet->descriptorSet  = vkDescriptorSet;

    m_shaderPipelines[shaderHandle]->descriptorSets[setIndex] = descriptorSet;

    return DescriptorSetHandle(descriptorSet);
}

void VulkanRHI::DestroyDescriptorSet(DescriptorSetHandle descriptorSetHandle)
{
    VulkanDescriptorSet* descriptorSet =
        reinterpret_cast<VulkanDescriptorSet*>(descriptorSetHandle.value);

    vkFreeDescriptorSets(m_device->GetVkHandle(), descriptorSet->descriptorPool, 1,
                         &descriptorSet->descriptorSet);

    m_descriptorPoolManager->UnRefDescriptorPool(descriptorSet->iter,
                                                 descriptorSet->descriptorPool);
    VersatileResource::Free(m_resourceAllocator, descriptorSet);
}

void VulkanRHI::UpdateDescriptorSet(DescriptorSetHandle descriptorSetHandle,
                                    const std::vector<ShaderResourceBinding>& resourceBindings)
{
    std::vector<VkWriteDescriptorSet> writes;
    writes.resize(resourceBindings.size());
    // std::vector<VkDescriptorImageInfo> imageInfos;
    // std::vector<VkDescriptorBufferInfo> bufferInfos;
    // std::vector<VkBufferView> bufferViews;

    for (uint32_t i = 0; i < resourceBindings.size(); i++)
    {
        const auto& srb = resourceBindings[i];
        InitVkStruct(writes[i], VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
        writes[i].dstBinding = srb.binding;

        uint32_t numDescriptors = 1;
        // VkDescriptorBufferInfo* currBufferInfos = bufferInfos.data() + bufferInfos.size();
        // VkDescriptorImageInfo* currImageInfos   = imageInfos.data() + imageInfos.size();
        // VkBufferView* currBufferViews           = bufferViews.data() + bufferViews.size();

        switch (srb.type)
        {
            case ShaderResourceType::eSampler:
            {
                numDescriptors = srb.handles.size();
                VkDescriptorImageInfo* imageInfos =
                    ALLOCA_ARRAY(VkDescriptorImageInfo, numDescriptors);
                for (uint32_t j = 0; j < numDescriptors; j++)
                {
                    VkDescriptorImageInfo imageInfo{};
                    imageInfo.sampler     = TO_VK_SAMPLER(srb.handles[j]);
                    imageInfo.imageView   = VK_NULL_HANDLE;
                    imageInfo.imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                    imageInfos[j]         = imageInfo;
                }
                writes[i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
                writes[i].pImageInfo     = imageInfos;
            }
            break;
            case ShaderResourceType::eTexture:
            {
                numDescriptors = srb.handles.size();
                VkDescriptorImageInfo* imageInfos =
                    ALLOCA_ARRAY(VkDescriptorImageInfo, numDescriptors);
                for (uint32_t j = 0; j < numDescriptors; j++)
                {
                    VkDescriptorImageInfo imageInfo{};
                    imageInfo.sampler     = VK_NULL_HANDLE;
                    imageInfo.imageView   = TO_VK_TEXTURE(srb.handles[j])->imageView;
                    imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                    imageInfos[j]         = imageInfo;
                }
                writes[i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
                writes[i].pImageInfo     = imageInfos;
            }
            break;
            case ShaderResourceType::eSamplerWithTexture:
            {
                VERIFY_EXPR(srb.handles.size() % 2 == 0 && srb.handles.size() >= 2);
                numDescriptors = srb.handles.size() / 2;
                VkDescriptorImageInfo* imageInfos =
                    ALLOCA_ARRAY(VkDescriptorImageInfo, numDescriptors);
                for (uint32_t j = 0; j < numDescriptors; j++)
                {
                    VkDescriptorImageInfo imageInfo{};
                    imageInfo.sampler   = TO_VK_SAMPLER(srb.handles[j * 2 + 0]);
                    imageInfo.imageView = TO_VK_TEXTURE(srb.handles[j * 2 + 1])->imageView;
                    imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                    imageInfos[j]         = imageInfo;
                }
                writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                writes[i].pImageInfo     = imageInfos;
            }
            break;
            case ShaderResourceType::eImage:
            {
                numDescriptors = srb.handles.size();
                VkDescriptorImageInfo* imageInfos =
                    ALLOCA_ARRAY(VkDescriptorImageInfo, numDescriptors);
                for (uint32_t j = 0; j < numDescriptors; j++)
                {
                    VkDescriptorImageInfo imageInfo{};
                    imageInfo.sampler     = VK_NULL_HANDLE;
                    imageInfo.imageView   = TO_VK_TEXTURE(srb.handles[j])->imageView;
                    imageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
                    imageInfos[j]         = imageInfo;
                }
                writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                writes[i].pImageInfo     = imageInfos;
            }
            break;
            case ShaderResourceType::eTextureBuffer:
            {
                numDescriptors = srb.handles.size();
                VkDescriptorBufferInfo* bufferInfos =
                    ALLOCA_ARRAY(VkDescriptorBufferInfo, numDescriptors);
                VkBufferView* bufferViews = ALLOCA_ARRAY(VkBufferView, numDescriptors);
                for (uint32_t j = 0; j < numDescriptors; j++)
                {
                    VulkanBuffer* vulkanBuffer = TO_VK_BUFFER(srb.handles[j]);
                    VkDescriptorBufferInfo bufferInfo{};
                    bufferInfo.buffer = vulkanBuffer->buffer;
                    bufferInfo.range  = vulkanBuffer->requiredSize;
                    bufferViews[j]    = vulkanBuffer->bufferView;
                    bufferInfos[j]    = bufferInfo;
                }
                writes[i].descriptorType   = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
                writes[i].pBufferInfo      = bufferInfos;
                writes[i].pTexelBufferView = bufferViews;
            }
            break;
            case ShaderResourceType::eSamplerWithTextureBuffer:
            {
                VERIFY_EXPR(srb.handles.size() % 2 == 0 && srb.handles.size() >= 2);
                numDescriptors = srb.handles.size() / 2;
                VkDescriptorImageInfo* imageInfos =
                    ALLOCA_ARRAY(VkDescriptorImageInfo, numDescriptors);
                VkDescriptorBufferInfo* bufferInfos =
                    ALLOCA_ARRAY(VkDescriptorBufferInfo, numDescriptors);
                VkBufferView* bufferViews = ALLOCA_ARRAY(VkBufferView, numDescriptors);
                for (uint32_t j = 0; j < numDescriptors; j++)
                {
                    VkDescriptorImageInfo imageInfo{};
                    imageInfo.sampler   = TO_VK_SAMPLER(srb.handles[j * 2 + 0]);
                    imageInfo.imageView = VK_NULL_HANDLE;
                    imageInfo.imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                    imageInfos[j]         = imageInfo;

                    VulkanBuffer* vulkanBuffer = TO_VK_BUFFER(srb.handles[j * 2 + 1]);
                    VkDescriptorBufferInfo bufferInfo{};
                    bufferInfo.buffer = vulkanBuffer->buffer;
                    bufferInfo.range  = vulkanBuffer->requiredSize;
                    bufferViews[j]    = vulkanBuffer->bufferView;
                    bufferInfos[j]    = bufferInfo;
                }
                writes[i].descriptorType   = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
                writes[i].pImageInfo       = imageInfos;
                writes[i].pBufferInfo      = bufferInfos;
                writes[i].pTexelBufferView = bufferViews;
            }
            break;
            case ShaderResourceType::eImageBuffer:
            {
                numDescriptors = srb.handles.size();
                VkDescriptorBufferInfo* bufferInfos =
                    ALLOCA_ARRAY(VkDescriptorBufferInfo, numDescriptors);
                VkBufferView* bufferViews = ALLOCA_ARRAY(VkBufferView, numDescriptors);
                for (uint32_t j = 0; j < numDescriptors; j++)
                {
                    VulkanBuffer* vulkanBuffer = TO_VK_BUFFER(srb.handles[j]);
                    VkDescriptorBufferInfo bufferInfo{};
                    bufferInfo.buffer = vulkanBuffer->buffer;
                    bufferInfo.range  = vulkanBuffer->requiredSize;
                    bufferViews[j]    = vulkanBuffer->bufferView;
                    bufferInfos[j]    = bufferInfo;
                }
                writes[i].descriptorType   = VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
                writes[i].pBufferInfo      = bufferInfos;
                writes[i].pTexelBufferView = bufferViews;
            }
            break;
            case ShaderResourceType::eUniformBuffer:
            {
                VulkanBuffer* vulkanBuffer         = TO_VK_BUFFER(srb.handles[0]);
                VkDescriptorBufferInfo* bufferInfo = ALLOCA_SINGLE(VkDescriptorBufferInfo);

                *bufferInfo        = {};
                bufferInfo->buffer = vulkanBuffer->buffer;
                bufferInfo->range  = vulkanBuffer->requiredSize;

                writes[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                writes[i].pBufferInfo    = bufferInfo;
            }
            break;
            case ShaderResourceType::eStorageBuffer:
            {
                VulkanBuffer* vulkanBuffer         = TO_VK_BUFFER(srb.handles[0]);
                VkDescriptorBufferInfo* bufferInfo = ALLOCA_SINGLE(VkDescriptorBufferInfo);

                *bufferInfo        = {};
                bufferInfo->buffer = vulkanBuffer->buffer;
                bufferInfo->range  = vulkanBuffer->requiredSize;

                writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                writes[i].pBufferInfo    = bufferInfo;
            }
            break;
            case ShaderResourceType::eInputAttachment:
            {
                numDescriptors = srb.handles.size();
                VkDescriptorImageInfo* imageInfos =
                    ALLOCA_ARRAY(VkDescriptorImageInfo, numDescriptors);
                for (uint32_t j = 0; j < numDescriptors; j++)
                {
                    VkDescriptorImageInfo imageInfo{};
                    imageInfo.sampler     = VK_NULL_HANDLE;
                    imageInfo.imageView   = TO_VK_TEXTURE(srb.handles[j])->imageView;
                    imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                    imageInfos[j]         = imageInfo;
                }
                writes[i].descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
                writes[i].pImageInfo     = imageInfos;
            }
            break;
            default:
            {
                LOGE("Invalid descriptor type");
            }
        }
        writes[i].descriptorCount = numDescriptors;
    }
    for (auto& write : writes)
    {
        VulkanDescriptorSet* descriptorSet =
            reinterpret_cast<VulkanDescriptorSet*>(descriptorSetHandle.value);
        write.dstSet = descriptorSet->descriptorSet;
    }
    vkUpdateDescriptorSets(m_device->GetVkHandle(), writes.size(), writes.data(), 0, nullptr);
}
} // namespace zen::rhi