#include "Graphics/RHI/RHICommon.h"
#include "Graphics/RHI/RHIResource.h"
#include "Graphics/VulkanRHI/VulkanRHI.h"
#include "Graphics/VulkanRHI/VulkanTypes.h"
#include "Graphics/VulkanRHI/VulkanPipeline.h"
#include "Graphics/VulkanRHI/VulkanResourceAllocator.h"
#include "Graphics/RHI/RHIOptions.h"
#include "Graphics/RHI/RHIShaderUtil.h"
#include "Graphics/VulkanRHI/VulkanCommon.h"
#include "Graphics/VulkanRHI/VulkanDevice.h"
#include "Graphics/VulkanRHI/VulkanDescriptorPool.h"
#include "Graphics/VulkanRHI/VulkanTypes.h"
#include "Platform/FileSystem.h"
#include "Templates/HeapVector.h"
#include "Utils/Errors.h"
#include "Utils/Helpers.h"

namespace zen
{
RHIShader* VulkanResourceFactory::CreateShader(const RHIShaderCreateInfo& createInfo)
{
    RHIShader* pShader = VulkanShader::CreateObject(createInfo);

    return pShader;
}

RHIPipeline* VulkanResourceFactory::CreatePipeline(const RHIComputePipelineCreateInfo& createInfo)
{
    RHIPipeline* pComputePipeline = VulkanPipeline::CreateObject(createInfo);

    return pComputePipeline;
}

RHIPipeline* VulkanResourceFactory::CreatePipeline(const RHIGfxPipelineCreateInfo& createInfo)
{
    RHIPipeline* pGfxPipeline = VulkanPipeline::CreateObject(createInfo);

    return pGfxPipeline;
}

RHIPipeline* VulkanRHI::CreatePipeline(const RHIComputePipelineCreateInfo& createInfo)
{
    return GDynamicRHI->GetResourceFactory()->CreatePipeline(createInfo);
}

RHIPipeline* VulkanRHI::CreatePipeline(const RHIGfxPipelineCreateInfo& createInfo)
{
    return GDynamicRHI->GetResourceFactory()->CreatePipeline(createInfo);
}

void VulkanRHI::DestroyPipeline(RHIPipeline* pPipeline)
{
    pPipeline->ReleaseReference();
    // vkDestroyPipeline(GetVkDevice(), pipeline->pipeline, nullptr);
    // VersatileResource::Free(m_resourceAllocator, pipeline);
}

RHIShader* VulkanRHI::CreateShader(const RHIShaderCreateInfo& createInfo)
{
    return GDynamicRHI->GetResourceFactory()->CreateShader(createInfo);
}

void VulkanRHI::DestroyShader(RHIShader* pShader)
{
    pShader->ReleaseReference();
}

VulkanShader* VulkanShader::CreateObject(const RHIShaderCreateInfo& createInfo)
{
    VulkanShader* pShader =
        VersatileResource::AllocMem<VulkanShader>(GVulkanRHI->GetResourceAllocator());

    new (pShader) VulkanShader(createInfo);

    pShader->Init();

    return pShader;
}

// RHIDescriptorSet* VulkanShader::CreateDescriptorSet(uint32_t setIndex)
// {
//     VulkanDescriptorSet* pDescriptorSet = nullptr;
//     const bool validSet =
//         setIndex < GetNumDescriptorSetLayouts() && !(setIndex == 0 && HasGlobalBindlessSet());
//     VERIFY_EXPR(validSet);
//     if (validSet)
//     {
//         pDescriptorSet =
//             VersatileResource::AllocMem<VulkanDescriptorSet>(GVulkanRHI->GetResourceAllocator());
//         new (pDescriptorSet) VulkanDescriptorSet(this, setIndex);
//         pDescriptorSet->Init();
//     }
//     return pDescriptorSet;
// }

void VulkanShader::Init()
{
    for (uint32_t i = 0; i < ToUnderlying(RHIShaderStage::eMax); i++)
    {
        RHIShaderStage stage = static_cast<RHIShaderStage>(i);

        if (m_shaderGroupSPIRV->HasShaderStage(stage))
        {
            m_shaderGroupSPIRV->SetStageSPIRV(
                stage, platform::FileSystem::LoadSpvFile(m_spirvFileName[i]));
        }
    }

    RHIShaderGroupInfo sgInfo{};
    RHIShaderUtil::ReflectShaderGroupInfo(m_shaderGroupSPIRV, sgInfo);
    sgInfo.name = m_name;
    m_SRDTable  = sgInfo.SRDTable;

    for (SmallVector<RHIShaderResourceDescriptor> const& setSRDs : m_SRDTable)
    {
        for (const RHIShaderResourceDescriptor& srd : setSRDs)
        {
            ++m_SRDCount[ToUnderlying(srd.type)];
            m_namedSRDLut[srd.name] = &srd;
        }
    }

    if (!m_specializationConstants.empty())
    {
        // set specialization constants
        for (RHIShaderSpecializationConstant& spc : sgInfo.specializationConstants)
        {
            if (!m_specializationConstants.contains(spc.constantId))
            {
                continue;
            }

            switch (spc.type)
            {
                case RHIShaderSpecializationConstantType::eBool:
                    spc.boolValue = static_cast<bool>(m_specializationConstants.at(spc.constantId));
                    break;
                case RHIShaderSpecializationConstantType::eInt:
                    spc.intValue = m_specializationConstants.at(spc.constantId);
                    break;
                case RHIShaderSpecializationConstantType::eFloat:
                    spc.floatValue =
                        static_cast<float>(m_specializationConstants.at(spc.constantId));
                    break;

                default: break;
            }
        }
    }

    // Create specialization info from tracked state. This is shared by all shaders.
    const HeapVector<RHIShaderSpecializationConstant>& specConstants =
        sgInfo.specializationConstants;

    if (!specConstants.empty())
    {
        m_spcMapEntries.resize(specConstants.size());

        for (uint32_t i = 0; i < specConstants.size(); i++)
        {
            VkSpecializationMapEntry& entry = m_spcMapEntries[i];
            // fill in data
            entry.constantID = specConstants[i].constantId;

            switch (specConstants[i].type)
            {
                case RHIShaderSpecializationConstantType::eBool:
                {
                    entry.size   = sizeof(bool);
                    entry.offset = reinterpret_cast<const char*>(&specConstants[i].boolValue) -
                        reinterpret_cast<const char*>(specConstants.data());
                }
                break;

                case RHIShaderSpecializationConstantType::eInt:
                {
                    entry.size   = sizeof(int);
                    entry.offset = reinterpret_cast<const char*>(&specConstants[i].intValue) -
                        reinterpret_cast<const char*>(specConstants.data());
                }
                break;

                case RHIShaderSpecializationConstantType::eFloat:
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

    m_specializationInfo.mapEntryCount = static_cast<uint32_t>(m_spcMapEntries.size());
    m_specializationInfo.pMapEntries   = m_spcMapEntries.empty() ? nullptr : m_spcMapEntries.data();
    m_specializationInfo.dataSize = specConstants.size() * sizeof(RHIShaderSpecializationConstant);
    m_specializationInfo.pData    = specConstants.empty() ? nullptr : specConstants.data();

    m_stageCreateInfos.reserve(m_shaderGroupSPIRV->GetStageCount());

    for (uint32_t i = 0; i < ToUnderlying(RHIShaderStage::eMax); i++)
    {
        RHIShaderStage stage = static_cast<RHIShaderStage>(i);

        if (m_shaderGroupSPIRV->HasShaderStage(stage))
        {
            const HeapVector<uint8_t>& spirvCode = m_shaderGroupSPIRV->GetStageSPIRV(stage);

            VkShaderModuleCreateInfo shaderModuleCI;
            InitVkStruct(shaderModuleCI, VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
            shaderModuleCI.codeSize = spirvCode.size();
            shaderModuleCI.pCode    = reinterpret_cast<const uint32_t*>(spirvCode.data());
            VkShaderModule module{VK_NULL_HANDLE};
            VKCHECK(
                vkCreateShaderModule(GVulkanRHI->GetVkDevice(), &shaderModuleCI, nullptr, &module));

            VkPipelineShaderStageCreateInfo pipelineShaderStageCI;
            InitVkStruct(pipelineShaderStageCI,
                         VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO);
            pipelineShaderStageCI.stage               = ShaderStageToVkShaderStageFlagBits(stage);
            pipelineShaderStageCI.pName               = "main";
            pipelineShaderStageCI.module              = module;
            pipelineShaderStageCI.pSpecializationInfo = &m_specializationInfo;

            m_stageCreateInfos.push_back(pipelineShaderStageCI);
        }
    }

    // Create descriptor pool key while createing descriptor set layouts
    // VulkanDescriptorPoolKey descriptorPoolKey{};
    // Create descriptorSetLayouts
    const size_t setCount = m_SRDTable.size();
    HeapVector<HeapVector<VkDescriptorSetLayoutBinding>> dsBindings(setCount);
    m_descriptorSetInfos.resize(setCount);
    m_descriptorSetLayouts.resize(setCount);

    for (uint32_t i = 0; i < setCount; i++)
    {
        // process global bindless descriptor set 0
        if (i == kGlobalBindlessHeapIndex && !m_SRDTable[kGlobalBindlessHeapIndex].empty())
        {
            bool isBindless = false;

            for (const RHIShaderResourceDescriptor& srd : m_SRDTable[0])
            {
                if (srd.bindless)
                {
                    isBindless = true;
                    break;
                }
            }

            if (isBindless)
            {
                const VkDescriptorSetLayout dsLayout =
                    GVulkanRHI->GetBindlessDescriptorPoolManager()->GetGlobalBindlessLayout();
                VERIFY_EXPR(dsLayout != nullptr);

                m_descriptorSetLayouts[i]             = dsLayout;
                m_descriptorSetInfos[i].layoutId      = 0;
                m_descriptorSetInfos[i].poolKey       = {};
                m_descriptorSetInfos[i].variableCount = 0;

                m_hasGlobalBindlessSet = true;

                continue;
            }
        }

        uint32_t setVariableCount = 0;
        bool needUABFlag          = false;
        VulkanDescriptorPoolKey setPoolKey{};
        HeapVector<VkDescriptorBindingFlags> bindingFlags;
        size_t dsLayoutHash = 0;

        // collect bindings for set i
        dsBindings[i].reserve(m_SRDTable[i].size());

        for (const RHIShaderResourceDescriptor& srd : m_SRDTable[i])
        {
            VkDescriptorSetLayoutBinding binding{};
            binding.binding        = srd.binding;
            binding.descriptorType = ShaderResourceTypeToVkDescriptorType(srd.type);
            binding.stageFlags     = ShaderStageFlagsBitsToVkShaderStageFlags(srd.stageFlags);

            if (srd.bindless)
            {
                binding.descriptorCount =
                    GVulkanRHI->GetDevice()->GetDescriptorSetUpdateAfterBindLimit(
                        binding.descriptorType);
                setVariableCount = binding.descriptorCount;
                bindingFlags.push_back(VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                                       VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
                                       VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT);
                needUABFlag = true;
            }
            else
            {
                binding.descriptorCount = srd.arraySize;
                bindingFlags.push_back(
                    binding.descriptorCount > 1 ? VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT : 0);
            }

            dsBindings[i].push_back(binding);

            setPoolKey.descriptorCount[ToUnderlying(srd.type)] += binding.descriptorCount;

            util::HashCombine(dsLayoutHash, binding.binding);
            util::HashCombine(dsLayoutHash, binding.descriptorType);
            util::HashCombine(dsLayoutHash, binding.descriptorCount);
            util::HashCombine(dsLayoutHash, binding.stageFlags);
            util::HashCombine(dsLayoutHash, bindingFlags.back());
        }

        // create descriptor set layout for set i
        VkDescriptorSetLayoutCreateInfo layoutCI;
        InitVkStruct(layoutCI, VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);
        layoutCI.bindingCount = static_cast<uint32_t>(dsBindings[i].size());
        layoutCI.pBindings    = dsBindings[i].empty() ? nullptr : dsBindings[i].data();
        layoutCI.flags =
            needUABFlag ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT : 0;
        // set binding flags
        VkDescriptorSetLayoutBindingFlagsCreateInfo bindingFlagsCreateInfo;
        InitVkStruct(bindingFlagsCreateInfo,
                     VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO);
        bindingFlagsCreateInfo.bindingCount  = static_cast<uint32_t>(bindingFlags.size());
        bindingFlagsCreateInfo.pBindingFlags = bindingFlags.data();

        layoutCI.pNext = &bindingFlagsCreateInfo;

        VkDescriptorSetLayout dsLayout{VK_NULL_HANDLE};
        VKCHECK(
            vkCreateDescriptorSetLayout(GVulkanRHI->GetVkDevice(), &layoutCI, nullptr, &dsLayout));

        util::HashCombine(dsLayoutHash, layoutCI.flags);

        m_descriptorSetLayouts[i]             = dsLayout;
        m_descriptorSetInfos[i].poolKey       = setPoolKey;
        m_descriptorSetInfos[i].ownsLayout    = true;
        m_descriptorSetInfos[i].variableCount = setVariableCount;
        m_descriptorSetInfos[i].layoutId =
            GVulkanRHI->GetDescriptorPoolManager2()->GetOrCreateLayoutId(dsLayoutHash);
    }

    // vertex input state
    const size_t vertexInputCount = sgInfo.vertexInputAttributes.size();

    if (vertexInputCount > 0)
    {
        m_vertexInputInfo.vkAttributes.resize(vertexInputCount);
        // packed data for vertex inputs, only 1 binding
        VkVertexInputBindingDescription vkBinding{};
        vkBinding.binding   = 0;
        vkBinding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        vkBinding.stride    = sgInfo.vertexBindingStride;
        m_vertexInputInfo.vkBindings.emplace_back(vkBinding);

        // populate attributes
        for (uint32_t i = 0; i < vertexInputCount; i++)
        {
            m_vertexInputInfo.vkAttributes[i].binding  = sgInfo.vertexInputAttributes[i].binding;
            m_vertexInputInfo.vkAttributes[i].location = sgInfo.vertexInputAttributes[i].location;
            m_vertexInputInfo.vkAttributes[i].offset   = sgInfo.vertexInputAttributes[i].offset;
            m_vertexInputInfo.vkAttributes[i].format =
                static_cast<VkFormat>(sgInfo.vertexInputAttributes[i].format);
        }

        InitVkStruct(m_vertexInputInfo.stateCI,
                     VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO);
        m_vertexInputInfo.stateCI.vertexAttributeDescriptionCount =
            m_vertexInputInfo.vkAttributes.size();
        m_vertexInputInfo.stateCI.pVertexAttributeDescriptions =
            m_vertexInputInfo.vkAttributes.data();
        m_vertexInputInfo.stateCI.vertexBindingDescriptionCount =
            m_vertexInputInfo.vkBindings.size();
        m_vertexInputInfo.stateCI.pVertexBindingDescriptions = m_vertexInputInfo.vkBindings.data();
    }
    else
    {
        InitVkStruct(m_vertexInputInfo.stateCI,
                     VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO);
        m_vertexInputInfo.stateCI.vertexAttributeDescriptionCount = 0;
        m_vertexInputInfo.stateCI.pVertexAttributeDescriptions    = nullptr;
        m_vertexInputInfo.stateCI.vertexBindingDescriptionCount   = 0;
        m_vertexInputInfo.stateCI.pVertexBindingDescriptions      = nullptr;
    }

    // Create pipeline layout
    VkPipelineLayoutCreateInfo pipelineLayoutCI;
    InitVkStruct(pipelineLayoutCI, VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
    pipelineLayoutCI.pSetLayouts    = m_descriptorSetLayouts.data();
    pipelineLayoutCI.setLayoutCount = m_descriptorSetLayouts.size();
    VkPushConstantRange prc{};
    const RHIShaderGroupInfo::ShaderPushConstants& pc = sgInfo.pushConstants;

    if (pc.size > 0)
    {
        prc.stageFlags = ShaderStageFlagsBitsToVkShaderStageFlags(pc.stageFlags);
        prc.size       = pc.size;
        pipelineLayoutCI.pushConstantRangeCount = 1;
        pipelineLayoutCI.pPushConstantRanges    = &prc;
    }

    vkCreatePipelineLayout(GVulkanRHI->GetVkDevice(), &pipelineLayoutCI, nullptr,
                           &m_pipelineLayout);

    // descriptorPoolKey = descriptorPoolKey;
    m_pushConstantsStageFlags =
        ShaderStageFlagsBitsToVkShaderStageFlags(sgInfo.pushConstants.stageFlags);

    const NameID debugName(fmt::format("{}_PipelineLayout", sgInfo.name.CStr()));
    GVulkanRHI->GetDevice()->SetObjectName(VK_OBJECT_TYPE_PIPELINE_LAYOUT,
                                           reinterpret_cast<uint64_t>(m_pipelineLayout), debugName);
}

void VulkanShader::Destroy()
{
    for (uint32_t i = 0; i < m_descriptorSetInfos.size(); ++i)
    {
        if (m_descriptorSetInfos[i].ownsLayout)
        {
            vkDestroyDescriptorSetLayout(GVulkanRHI->GetVkDevice(), m_descriptorSetLayouts[i],
                                         nullptr);
        }
    }

    for (VkPipelineShaderStageCreateInfo& stageCreateInfo : m_stageCreateInfos)
    {
        vkDestroyShaderModule(GVulkanRHI->GetVkDevice(), stageCreateInfo.module, nullptr);
    }

    if (m_pipelineLayout != VK_NULL_HANDLE)
    {
        vkDestroyPipelineLayout(GVulkanRHI->GetVkDevice(), m_pipelineLayout, nullptr);
    }

    this->~VulkanShader();
    VersatileResource::Free(GVulkanRHI->GetResourceAllocator(), this);
}

VulkanPipeline* VulkanPipeline::CreateObject(const RHIGfxPipelineCreateInfo& createInfo)
{
    VulkanPipeline* pGfxPipeline =
        VersatileResource::AllocMem<VulkanPipeline>(GVulkanRHI->GetResourceAllocator());

    new (pGfxPipeline) VulkanPipeline(createInfo);

    pGfxPipeline->Init();

    return pGfxPipeline;
}

VulkanPipeline* VulkanPipeline::CreateObject(const RHIComputePipelineCreateInfo& createInfo)
{
    VulkanPipeline* pCompPipeline =
        VersatileResource::AllocMem<VulkanPipeline>(GVulkanRHI->GetResourceAllocator());

    new (pCompPipeline) VulkanPipeline(createInfo);

    pCompPipeline->Init();

    return pCompPipeline;
}

void VulkanPipeline::Init()
{
    if (m_type == RHIPipelineType::eCompute)
    {
        InitCompute();
        m_bindPoint = VK_PIPELINE_BIND_POINT_COMPUTE;
    }
    else if (m_type == RHIPipelineType::eGraphics)
    {
        InitGraphics();
        m_bindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    }
}

void VulkanPipeline::Destroy()
{
    vkDestroyPipeline(GVulkanRHI->GetVkDevice(), m_vkPipeline, nullptr);
    this->~VulkanPipeline();
    VersatileResource::Free(GVulkanRHI->GetResourceAllocator(), this);
}

void VulkanPipeline::InitGraphics()
{
    // Input Assembly
    VkPipelineInputAssemblyStateCreateInfo IAStateCI;
    InitVkStruct(IAStateCI, VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO);
    IAStateCI.topology               = ToVkPrimitiveTopology(m_gfxStates.primitiveType);
    IAStateCI.primitiveRestartEnable = VK_FALSE;

    // Viewport State
    VkPipelineViewportStateCreateInfo VPStateCI;
    InitVkStruct(VPStateCI, VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO);
    VPStateCI.scissorCount  = 1;
    VPStateCI.viewportCount = 1;

    // Rasterization State
    const RHIGfxPipelineRasterizationState& rasterizationState = m_gfxStates.rasterizationState;
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
    const RHIGfxPipelineMultiSampleState& multisampleState = m_gfxStates.multiSampleState;
    VkPipelineMultisampleStateCreateInfo MSStateCI;
    InitVkStruct(MSStateCI, VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO);
    MSStateCI.rasterizationSamples  = ToVkSampleCountFlagBits(multisampleState.sampleCount);
    MSStateCI.sampleShadingEnable   = multisampleState.enableSampleShading;
    MSStateCI.minSampleShading      = multisampleState.minSampleShading;
    MSStateCI.alphaToCoverageEnable = multisampleState.enableAlphaToCoverage;
    MSStateCI.alphaToOneEnable      = multisampleState.enableAlphaToOne;
    MSStateCI.pSampleMask           = &multisampleState.sampleMasks;

    // Depth Stencil
    const RHIGfxPipelineDepthStencilState& depthStencilState = m_gfxStates.depthStencilState;
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
    const RHIGfxPipelineColorBlendState& colorBlendState = m_gfxStates.colorBlendState;
    HeapVector<VkPipelineColorBlendAttachmentState> vkCBAttStates;
    vkCBAttStates.resize(colorBlendState.attachmentsMask.Count());

    for (uint32_t i : colorBlendState.attachmentsMask)
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
        vkCBAttStates[i].alphaBlendOp   = ToVkBlendOp(colorBlendState.attachments[i].alphaBlendOp);
        vkCBAttStates[i].colorWriteMask = colorBlendState.attachments[i].colorWriteMask;

        // vkCBAttStates[i].colorWriteMask |= VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        //     VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

        // if (colorBlendState.attachments[i].writeR)
        // {
        //     vkCBAttStates[i].colorWriteMask |= VK_COLOR_COMPONENT_R_BIT;
        // }
        // if (colorBlendState.attachments[i].writeG)
        // {
        //     vkCBAttStates[i].colorWriteMask |= VK_COLOR_COMPONENT_G_BIT;
        // }
        // if (colorBlendState.attachments[i].writeB)
        // {
        //     vkCBAttStates[i].colorWriteMask |= VK_COLOR_COMPONENT_B_BIT;
        // }
        // if (colorBlendState.attachments[i].writeA)
        // {
        //     vkCBAttStates[i].colorWriteMask |= VK_COLOR_COMPONENT_A_BIT;
        // }
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

    uint32_t numDynamicStates = m_gfxStates.dynamicStates.enabledStates.Count();
    HeapVector<VkDynamicState> vkDynamicStates(numDynamicStates);

    for (uint32_t i : m_gfxStates.dynamicStates.enabledStates)
    {
        vkDynamicStates[i] = ToVkDynamicState(static_cast<RHIDynamicState>(i));
    }

    dynamicStateCI.dynamicStateCount = numDynamicStates;
    dynamicStateCI.pDynamicStates    = numDynamicStates == 0 ? nullptr : vkDynamicStates.data();

    VulkanShader* pShader = TO_VK_SHADER(m_pShader);

    VkGraphicsPipelineCreateInfo pipelineCI;
    InitVkStruct(pipelineCI, VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO);
    pipelineCI.stageCount          = pShader->GetNumShaderStages();
    pipelineCI.pStages             = pShader->GetStageCreateInfoData();
    pipelineCI.pVertexInputState   = pShader->GetVertexInputStateCreateInfoData();
    pipelineCI.pInputAssemblyState = &IAStateCI;
    pipelineCI.pViewportState      = &VPStateCI;
    pipelineCI.pRasterizationState = &rasterizationCI;
    pipelineCI.pMultisampleState   = &MSStateCI;
    pipelineCI.pDepthStencilState  = &DSStateCI;
    pipelineCI.pColorBlendState    = &CBStateCI;
    pipelineCI.pDynamicState       = &dynamicStateCI;
    pipelineCI.layout              = pShader->GetVkPipelineLayout();
    pipelineCI.subpass             = m_subpassIdx;

    HeapVector<VkFormat> colorAttachmentFormats;
    VkPipelineRenderingCreateInfoKHR renderingCI;

    if (!RHIOptions::GetInstance().UseDynamicRendering())
    {
        // build render pass here
        pipelineCI.renderPass = GVulkanRHI->GetOrCreateRenderPass(m_pRenderingLayout);
    }
    else
    {
        pipelineCI.renderPass = VK_NULL_HANDLE;
        colorAttachmentFormats.reserve(m_pRenderingLayout->numColorRenderTargets);

        for (uint32_t i = 0; i < m_pRenderingLayout->numColorRenderTargets; i++)
        {
            VkFormat colorFormat = ToVkFormat(m_pRenderingLayout->colorRenderTargets[i].format);
            colorAttachmentFormats.emplace_back(colorFormat);
        }

        InitVkStruct(renderingCI, VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR);
        renderingCI.colorAttachmentCount    = m_pRenderingLayout->numColorRenderTargets;
        renderingCI.pColorAttachmentFormats = colorAttachmentFormats.data();

        if (m_pRenderingLayout->hasDepthStencilRT)
        {
            VkFormat depthFormat = ToVkFormat(m_pRenderingLayout->depthStencilRenderTarget.format);
            renderingCI.depthAttachmentFormat   = depthFormat;
            renderingCI.stencilAttachmentFormat = depthFormat;
        }

        pipelineCI.pNext = &renderingCI;
    }

    VKCHECK(vkCreateGraphicsPipelines(GVulkanRHI->GetVkDevice(), nullptr, 1, &pipelineCI, nullptr,
                                      &m_vkPipeline));

    m_pushConstantsStageFlags = pShader->GetPushConstantsStageFlags();
}

void VulkanPipeline::InitCompute()
{
    VulkanShader* pShader = TO_VK_SHADER(m_pShader);

    VkComputePipelineCreateInfo pipelineCI{};
    pipelineCI.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineCI.stage  = pShader->GetStageCreateInfoData()[0];
    pipelineCI.layout = pShader->GetVkPipelineLayout();

    VKCHECK(vkCreateComputePipelines(GVulkanRHI->GetVkDevice(), nullptr, 1, &pipelineCI, nullptr,
                                     &m_vkPipeline));
    m_pushConstantsStageFlags = pShader->GetPushConstantsStageFlags();
}

// RHIPipeline* VulkanRHI::CreateComputePipeline(RHIShader* shaderHandle)
// {
//     VulkanShader* shader = TO_VK_SHADER(shaderHandle);
//
//     VkComputePipelineCreateInfo pipelineCI{};
//     pipelineCI.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
//     pipelineCI.stage  = shader->GetStageCreateInfoData()[0];
//     pipelineCI.layout = shader->GetVkPipelineLayout();
//
//     VkPipeline computePipeline{VK_NULL_HANDLE};
//     VKCHECK(vkCreateComputePipelines(GetVkDevice(), nullptr, 1, &pipelineCI, nullptr,
//                                      &computePipeline));
//
//     VulkanPipeline* pipeline = VersatileResource::Alloc<VulkanPipeline>(m_resourceAllocator);
//     pipeline->pipeline       = computePipeline;
//     pipeline->pipelineLayout = shader->GetVkPipelineLayout();
//     // pipeline->descriptorSetCount = shader->GetNumDescriptorSetLayouts();
//     pipeline->pushConstantsStageFlags = shader->GetPushConstantsStageFlags();
//
//     // m_shaderPipelines[shaderHandle] = pipeline;
//
//     return RHIPipeline * (pipeline);
// }

} // namespace zen
