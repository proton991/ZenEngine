#include "Graphics/VulkanRHI/VulkanRHI.h"
#include "Graphics/VulkanRHI/VulkanPipeline.h"
#include "Graphics/RHI/RHIOptions.h"
#include "Graphics/RHI/RHIShaderUtil.h"
#include "Graphics/VulkanRHI/VulkanCommon.h"
#include "Graphics/VulkanRHI/VulkanDevice.h"
#include "Graphics/VulkanRHI/VulkanResourceAllocator.h"
#include "Graphics/VulkanRHI/VulkanTypes.h"
#include "Platform/FileSystem.h"

namespace zen
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

void VulkanRHI::DestroyPipeline(RHIPipeline* pipeline)
{
    if (pipeline->GetResourceTag() == "")
    {
        int a = 1;
    }
    pipeline->ReleaseReference();
    // vkDestroyPipeline(GetVkDevice(), pipeline->pipeline, nullptr);
    // VersatileResource::Free(m_resourceAllocator, pipeline);
}


RHIShader* VulkanRHI::CreateShader(const RHIShaderCreateInfo& createInfo)
{
    return GDynamicRHI->GetResourceFactory()->CreateShader(createInfo);
}

void VulkanRHI::DestroyShader(RHIShader* shader)
{
    shader->ReleaseReference();
}

VulkanShader* VulkanShader::CreateObject(const RHIShaderCreateInfo& createInfo)
{
    VulkanShader* pShader =
        VersatileResource::AllocMem<VulkanShader>(GVulkanRHI->GetResourceAllocator());

    new (pShader) VulkanShader(createInfo);

    pShader->Init();

    return pShader;
}

RHIDescriptorSet* VulkanShader::CreateDescriptorSet(uint32_t setIndex)
{
    VulkanDescriptorSet* pDescriptorSet =
        VersatileResource::AllocMem<VulkanDescriptorSet>(GVulkanRHI->GetResourceAllocator());

    new (pDescriptorSet) VulkanDescriptorSet(this, setIndex);

    pDescriptorSet->Init();

    return pDescriptorSet;
}


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

    if (!m_specializationConstants.empty())
    {
        // set specialization constants
        for (auto& spc : sgInfo.specializationConstants)
        {
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
    const auto& specConstants = sgInfo.specializationConstants;
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

    for (auto& kv : sgInfo.sprivCode)
    {
        VkShaderModuleCreateInfo shaderModuleCI;
        InitVkStruct(shaderModuleCI, VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
        shaderModuleCI.codeSize = kv.second.size();
        shaderModuleCI.pCode    = reinterpret_cast<const uint32_t*>(kv.second.data());
        VkShaderModule module{VK_NULL_HANDLE};
        VKCHECK(vkCreateShaderModule(GVulkanRHI->GetVkDevice(), &shaderModuleCI, nullptr, &module));

        VkPipelineShaderStageCreateInfo pipelineShaderStageCI;
        InitVkStruct(pipelineShaderStageCI, VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO);
        pipelineShaderStageCI.stage               = ShaderStageToVkShaderStageFlagBits(kv.first);
        pipelineShaderStageCI.pName               = "main";
        pipelineShaderStageCI.module              = module;
        pipelineShaderStageCI.pSpecializationInfo = &m_specializationInfo;

        m_stageCreateInfos.push_back(pipelineShaderStageCI);
    }
    // Create descriptor pool key while createing descriptor set layouts
    // VulkanDescriptorPoolKey descriptorPoolKey{};
    // Create descriptorSetLayouts
    std::vector<std::vector<VkDescriptorSetLayoutBinding>> dsBindings;
    const auto setCount = sgInfo.SRDTable.size();
    dsBindings.resize(setCount);
    for (uint32_t i = 0; i < setCount; i++)
    {
        std::vector<VkDescriptorBindingFlags> bindingFlags;
        // collect bindings for set i
        for (uint32_t j = 0; j < sgInfo.SRDTable[i].size(); j++)
        {
            const RHIShaderResourceDescriptor& srd = sgInfo.SRDTable[i][j];
            VkDescriptorSetLayoutBinding binding{};
            binding.binding        = srd.binding;
            binding.descriptorType = ShaderResourceTypeToVkDescriptorType(srd.type);
            binding.descriptorCount =
                DecideDescriptorCount(binding.descriptorType, srd.arraySize,
                                      GVulkanRHI->GetDevice()->GetDescriptorIndexingProperties());
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
            m_descriptorPoolKey.descriptorCount[ToUnderlying(srd.type)] += binding.descriptorCount;
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
        VKCHECK(vkCreateDescriptorSetLayout(GVulkanRHI->GetVkDevice(), &layoutCI, nullptr,
                                            &descriptorSetLayout));
        m_descriptorSetLayouts.push_back(descriptorSetLayout);
    }
    // vertex input state
    const auto vertexInputCount = sgInfo.vertexInputAttributes.size();
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
    vkCreatePipelineLayout(GVulkanRHI->GetVkDevice(), &pipelineLayoutCI, nullptr,
                           &m_pipelineLayout);

    // descriptorPoolKey = descriptorPoolKey;
    m_pushConstantsStageFlags =
        ShaderStageFlagsBitsToVkShaderStageFlags(sgInfo.pushConstants.stageFlags);

    auto debugName = sgInfo.name + "_PipelineLayout";
    GVulkanRHI->GetDevice()->SetObjectName(VK_OBJECT_TYPE_PIPELINE_LAYOUT,
                                           reinterpret_cast<uint64_t>(m_pipelineLayout),
                                           debugName.c_str());
}

void VulkanShader::Destroy()
{
    for (auto& dsLayout : m_descriptorSetLayouts)
    {
        vkDestroyDescriptorSetLayout(GVulkanRHI->GetVkDevice(), dsLayout, nullptr);
    }

    for (auto& stageCreateInfo : m_stageCreateInfos)
    {
        vkDestroyShaderModule(GVulkanRHI->GetVkDevice(), stageCreateInfo.module, nullptr);
    }
    if (m_pipelineLayout != VK_NULL_HANDLE)
    {
        vkDestroyPipelineLayout(GVulkanRHI->GetVkDevice(), m_pipelineLayout, nullptr);
    }
    VersatileResource::Free(GVulkanRHI->GetResourceAllocator(), this);
}

// ShaderHandle VulkanRHI::CreateShader(const RHIShaderGroupInfo& sgInfo)
// {
//     VulkanShader* shader = VersatileResource::Alloc<VulkanShader>(m_resourceAllocator);
//
//     // Create specialization info from tracked state. This is shared by all shaders.
//     const auto& specConstants = sgInfo.specializationConstants;
//     if (!specConstants.empty())
//     {
//         shader->m_spcMapEntries.resize(specConstants.size());
//         for (uint32_t i = 0; i < specConstants.size(); i++)
//         {
//             VkSpecializationMapEntry& entry = shader->m_spcMapEntries[i];
//             // fill in data
//             entry.constantID = specConstants[i].constantId;
//             switch (specConstants[i].type)
//             {
//
//                 case RHIShaderSpecializationConstantType::eBool:
//                 {
//                     entry.size   = sizeof(bool);
//                     entry.offset = reinterpret_cast<const char*>(&specConstants[i].boolValue) -
//                         reinterpret_cast<const char*>(specConstants.data());
//                 }
//                 break;
//                 case RHIShaderSpecializationConstantType::eInt:
//                 {
//                     entry.size   = sizeof(int);
//                     entry.offset = reinterpret_cast<const char*>(&specConstants[i].intValue) -
//                         reinterpret_cast<const char*>(specConstants.data());
//                 }
//                 break;
//                 case RHIShaderSpecializationConstantType::eFloat:
//                 {
//                     entry.size   = sizeof(float);
//                     entry.offset = reinterpret_cast<const char*>(&specConstants[i].floatValue) -
//                         reinterpret_cast<const char*>(specConstants.data());
//                 }
//                 break;
//                 default: break;
//             }
//         }
//     }
//     shader->m_specializationInfo.mapEntryCount = static_cast<uint32_t>(shader->m_spcMapEntries.size());
//     shader->m_specializationInfo.pMapEntries =
//         shader->m_spcMapEntries.empty() ? nullptr : shader->m_spcMapEntries.data();
//     shader->m_specializationInfo.dataSize =
//         specConstants.size() * sizeof(RHIShaderSpecializationConstant);
//     shader->m_specializationInfo.pData = specConstants.empty() ? nullptr : specConstants.data();
//
//     for (auto& kv : sgInfo.sprivCode)
//     {
//         VkShaderModuleCreateInfo shaderModuleCI;
//         InitVkStruct(shaderModuleCI, VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
//         shaderModuleCI.codeSize = kv.second.size();
//         shaderModuleCI.pCode    = reinterpret_cast<const uint32_t*>(kv.second.data());
//         VkShaderModule module{VK_NULL_HANDLE};
//         VKCHECK(vkCreateShaderModule(GetVkDevice(), &shaderModuleCI, nullptr, &module));
//
//         VkPipelineShaderStageCreateInfo pipelineShaderStageCI;
//         InitVkStruct(pipelineShaderStageCI, VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO);
//         pipelineShaderStageCI.stage               = ShaderStageToVkShaderStageFlagBits(kv.first);
//         pipelineShaderStageCI.pName               = "main";
//         pipelineShaderStageCI.module              = module;
//         pipelineShaderStageCI.pSpecializationInfo = &shader->m_specializationInfo;
//
//         shader->m_stageCreateInfos.push_back(pipelineShaderStageCI);
//     }
//     // Create descriptor pool key while createing descriptor set layouts
//     VulkanDescriptorPoolKey descriptorPoolKey{};
//     // Create descriptorSetLayouts
//     std::vector<std::vector<VkDescriptorSetLayoutBinding>> dsBindings;
//     const auto setCount = sgInfo.SRDs.size();
//     dsBindings.resize(setCount);
//     for (uint32_t i = 0; i < setCount; i++)
//     {
//         std::vector<VkDescriptorBindingFlags> bindingFlags;
//         // collect bindings for set i
//         for (uint32_t j = 0; j < sgInfo.SRDs[i].size(); j++)
//         {
//             const RHIShaderResourceDescriptor& srd = sgInfo.SRDs[i][j];
//             VkDescriptorSetLayoutBinding binding{};
//             binding.binding         = srd.binding;
//             binding.descriptorType  = ShaderResourceTypeToVkDescriptorType(srd.type);
//             binding.descriptorCount = DecideDescriptorCount(
//                 binding.descriptorType, srd.arraySize, m_device->GetDescriptorIndexingProperties());
//             binding.stageFlags = ShaderStageFlagsBitsToVkShaderStageFlags(srd.stageFlags);
//             if (binding.descriptorCount > 1)
//             {
//                 bindingFlags.push_back(VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT);
//             }
//             else
//             {
//                 bindingFlags.push_back(0);
//             }
//             dsBindings[i].push_back(binding);
//             descriptorPoolKey.descriptorCount[ToUnderlying(srd.type)] += binding.descriptorCount;
//         }
//         if (dsBindings[i].empty())
//             continue;
//         // create descriptor set layout for set i
//         VkDescriptorSetLayoutCreateInfo layoutCI;
//         InitVkStruct(layoutCI, VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);
//         layoutCI.bindingCount = dsBindings[i].size();
//         layoutCI.pBindings    = dsBindings[i].data();
//         layoutCI.flags        = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
//         // set binding flags
//         VkDescriptorSetLayoutBindingFlagsCreateInfo bindingFlagsCreateInfo;
//         InitVkStruct(bindingFlagsCreateInfo,
//                      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO);
//         bindingFlagsCreateInfo.bindingCount  = bindingFlags.size();
//         bindingFlagsCreateInfo.pBindingFlags = bindingFlags.data();
//         layoutCI.pNext                       = &bindingFlagsCreateInfo;
//
//         VkDescriptorSetLayout descriptorSetLayout{VK_NULL_HANDLE};
//         VKCHECK(
//             vkCreateDescriptorSetLayout(GetVkDevice(), &layoutCI, nullptr, &descriptorSetLayout));
//         shader->descriptorSetLayouts.push_back(descriptorSetLayout);
//     }
//     // vertex input state
//     const auto vertexInputCount = sgInfo.vertexInputAttributes.size();
//     if (vertexInputCount > 0)
//     {
//         shader->m_vertexInputInfo.vkAttributes.resize(vertexInputCount);
//         // packed data for vertex inputs, only 1 binding
//         VkVertexInputBindingDescription vkBinding{};
//         vkBinding.binding   = 0;
//         vkBinding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
//         vkBinding.stride    = sgInfo.vertexBindingStride;
//         shader->m_vertexInputInfo.vkBindings.emplace_back(vkBinding);
//         // populate attributes
//         for (uint32_t i = 0; i < vertexInputCount; i++)
//         {
//             shader->m_vertexInputInfo.vkAttributes[i].binding =
//                 sgInfo.vertexInputAttributes[i].binding;
//             shader->m_vertexInputInfo.vkAttributes[i].location =
//                 sgInfo.vertexInputAttributes[i].location;
//             shader->m_vertexInputInfo.vkAttributes[i].offset = sgInfo.vertexInputAttributes[i].offset;
//             shader->m_vertexInputInfo.vkAttributes[i].format =
//                 static_cast<VkFormat>(sgInfo.vertexInputAttributes[i].format);
//         }
//         InitVkStruct(shader->m_vertexInputInfo.stateCI,
//                      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO);
//         shader->m_vertexInputInfo.stateCI.vertexAttributeDescriptionCount =
//             shader->m_vertexInputInfo.vkAttributes.size();
//         shader->m_vertexInputInfo.stateCI.pVertexAttributeDescriptions =
//             shader->m_vertexInputInfo.vkAttributes.data();
//         shader->m_vertexInputInfo.stateCI.vertexBindingDescriptionCount =
//             shader->m_vertexInputInfo.vkBindings.size();
//         shader->m_vertexInputInfo.stateCI.pVertexBindingDescriptions =
//             shader->m_vertexInputInfo.vkBindings.data();
//     }
//     else
//     {
//         InitVkStruct(shader->m_vertexInputInfo.stateCI,
//                      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO);
//         shader->m_vertexInputInfo.stateCI.vertexAttributeDescriptionCount = 0;
//         shader->m_vertexInputInfo.stateCI.pVertexAttributeDescriptions    = nullptr;
//         shader->m_vertexInputInfo.stateCI.vertexBindingDescriptionCount   = 0;
//         shader->m_vertexInputInfo.stateCI.pVertexBindingDescriptions      = nullptr;
//     }
//     // Create pipeline layout
//     VkPipelineLayoutCreateInfo pipelineLayoutCI;
//     InitVkStruct(pipelineLayoutCI, VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
//     pipelineLayoutCI.pSetLayouts    = shader->descriptorSetLayouts.data();
//     pipelineLayoutCI.setLayoutCount = setCount;
//     VkPushConstantRange prc{};
//     const auto& pc = sgInfo.pushConstants;
//     if (pc.size > 0)
//     {
//         prc.stageFlags = ShaderStageFlagsBitsToVkShaderStageFlags(pc.stageFlags);
//         prc.size       = pc.size;
//         pipelineLayoutCI.pushConstantRangeCount = 1;
//         pipelineLayoutCI.pPushConstantRanges    = &prc;
//     }
//     vkCreatePipelineLayout(GetVkDevice(), &pipelineLayoutCI, nullptr, &shader->pipelineLayout);
//
//     shader->descriptorPoolKey = descriptorPoolKey;
//     shader->pushConstantsStageFlags =
//         ShaderStageFlagsBitsToVkShaderStageFlags(sgInfo.pushConstants.stageFlags);
//
//     auto debugName = sgInfo.name + "_PipelineLayout";
//     m_device->SetObjectName(VK_OBJECT_TYPE_PIPELINE_LAYOUT,
//                             reinterpret_cast<uint64_t>(shader->pipelineLayout), debugName.c_str());
//
//     return ShaderHandle(shader);
// }

// void VulkanRHI::DestroyShader(ShaderHandle shaderHandle)
// {
//     VulkanShader* shader = TO_VK_SHADER(shaderHandle);
//
//     for (auto& dsLayout : shader->descriptorSetLayouts)
//     {
//         vkDestroyDescriptorSetLayout(GetVkDevice(), dsLayout, nullptr);
//     }
//
//     for (auto& stageCreateInfo : shader->m_stageCreateInfos)
//     {
//         vkDestroyShaderModule(GetVkDevice(), stageCreateInfo.module, nullptr);
//     }
//     if (shader->pipelineLayout != VK_NULL_HANDLE)
//     {
//         vkDestroyPipelineLayout(GetVkDevice(), shader->pipelineLayout, nullptr);
//     }
//     VersatileResource::Free(m_resourceAllocator, shader);
// }

// RHIPipeline* VulkanRHI::CreateGfxPipeline(RHIShader* shaderHandle,
//                                           const RHIGfxPipelineStates& states,
//                                           RenderPassHandle renderPassHandle,
//                                           uint32_t subpass)
// {
//     // Input Assembly
//     VkPipelineInputAssemblyStateCreateInfo IAStateCI;
//     InitVkStruct(IAStateCI, VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO);
//     IAStateCI.topology               = ToVkPrimitiveTopology(states.primitiveType);
//     IAStateCI.primitiveRestartEnable = VK_FALSE;
//
//     // Viewport State
//     VkPipelineViewportStateCreateInfo VPStateCI;
//     InitVkStruct(VPStateCI, VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO);
//     VPStateCI.scissorCount  = 1;
//     VPStateCI.viewportCount = 1;
//
//     // Rasterization State
//     const auto& rasterizationState = states.rasterizationState;
//     VkPipelineRasterizationStateCreateInfo rasterizationCI;
//     InitVkStruct(rasterizationCI, VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO);
//     rasterizationCI.cullMode                = ToVkCullModeFlags(rasterizationState.cullMode);
//     rasterizationCI.frontFace               = ToVkFrontFace(rasterizationState.frontFace);
//     rasterizationCI.depthClampEnable        = rasterizationState.enableDepthClamp;
//     rasterizationCI.rasterizerDiscardEnable = rasterizationState.discardPrimitives;
//     rasterizationCI.polygonMode =
//         rasterizationState.wireframe ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
//     rasterizationCI.depthBiasEnable         = rasterizationState.enableDepthBias;
//     rasterizationCI.depthBiasClamp          = rasterizationState.depthBiasClamp;
//     rasterizationCI.depthBiasConstantFactor = rasterizationState.depthBiasConstantFactor;
//     rasterizationCI.depthBiasSlopeFactor    = rasterizationState.depthBiasSlopeFactor;
//     rasterizationCI.lineWidth               = rasterizationState.lineWidth;
//
//     // Multisample state
//     const auto& multisampleState = states.multiSampleState;
//     VkPipelineMultisampleStateCreateInfo MSStateCI;
//     InitVkStruct(MSStateCI, VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO);
//     MSStateCI.rasterizationSamples  = ToVkSampleCountFlagBits(multisampleState.sampleCount);
//     MSStateCI.sampleShadingEnable   = multisampleState.enableSampleShading;
//     MSStateCI.minSampleShading      = multisampleState.minSampleShading;
//     MSStateCI.alphaToCoverageEnable = multisampleState.enableAlphaToCoverage;
//     MSStateCI.alphaToOneEnable      = multisampleState.enbaleAlphaToOne;
//     MSStateCI.pSampleMask =
//         multisampleState.sampleMasks.empty() ? nullptr : multisampleState.sampleMasks.data();
//
//     // Depth Stencil
//     const auto& depthStencilState = states.depthStencilState;
//     VkPipelineDepthStencilStateCreateInfo DSStateCI;
//     InitVkStruct(DSStateCI, VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO);
//     DSStateCI.depthTestEnable       = depthStencilState.enableDepthTest;
//     DSStateCI.depthWriteEnable      = depthStencilState.enableDepthWrite;
//     DSStateCI.depthCompareOp        = ToVkCompareOp(depthStencilState.depthCompareOp);
//     DSStateCI.depthBoundsTestEnable = depthStencilState.enableDepthBoundsTest;
//     DSStateCI.stencilTestEnable     = depthStencilState.enableStencilTest;
//
//     DSStateCI.front.failOp      = ToVkStencilOp(depthStencilState.frontOp.fail);
//     DSStateCI.front.passOp      = ToVkStencilOp(depthStencilState.frontOp.pass);
//     DSStateCI.front.depthFailOp = ToVkStencilOp(depthStencilState.frontOp.depthFail);
//     DSStateCI.front.compareOp   = ToVkCompareOp(depthStencilState.frontOp.compare);
//     DSStateCI.front.compareMask = depthStencilState.frontOp.compareMask;
//     DSStateCI.front.writeMask   = depthStencilState.frontOp.writeMask;
//     DSStateCI.front.reference   = depthStencilState.frontOp.reference;
//
//     DSStateCI.back.failOp      = ToVkStencilOp(depthStencilState.backOp.fail);
//     DSStateCI.back.passOp      = ToVkStencilOp(depthStencilState.backOp.pass);
//     DSStateCI.back.depthFailOp = ToVkStencilOp(depthStencilState.backOp.depthFail);
//     DSStateCI.back.compareOp   = ToVkCompareOp(depthStencilState.backOp.compare);
//     DSStateCI.back.compareMask = depthStencilState.backOp.compareMask;
//     DSStateCI.back.writeMask   = depthStencilState.backOp.writeMask;
//     DSStateCI.back.reference   = depthStencilState.backOp.reference;
//
//     DSStateCI.minDepthBounds = depthStencilState.minDepthBounds;
//     DSStateCI.maxDepthBounds = depthStencilState.maxDepthBounds;
//
//     // Color Blend State
//     const auto& colorBlendState = states.colorBlendState;
//     std::vector<VkPipelineColorBlendAttachmentState> vkCBAttStates;
//     vkCBAttStates.resize(colorBlendState.attachments.size());
//     for (uint32_t i = 0; i < colorBlendState.attachments.size(); i++)
//     {
//         vkCBAttStates[i].blendEnable = colorBlendState.attachments[i].enableBlend;
//
//         vkCBAttStates[i].srcColorBlendFactor =
//             ToVkBlendFactor(colorBlendState.attachments[i].srcAlphaBlendFactor);
//         vkCBAttStates[i].dstColorBlendFactor =
//             ToVkBlendFactor(colorBlendState.attachments[i].dstColorBlendFactor);
//         vkCBAttStates[i].colorBlendOp = ToVkBlendOp(colorBlendState.attachments[i].colorBlendOp);
//
//         vkCBAttStates[i].srcAlphaBlendFactor =
//             ToVkBlendFactor(colorBlendState.attachments[i].srcAlphaBlendFactor);
//         vkCBAttStates[i].dstAlphaBlendFactor =
//             ToVkBlendFactor(colorBlendState.attachments[i].dstAlphaBlendFactor);
//         vkCBAttStates[i].alphaBlendOp = ToVkBlendOp(colorBlendState.attachments[i].alphaBlendOp);
//
//         if (colorBlendState.attachments[i].writeR)
//         {
//             vkCBAttStates[i].colorWriteMask |= VK_COLOR_COMPONENT_R_BIT;
//         }
//         if (colorBlendState.attachments[i].writeG)
//         {
//             vkCBAttStates[i].colorWriteMask |= VK_COLOR_COMPONENT_G_BIT;
//         }
//         if (colorBlendState.attachments[i].writeB)
//         {
//             vkCBAttStates[i].colorWriteMask |= VK_COLOR_COMPONENT_B_BIT;
//         }
//         if (colorBlendState.attachments[i].writeA)
//         {
//             vkCBAttStates[i].colorWriteMask |= VK_COLOR_COMPONENT_A_BIT;
//         }
//     }
//     VkPipelineColorBlendStateCreateInfo CBStateCI;
//     InitVkStruct(CBStateCI, VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO);
//     CBStateCI.logicOpEnable     = colorBlendState.enableLogicOp;
//     CBStateCI.logicOp           = ToVkLogicOp(colorBlendState.logicOp);
//     CBStateCI.attachmentCount   = static_cast<uint32_t>(vkCBAttStates.size());
//     CBStateCI.pAttachments      = vkCBAttStates.empty() ? nullptr : vkCBAttStates.data();
//     CBStateCI.blendConstants[0] = colorBlendState.blendConstants.r;
//     CBStateCI.blendConstants[1] = colorBlendState.blendConstants.g;
//     CBStateCI.blendConstants[2] = colorBlendState.blendConstants.b;
//     CBStateCI.blendConstants[3] = colorBlendState.blendConstants.a;
//
//     // Dynamic States
//     VkPipelineDynamicStateCreateInfo dynamicStateCI;
//     InitVkStruct(dynamicStateCI, VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO);
//     std::vector<VkDynamicState> vkDynamicStates(states.dynamicStates.size());
//     for (uint32_t i = 0; i < states.dynamicStates.size(); i++)
//     {
//         vkDynamicStates[i] = ToVkDynamicState(states.dynamicStates[i]);
//     }
//     dynamicStateCI.dynamicStateCount = static_cast<uint32_t>(states.dynamicStates.size());
//     dynamicStateCI.pDynamicStates    = vkDynamicStates.empty() ? nullptr : vkDynamicStates.data();
//
//     VulkanShader* shader = TO_VK_SHADER(shaderHandle);
//
//     VkGraphicsPipelineCreateInfo pipelineCI;
//     InitVkStruct(pipelineCI, VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO);
//     pipelineCI.stageCount          = shader->GetNumShaderStages();
//     pipelineCI.pStages             = shader->GetStageCreateInfoData();
//     pipelineCI.pVertexInputState   = shader->GetVertexInputStateCreateInfoData();
//     pipelineCI.pInputAssemblyState = &IAStateCI;
//     pipelineCI.pViewportState      = &VPStateCI;
//     pipelineCI.pRasterizationState = &rasterizationCI;
//     pipelineCI.pMultisampleState   = &MSStateCI;
//     pipelineCI.pDepthStencilState  = &DSStateCI;
//     pipelineCI.pColorBlendState    = &CBStateCI;
//     pipelineCI.pDynamicState       = &dynamicStateCI;
//     pipelineCI.layout              = shader->GetVkPipelineLayout();
//     pipelineCI.renderPass          = TO_VK_RENDER_PASS(renderPassHandle);
//     pipelineCI.subpass             = subpass;
//
//     VkPipeline gfxPipeline{VK_NULL_HANDLE};
//     VKCHECK(
//         vkCreateGraphicsPipelines(GetVkDevice(), nullptr, 1, &pipelineCI, nullptr, &gfxPipeline));
//
//     VulkanPipeline* pipeline = VersatileResource::Alloc<VulkanPipeline>(m_resourceAllocator);
//     pipeline->pipeline       = gfxPipeline;
//     pipeline->pipelineLayout = shader->GetVkPipelineLayout();
//     // pipeline->descriptorSetCount = shader->GetNumDescriptorSetLayouts();
//     pipeline->pushConstantsStageFlags = shader->GetPushConstantsStageFlags();
//     //    pipeline->descriptorSets.resize(pipeline->descriptorSetCount);
//
//     // m_shaderPipelines[shaderHandle] = pipeline;
//
//     return RHIPipeline * (pipeline);
// }

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
    }
    else if (m_type == RHIPipelineType::eGraphics)
    {
        InitGraphics();
    }
}

void VulkanPipeline::Destroy()
{
    vkDestroyPipeline(GVulkanRHI->GetVkDevice(), m_vkPipeline, nullptr);
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
    const auto& rasterizationState = m_gfxStates.rasterizationState;
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
    const auto& multisampleState = m_gfxStates.multiSampleState;
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
    const auto& depthStencilState = m_gfxStates.depthStencilState;
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
    const auto& colorBlendState = m_gfxStates.colorBlendState;
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
    std::vector<VkDynamicState> vkDynamicStates(m_gfxStates.dynamicStates.size());
    for (uint32_t i = 0; i < m_gfxStates.dynamicStates.size(); i++)
    {
        vkDynamicStates[i] = ToVkDynamicState(m_gfxStates.dynamicStates[i]);
    }
    dynamicStateCI.dynamicStateCount = static_cast<uint32_t>(m_gfxStates.dynamicStates.size());
    dynamicStateCI.pDynamicStates    = vkDynamicStates.empty() ? nullptr : vkDynamicStates.data();

    VulkanShader* shader = TO_VK_SHADER(m_shader);

    VkGraphicsPipelineCreateInfo pipelineCI;
    InitVkStruct(pipelineCI, VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO);
    pipelineCI.stageCount          = shader->GetNumShaderStages();
    pipelineCI.pStages             = shader->GetStageCreateInfoData();
    pipelineCI.pVertexInputState   = shader->GetVertexInputStateCreateInfoData();
    pipelineCI.pInputAssemblyState = &IAStateCI;
    pipelineCI.pViewportState      = &VPStateCI;
    pipelineCI.pRasterizationState = &rasterizationCI;
    pipelineCI.pMultisampleState   = &MSStateCI;
    pipelineCI.pDepthStencilState  = &DSStateCI;
    pipelineCI.pColorBlendState    = &CBStateCI;
    pipelineCI.pDynamicState       = &dynamicStateCI;
    pipelineCI.layout              = shader->GetVkPipelineLayout();
    pipelineCI.subpass             = m_subpassIdx;

    std::vector<VkFormat> colorAttachmentFormats;
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

    m_pushConstantsStageFlags = shader->GetPushConstantsStageFlags();
}

void VulkanPipeline::InitCompute()
{
    VulkanShader* shader = TO_VK_SHADER(m_shader);

    VkComputePipelineCreateInfo pipelineCI{};
    pipelineCI.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineCI.stage  = shader->GetStageCreateInfoData()[0];
    pipelineCI.layout = shader->GetVkPipelineLayout();

    VkPipeline computePipeline{VK_NULL_HANDLE};
    VKCHECK(vkCreateComputePipelines(GVulkanRHI->GetVkDevice(), nullptr, 1, &pipelineCI, nullptr,
                                     &m_vkPipeline));
    m_pushConstantsStageFlags = shader->GetPushConstantsStageFlags();
}

// RHIPipeline* VulkanRHI::CreateGfxPipeline(RHIShader* shaderHandle,
//                                           const RHIGfxPipelineStates& states,
//                                           const RHIRenderPassLayout& renderPassLayout,
//                                           uint32_t subpass)
//
// {
//     // Input Assembly
//     VkPipelineInputAssemblyStateCreateInfo IAStateCI;
//     InitVkStruct(IAStateCI, VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO);
//     IAStateCI.topology               = ToVkPrimitiveTopology(states.primitiveType);
//     IAStateCI.primitiveRestartEnable = VK_FALSE;
//
//     // Viewport State
//     VkPipelineViewportStateCreateInfo VPStateCI;
//     InitVkStruct(VPStateCI, VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO);
//     VPStateCI.scissorCount  = 1;
//     VPStateCI.viewportCount = 1;
//
//     // Rasterization State
//     const auto& rasterizationState = states.rasterizationState;
//     VkPipelineRasterizationStateCreateInfo rasterizationCI;
//     InitVkStruct(rasterizationCI, VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO);
//     rasterizationCI.cullMode                = ToVkCullModeFlags(rasterizationState.cullMode);
//     rasterizationCI.frontFace               = ToVkFrontFace(rasterizationState.frontFace);
//     rasterizationCI.depthClampEnable        = rasterizationState.enableDepthClamp;
//     rasterizationCI.rasterizerDiscardEnable = rasterizationState.discardPrimitives;
//     rasterizationCI.polygonMode =
//         rasterizationState.wireframe ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
//     rasterizationCI.depthBiasEnable         = rasterizationState.enableDepthBias;
//     rasterizationCI.depthBiasClamp          = rasterizationState.depthBiasClamp;
//     rasterizationCI.depthBiasConstantFactor = rasterizationState.depthBiasConstantFactor;
//     rasterizationCI.depthBiasSlopeFactor    = rasterizationState.depthBiasSlopeFactor;
//     rasterizationCI.lineWidth               = rasterizationState.lineWidth;
//
//     // Multisample state
//     const auto& multisampleState = states.multiSampleState;
//     VkPipelineMultisampleStateCreateInfo MSStateCI;
//     InitVkStruct(MSStateCI, VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO);
//     MSStateCI.rasterizationSamples  = ToVkSampleCountFlagBits(multisampleState.sampleCount);
//     MSStateCI.sampleShadingEnable   = multisampleState.enableSampleShading;
//     MSStateCI.minSampleShading      = multisampleState.minSampleShading;
//     MSStateCI.alphaToCoverageEnable = multisampleState.enableAlphaToCoverage;
//     MSStateCI.alphaToOneEnable      = multisampleState.enbaleAlphaToOne;
//     MSStateCI.pSampleMask =
//         multisampleState.sampleMasks.empty() ? nullptr : multisampleState.sampleMasks.data();
//
//     // Depth Stencil
//     const auto& depthStencilState = states.depthStencilState;
//     VkPipelineDepthStencilStateCreateInfo DSStateCI;
//     InitVkStruct(DSStateCI, VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO);
//     DSStateCI.depthTestEnable       = depthStencilState.enableDepthTest;
//     DSStateCI.depthWriteEnable      = depthStencilState.enableDepthWrite;
//     DSStateCI.depthCompareOp        = ToVkCompareOp(depthStencilState.depthCompareOp);
//     DSStateCI.depthBoundsTestEnable = depthStencilState.enableDepthBoundsTest;
//     DSStateCI.stencilTestEnable     = depthStencilState.enableStencilTest;
//
//     DSStateCI.front.failOp      = ToVkStencilOp(depthStencilState.frontOp.fail);
//     DSStateCI.front.passOp      = ToVkStencilOp(depthStencilState.frontOp.pass);
//     DSStateCI.front.depthFailOp = ToVkStencilOp(depthStencilState.frontOp.depthFail);
//     DSStateCI.front.compareOp   = ToVkCompareOp(depthStencilState.frontOp.compare);
//     DSStateCI.front.compareMask = depthStencilState.frontOp.compareMask;
//     DSStateCI.front.writeMask   = depthStencilState.frontOp.writeMask;
//     DSStateCI.front.reference   = depthStencilState.frontOp.reference;
//
//     DSStateCI.back.failOp      = ToVkStencilOp(depthStencilState.backOp.fail);
//     DSStateCI.back.passOp      = ToVkStencilOp(depthStencilState.backOp.pass);
//     DSStateCI.back.depthFailOp = ToVkStencilOp(depthStencilState.backOp.depthFail);
//     DSStateCI.back.compareOp   = ToVkCompareOp(depthStencilState.backOp.compare);
//     DSStateCI.back.compareMask = depthStencilState.backOp.compareMask;
//     DSStateCI.back.writeMask   = depthStencilState.backOp.writeMask;
//     DSStateCI.back.reference   = depthStencilState.backOp.reference;
//
//     DSStateCI.minDepthBounds = depthStencilState.minDepthBounds;
//     DSStateCI.maxDepthBounds = depthStencilState.maxDepthBounds;
//
//     // Color Blend State
//     const auto& colorBlendState = states.colorBlendState;
//     std::vector<VkPipelineColorBlendAttachmentState> vkCBAttStates;
//     vkCBAttStates.resize(colorBlendState.attachments.size());
//     for (uint32_t i = 0; i < colorBlendState.attachments.size(); i++)
//     {
//         vkCBAttStates[i].blendEnable = colorBlendState.attachments[i].enableBlend;
//
//         vkCBAttStates[i].srcColorBlendFactor =
//             ToVkBlendFactor(colorBlendState.attachments[i].srcAlphaBlendFactor);
//         vkCBAttStates[i].dstColorBlendFactor =
//             ToVkBlendFactor(colorBlendState.attachments[i].dstColorBlendFactor);
//         vkCBAttStates[i].colorBlendOp = ToVkBlendOp(colorBlendState.attachments[i].colorBlendOp);
//
//         vkCBAttStates[i].srcAlphaBlendFactor =
//             ToVkBlendFactor(colorBlendState.attachments[i].srcAlphaBlendFactor);
//         vkCBAttStates[i].dstAlphaBlendFactor =
//             ToVkBlendFactor(colorBlendState.attachments[i].dstAlphaBlendFactor);
//         vkCBAttStates[i].alphaBlendOp = ToVkBlendOp(colorBlendState.attachments[i].alphaBlendOp);
//
//         if (colorBlendState.attachments[i].writeR)
//         {
//             vkCBAttStates[i].colorWriteMask |= VK_COLOR_COMPONENT_R_BIT;
//         }
//         if (colorBlendState.attachments[i].writeG)
//         {
//             vkCBAttStates[i].colorWriteMask |= VK_COLOR_COMPONENT_G_BIT;
//         }
//         if (colorBlendState.attachments[i].writeB)
//         {
//             vkCBAttStates[i].colorWriteMask |= VK_COLOR_COMPONENT_B_BIT;
//         }
//         if (colorBlendState.attachments[i].writeA)
//         {
//             vkCBAttStates[i].colorWriteMask |= VK_COLOR_COMPONENT_A_BIT;
//         }
//     }
//     VkPipelineColorBlendStateCreateInfo CBStateCI;
//     InitVkStruct(CBStateCI, VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO);
//     CBStateCI.logicOpEnable     = colorBlendState.enableLogicOp;
//     CBStateCI.logicOp           = ToVkLogicOp(colorBlendState.logicOp);
//     CBStateCI.attachmentCount   = static_cast<uint32_t>(vkCBAttStates.size());
//     CBStateCI.pAttachments      = vkCBAttStates.empty() ? nullptr : vkCBAttStates.data();
//     CBStateCI.blendConstants[0] = colorBlendState.blendConstants.r;
//     CBStateCI.blendConstants[1] = colorBlendState.blendConstants.g;
//     CBStateCI.blendConstants[2] = colorBlendState.blendConstants.b;
//     CBStateCI.blendConstants[3] = colorBlendState.blendConstants.a;
//
//     // Dynamic States
//     VkPipelineDynamicStateCreateInfo dynamicStateCI;
//     InitVkStruct(dynamicStateCI, VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO);
//     std::vector<VkDynamicState> vkDynamicStates(states.dynamicStates.size());
//     for (uint32_t i = 0; i < states.dynamicStates.size(); i++)
//     {
//         vkDynamicStates[i] = ToVkDynamicState(states.dynamicStates[i]);
//     }
//     dynamicStateCI.dynamicStateCount = static_cast<uint32_t>(states.dynamicStates.size());
//     dynamicStateCI.pDynamicStates    = vkDynamicStates.empty() ? nullptr : vkDynamicStates.data();
//
//     VulkanShader* shader = TO_VK_SHADER(shaderHandle);
//
//     VkGraphicsPipelineCreateInfo pipelineCI;
//     InitVkStruct(pipelineCI, VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO);
//     pipelineCI.stageCount          = shader->GetNumShaderStages();
//     pipelineCI.pStages             = shader->GetStageCreateInfoData();
//     pipelineCI.pVertexInputState   = shader->GetVertexInputStateCreateInfoData();
//     pipelineCI.pInputAssemblyState = &IAStateCI;
//     pipelineCI.pViewportState      = &VPStateCI;
//     pipelineCI.pRasterizationState = &rasterizationCI;
//     pipelineCI.pMultisampleState   = &MSStateCI;
//     pipelineCI.pDepthStencilState  = &DSStateCI;
//     pipelineCI.pColorBlendState    = &CBStateCI;
//     pipelineCI.pDynamicState       = &dynamicStateCI;
//     pipelineCI.layout              = shader->GetVkPipelineLayout();
//     pipelineCI.renderPass          = VK_NULL_HANDLE;
//     pipelineCI.subpass             = subpass;
//
//     std::vector<VkFormat> colorAttachmentFormats;
//     colorAttachmentFormats.reserve(renderPassLayout.GetNumColorRenderTargets());
//     for (uint32_t i = 0; i < renderPassLayout.GetNumColorRenderTargets(); i++)
//     {
//         VkFormat colorFormat = ToVkFormat(renderPassLayout.GetColorRenderTargets()[i].format);
//         colorAttachmentFormats.emplace_back(colorFormat);
//     }
//     VkPipelineRenderingCreateInfoKHR renderingCI;
//     InitVkStruct(renderingCI, VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR);
//     renderingCI.colorAttachmentCount    = static_cast<uint32_t>(colorAttachmentFormats.size());
//     renderingCI.pColorAttachmentFormats = colorAttachmentFormats.data();
//
//     if (renderPassLayout.HasDepthStencilRenderTarget())
//     {
//         VkFormat depthFormat = ToVkFormat(renderPassLayout.GetDepthStencilRenderTarget().format);
//         renderingCI.depthAttachmentFormat   = depthFormat;
//         renderingCI.stencilAttachmentFormat = depthFormat;
//     }
//     pipelineCI.pNext = &renderingCI;
//
//     VkPipeline gfxPipeline{VK_NULL_HANDLE};
//     VKCHECK(
//         vkCreateGraphicsPipelines(GetVkDevice(), nullptr, 1, &pipelineCI, nullptr, &gfxPipeline));
//
//     VulkanPipeline* pipeline = VersatileResource::Alloc<VulkanPipeline>(m_resourceAllocator);
//     pipeline->pipeline       = gfxPipeline;
//     pipeline->pipelineLayout = shader->GetVkPipelineLayout();
//     // pipeline->descriptorSetCount = shader->GetNumDescriptorSetLayouts();
//     pipeline->pushConstantsStageFlags = shader->GetPushConstantsStageFlags();
//     //    pipeline->descriptorSets.resize(pipeline->descriptorSetCount);
//
//     // m_shaderPipelines[shaderHandle] = pipeline;
//
//     return RHIPipeline * (pipeline);
// }

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
    VkDescriptorPoolSize poolSizes[ToUnderlying(RHIShaderResourceType::eMax)];
    uint32_t poolSizeCount = 0;
    {
        const auto MaxDescriptorPerPool = RHIOptions::GetInstance().MaxDescriptorSetPerPool();
        auto* currPoolSize              = poolSizes;
        if (poolKey.descriptorCount[ToUnderlying(RHIShaderResourceType::eSampler)])
        {
            *currPoolSize      = {};
            currPoolSize->type = VK_DESCRIPTOR_TYPE_SAMPLER;
            currPoolSize->descriptorCount =
                poolKey.descriptorCount[ToUnderlying(RHIShaderResourceType::eSampler)] *
                MaxDescriptorPerPool;
            currPoolSize++;
            poolSizeCount++;
        }
        if (poolKey.descriptorCount[ToUnderlying(RHIShaderResourceType::eTexture)])
        {
            *currPoolSize      = {};
            currPoolSize->type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            currPoolSize->descriptorCount =
                poolKey.descriptorCount[ToUnderlying(RHIShaderResourceType::eTexture)] *
                MaxDescriptorPerPool;
            currPoolSize++;
            poolSizeCount++;
        }
        if (poolKey.descriptorCount[ToUnderlying(RHIShaderResourceType::eSamplerWithTexture)])
        {
            *currPoolSize      = {};
            currPoolSize->type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            currPoolSize->descriptorCount =
                poolKey.descriptorCount[ToUnderlying(RHIShaderResourceType::eSamplerWithTexture)] *
                MaxDescriptorPerPool;
            currPoolSize++;
            poolSizeCount++;
        }
        if (poolKey.descriptorCount[ToUnderlying(RHIShaderResourceType::eImage)])
        {
            *currPoolSize      = {};
            currPoolSize->type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            currPoolSize->descriptorCount =
                poolKey.descriptorCount[ToUnderlying(RHIShaderResourceType::eImage)] *
                MaxDescriptorPerPool;
            currPoolSize++;
            poolSizeCount++;
        }
        if (poolKey.descriptorCount[ToUnderlying(RHIShaderResourceType::eTextureBuffer)] ||
            poolKey.descriptorCount[ToUnderlying(RHIShaderResourceType::eSamplerWithTextureBuffer)])
        {
            *currPoolSize      = {};
            currPoolSize->type = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
            currPoolSize->descriptorCount =
                (poolKey.descriptorCount[ToUnderlying(RHIShaderResourceType::eTextureBuffer)] +
                 poolKey.descriptorCount[ToUnderlying(
                     RHIShaderResourceType::eSamplerWithTextureBuffer)]) *
                MaxDescriptorPerPool;
            currPoolSize++;
            poolSizeCount++;
        }
        if (poolKey.descriptorCount[ToUnderlying(RHIShaderResourceType::eImageBuffer)])
        {
            *currPoolSize      = {};
            currPoolSize->type = VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
            currPoolSize->descriptorCount =
                poolKey.descriptorCount[ToUnderlying(RHIShaderResourceType::eImageBuffer)] *
                MaxDescriptorPerPool;
            currPoolSize++;
            poolSizeCount++;
        }
        if (poolKey.descriptorCount[ToUnderlying(RHIShaderResourceType::eUniformBuffer)])
        {
            *currPoolSize      = {};
            currPoolSize->type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            currPoolSize->descriptorCount =
                poolKey.descriptorCount[ToUnderlying(RHIShaderResourceType::eUniformBuffer)] *
                MaxDescriptorPerPool;
            currPoolSize++;
            poolSizeCount++;
        }
        if (poolKey.descriptorCount[ToUnderlying(RHIShaderResourceType::eStorageBuffer)])
        {
            *currPoolSize      = {};
            currPoolSize->type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            currPoolSize->descriptorCount =
                poolKey.descriptorCount[ToUnderlying(RHIShaderResourceType::eStorageBuffer)] *
                MaxDescriptorPerPool;
            currPoolSize++;
            poolSizeCount++;
        }
        if (poolKey.descriptorCount[ToUnderlying(RHIShaderResourceType::eInputAttachment)])
        {
            *currPoolSize      = {};
            currPoolSize->type = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
            currPoolSize->descriptorCount =
                poolKey.descriptorCount[ToUnderlying(RHIShaderResourceType::eInputAttachment)] *
                MaxDescriptorPerPool;
            currPoolSize++;
            poolSizeCount++;
        }
        VERIFY_EXPR(poolSizeCount <= ToUnderlying(RHIShaderResourceType::eMax));
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

void VulkanDescriptorSet::Update(const std::vector<RHIShaderResourceBinding>& resourceBindings)
{
    std::vector<VkWriteDescriptorSet> writes;
    writes.resize(resourceBindings.size());

    for (uint32_t i = 0; i < resourceBindings.size(); i++)
    {
        const auto& srb = resourceBindings[i];
        InitVkStruct(writes[i], VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
        writes[i].dstBinding = srb.binding;

        uint32_t numDescriptors = 1;

        switch (srb.type)
        {
            case RHIShaderResourceType::eSampler:
            {
                numDescriptors = srb.resources.size();
                VkDescriptorImageInfo* imageInfos =
                    ALLOCA_ARRAY(VkDescriptorImageInfo, numDescriptors);
                for (uint32_t j = 0; j < numDescriptors; j++)
                {
                    VkDescriptorImageInfo imageInfo{};
                    imageInfo.sampler     = TO_VK_SAMPLER(srb.resources[j])->GetVkSampler();
                    imageInfo.imageView   = VK_NULL_HANDLE;
                    imageInfo.imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                    imageInfos[j]         = imageInfo;
                }
                writes[i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
                writes[i].pImageInfo     = imageInfos;
            }
            break;
            case RHIShaderResourceType::eTexture:
            {
                numDescriptors = srb.resources.size();
                VkDescriptorImageInfo* imageInfos =
                    ALLOCA_ARRAY(VkDescriptorImageInfo, numDescriptors);
                for (uint32_t j = 0; j < numDescriptors; j++)
                {
                    VkDescriptorImageInfo imageInfo{};
                    imageInfo.sampler     = VK_NULL_HANDLE;
                    imageInfo.imageView   = TO_VK_TEXTURE(srb.resources[j])->GetVkImageView();
                    imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                    imageInfos[j]         = imageInfo;
                }
                writes[i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
                writes[i].pImageInfo     = imageInfos;
            }
            break;
            case RHIShaderResourceType::eSamplerWithTexture:
            {
                VERIFY_EXPR(srb.resources.size() % 2 == 0 && srb.resources.size() >= 2);
                numDescriptors = srb.resources.size() / 2;
                VkDescriptorImageInfo* imageInfos =
                    ALLOCA_ARRAY(VkDescriptorImageInfo, numDescriptors);
                for (uint32_t j = 0; j < numDescriptors; j++)
                {
                    VkDescriptorImageInfo imageInfo{};
                    imageInfo.sampler   = TO_VK_SAMPLER(srb.resources[j * 2 + 0])->GetVkSampler();
                    imageInfo.imageView = TO_VK_TEXTURE(srb.resources[j * 2 + 1])->GetVkImageView();
                    imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                    imageInfos[j]         = imageInfo;
                }
                writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                writes[i].pImageInfo     = imageInfos;
            }
            break;
            case RHIShaderResourceType::eImage:
            {
                numDescriptors = srb.resources.size();
                VkDescriptorImageInfo* imageInfos =
                    ALLOCA_ARRAY(VkDescriptorImageInfo, numDescriptors);
                for (uint32_t j = 0; j < numDescriptors; j++)
                {
                    VkDescriptorImageInfo imageInfo{};
                    imageInfo.sampler     = VK_NULL_HANDLE;
                    imageInfo.imageView   = TO_VK_TEXTURE(srb.resources[j])->GetVkImageView();
                    imageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
                    imageInfos[j]         = imageInfo;
                }
                writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                writes[i].pImageInfo     = imageInfos;
            }
            break;
            case RHIShaderResourceType::eTextureBuffer:
            {
                numDescriptors = srb.resources.size();
                VkDescriptorBufferInfo* bufferInfos =
                    ALLOCA_ARRAY(VkDescriptorBufferInfo, numDescriptors);
                VkBufferView* bufferViews = ALLOCA_ARRAY(VkBufferView, numDescriptors);
                for (uint32_t j = 0; j < numDescriptors; j++)
                {
                    VulkanBuffer* vulkanBuffer = TO_VK_BUFFER(srb.resources[j]);
                    VkDescriptorBufferInfo bufferInfo{};
                    bufferInfo.buffer = vulkanBuffer->GetVkBuffer();
                    bufferInfo.range  = vulkanBuffer->GetRequiredSize();
                    bufferViews[j]    = vulkanBuffer->GetVkBufferView();
                    bufferInfos[j]    = bufferInfo;
                }
                writes[i].descriptorType   = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
                writes[i].pBufferInfo      = bufferInfos;
                writes[i].pTexelBufferView = bufferViews;
            }
            break;
            case RHIShaderResourceType::eSamplerWithTextureBuffer:
            {
                VERIFY_EXPR(srb.resources.size() % 2 == 0 && srb.resources.size() >= 2);
                numDescriptors = srb.resources.size() / 2;
                VkDescriptorImageInfo* imageInfos =
                    ALLOCA_ARRAY(VkDescriptorImageInfo, numDescriptors);
                VkDescriptorBufferInfo* bufferInfos =
                    ALLOCA_ARRAY(VkDescriptorBufferInfo, numDescriptors);
                VkBufferView* bufferViews = ALLOCA_ARRAY(VkBufferView, numDescriptors);
                for (uint32_t j = 0; j < numDescriptors; j++)
                {
                    VkDescriptorImageInfo imageInfo{};
                    imageInfo.sampler     = TO_VK_SAMPLER(srb.resources[j * 2 + 0])->GetVkSampler();
                    imageInfo.imageView   = VK_NULL_HANDLE;
                    imageInfo.imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                    imageInfos[j]         = imageInfo;

                    VulkanBuffer* vulkanBuffer = TO_VK_BUFFER(srb.resources[j * 2 + 1]);
                    VkDescriptorBufferInfo bufferInfo{};
                    bufferInfo.buffer = vulkanBuffer->GetVkBuffer();
                    bufferInfo.range  = vulkanBuffer->GetRequiredSize();
                    bufferViews[j]    = vulkanBuffer->GetVkBufferView();
                    bufferInfos[j]    = bufferInfo;
                }
                writes[i].descriptorType   = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
                writes[i].pImageInfo       = imageInfos;
                writes[i].pBufferInfo      = bufferInfos;
                writes[i].pTexelBufferView = bufferViews;
            }
            break;
            case RHIShaderResourceType::eImageBuffer:
            {
                numDescriptors = srb.resources.size();
                VkDescriptorBufferInfo* bufferInfos =
                    ALLOCA_ARRAY(VkDescriptorBufferInfo, numDescriptors);
                VkBufferView* bufferViews = ALLOCA_ARRAY(VkBufferView, numDescriptors);
                for (uint32_t j = 0; j < numDescriptors; j++)
                {
                    VulkanBuffer* vulkanBuffer = TO_VK_BUFFER(srb.resources[j]);
                    VkDescriptorBufferInfo bufferInfo{};
                    bufferInfo.buffer = vulkanBuffer->GetVkBuffer();
                    bufferInfo.range  = vulkanBuffer->GetRequiredSize();
                    bufferViews[j]    = vulkanBuffer->GetVkBufferView();
                    bufferInfos[j]    = bufferInfo;
                }
                writes[i].descriptorType   = VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
                writes[i].pBufferInfo      = bufferInfos;
                writes[i].pTexelBufferView = bufferViews;
            }
            break;
            case RHIShaderResourceType::eUniformBuffer:
            {
                VulkanBuffer* vulkanBuffer         = TO_VK_BUFFER(srb.resources[0]);
                VkDescriptorBufferInfo* bufferInfo = ALLOCA_SINGLE(VkDescriptorBufferInfo);

                *bufferInfo        = {};
                bufferInfo->buffer = vulkanBuffer->GetVkBuffer();
                bufferInfo->range  = vulkanBuffer->GetRequiredSize();

                writes[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                writes[i].pBufferInfo    = bufferInfo;
            }
            break;
            case RHIShaderResourceType::eStorageBuffer:
            {
                VulkanBuffer* vulkanBuffer         = TO_VK_BUFFER(srb.resources[0]);
                VkDescriptorBufferInfo* bufferInfo = ALLOCA_SINGLE(VkDescriptorBufferInfo);

                *bufferInfo        = {};
                bufferInfo->buffer = vulkanBuffer->GetVkBuffer();
                bufferInfo->range  = vulkanBuffer->GetRequiredSize();

                writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                writes[i].pBufferInfo    = bufferInfo;
            }
            break;
            case RHIShaderResourceType::eInputAttachment:
            {
                numDescriptors = srb.resources.size();
                VkDescriptorImageInfo* imageInfos =
                    ALLOCA_ARRAY(VkDescriptorImageInfo, numDescriptors);
                for (uint32_t j = 0; j < numDescriptors; j++)
                {
                    VkDescriptorImageInfo imageInfo{};
                    imageInfo.sampler     = VK_NULL_HANDLE;
                    imageInfo.imageView   = TO_VK_TEXTURE(srb.resources[j])->GetVkImageView();
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
        write.dstSet = m_vkDescriptorSet;
    }
    vkUpdateDescriptorSets(GVulkanRHI->GetVkDevice(), writes.size(), writes.data(), 0, nullptr);
}

VulkanDescriptorSet::VulkanDescriptorSet(const RHIShader* pShader, uint32_t setIndex) :
    RHIDescriptorSet(pShader, setIndex)
{}

void VulkanDescriptorSet::Init()
{
    const VulkanShader* shader            = TO_CVK_SHADER(m_pShader);
    VulkanDescriptorPoolManager* poolMngr = GVulkanRHI->GetDescriptorPoolManager();
    VulkanDescriptorPoolsIt iter{};
    VkDescriptorPool pool =
        poolMngr->GetOrCreateDescriptorPool(shader->GetDescriptorPoolKey(), &iter);
    VERIFY_EXPR(pool != VK_NULL_HANDLE);

    VkDescriptorSetAllocateInfo descriptorSetAllocateInfo{};
    InitVkStruct(descriptorSetAllocateInfo, VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO);
    descriptorSetAllocateInfo.descriptorPool     = pool;
    descriptorSetAllocateInfo.descriptorSetCount = 1;
    descriptorSetAllocateInfo.pSetLayouts = &shader->GetDescriptorSetLayoutData()[m_setIndex];

    VkDescriptorSet vkDescriptorSet{VK_NULL_HANDLE};
    VkResult result = vkAllocateDescriptorSets(GVulkanRHI->GetVkDevice(),
                                               &descriptorSetAllocateInfo, &vkDescriptorSet);
    if (result != VK_SUCCESS)
    {
        poolMngr->UnRefDescriptorPool(iter, pool);
        LOGE("Failed to allocate descriptor set");
    }

    m_poolIter         = iter;
    m_vkDescriptorPool = pool;
    m_vkDescriptorSet  = vkDescriptorSet;
}

void VulkanDescriptorSet::Destroy()
{
    vkFreeDescriptorSets(GVulkanRHI->GetVkDevice(), m_vkDescriptorPool, 1, &m_vkDescriptorSet);

    GVulkanRHI->GetDescriptorPoolManager()->UnRefDescriptorPool(m_poolIter, m_vkDescriptorPool);
    VersatileResource::Free(GVulkanRHI->GetResourceAllocator(), this);
}

// DescriptorSetHandle VulkanRHI::CreateDescriptorSet(RHIShader* shaderHandle, uint32_t setIndex)
// {
//     // if (!m_shaderPipelines.contains(shaderHandle))
//     // {
//     //     LOG_FATAL_ERROR("Pipeline should be created before allocating descriptorSets");
//     // }
//
//     VulkanShader* shader = TO_VK_SHADER(shaderHandle);
//     VulkanDescriptorPoolsIt iter{};
//     VkDescriptorPool pool =
//         m_descriptorPoolManager->GetOrCreateDescriptorPool(shader->GetDescriptorPoolKey(), &iter);
//     VERIFY_EXPR(pool != VK_NULL_HANDLE);
//
//     VkDescriptorSetAllocateInfo descriptorSetAllocateInfo{};
//     InitVkStruct(descriptorSetAllocateInfo, VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO);
//     descriptorSetAllocateInfo.descriptorPool     = pool;
//     descriptorSetAllocateInfo.descriptorSetCount = 1;
//     descriptorSetAllocateInfo.pSetLayouts        = &shader->GetDescriptorSetLayoutData()[setIndex];
//
//     VkDescriptorSet vkDescriptorSet{VK_NULL_HANDLE};
//     VkResult result = vkAllocateDescriptorSets(m_device->GetVkHandle(), &descriptorSetAllocateInfo,
//                                                &vkDescriptorSet);
//     if (result != VK_SUCCESS)
//     {
//         m_descriptorPoolManager->UnRefDescriptorPool(iter, pool);
//         LOGE("Failed to allocate descriptor set");
//     }
//
//     VulkanDescriptorSet* descriptorSet =
//         VersatileResource::Alloc<VulkanDescriptorSet>(m_resourceAllocator);
//     descriptorSet->iter           = iter;
//     descriptorSet->descriptorPool = pool;
//     descriptorSet->descriptorSet  = vkDescriptorSet;
//
//     // VulkanPipeline* vulkanPipeline           = TO_VK_PIPELINE(pipeline);
//     // vulkanPipeline->descriptorSets[setIndex] = descriptorSet;
//
//     // m_shaderPipelines[shaderHandle]->descriptorSets[setIndex] = descriptorSet;
//
//     return DescriptorSetHandle(descriptorSet);
// }
//
void VulkanRHI::DestroyDescriptorSet(RHIDescriptorSet* pDescriptorSet)
{
    pDescriptorSet->ReleaseReference();
}
//
// void VulkanRHI::UpdateDescriptorSet(DescriptorSetHandle descriptorSetHandle,
//                                     const std::vector<RHIShaderResourceBinding>& resourceBindings)
// {
//     std::vector<VkWriteDescriptorSet> writes;
//     writes.resize(resourceBindings.size());
//     // std::vector<VkDescriptorImageInfo> imageInfos;
//     // std::vector<VkDescriptorBufferInfo> bufferInfos;
//     // std::vector<VkBufferView> bufferViews;
//
//     for (uint32_t i = 0; i < resourceBindings.size(); i++)
//     {
//         const auto& srb = resourceBindings[i];
//         InitVkStruct(writes[i], VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
//         writes[i].dstBinding = srb.binding;
//
//         uint32_t numDescriptors = 1;
//         // VkDescriptorBufferInfo* currBufferInfos = bufferInfos.data() + bufferInfos.size();
//         // VkDescriptorImageInfo* currImageInfos   = imageInfos.data() + imageInfos.size();
//         // VkBufferView* currBufferViews           = bufferViews.data() + bufferViews.size();
//
//         switch (srb.type)
//         {
//             case RHIShaderResourceType::eSampler:
//             {
//                 numDescriptors = srb.resources.size();
//                 VkDescriptorImageInfo* imageInfos =
//                     ALLOCA_ARRAY(VkDescriptorImageInfo, numDescriptors);
//                 for (uint32_t j = 0; j < numDescriptors; j++)
//                 {
//                     VkDescriptorImageInfo imageInfo{};
//                     imageInfo.sampler     = TO_VK_SAMPLER(srb.resources[j])->GetVkSampler();
//                     imageInfo.imageView   = VK_NULL_HANDLE;
//                     imageInfo.imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
//                     imageInfos[j]         = imageInfo;
//                 }
//                 writes[i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
//                 writes[i].pImageInfo     = imageInfos;
//             }
//             break;
//             case RHIShaderResourceType::eTexture:
//             {
//                 numDescriptors = srb.resources.size();
//                 VkDescriptorImageInfo* imageInfos =
//                     ALLOCA_ARRAY(VkDescriptorImageInfo, numDescriptors);
//                 for (uint32_t j = 0; j < numDescriptors; j++)
//                 {
//                     VkDescriptorImageInfo imageInfo{};
//                     imageInfo.sampler     = VK_NULL_HANDLE;
//                     imageInfo.imageView   = TO_VK_TEXTURE(srb.resources[j])->GetVkImageView();
//                     imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
//                     imageInfos[j]         = imageInfo;
//                 }
//                 writes[i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
//                 writes[i].pImageInfo     = imageInfos;
//             }
//             break;
//             case RHIShaderResourceType::eSamplerWithTexture:
//             {
//                 VERIFY_EXPR(srb.resources.size() % 2 == 0 && srb.resources.size() >= 2);
//                 numDescriptors = srb.resources.size() / 2;
//                 VkDescriptorImageInfo* imageInfos =
//                     ALLOCA_ARRAY(VkDescriptorImageInfo, numDescriptors);
//                 for (uint32_t j = 0; j < numDescriptors; j++)
//                 {
//                     VkDescriptorImageInfo imageInfo{};
//                     imageInfo.sampler   = TO_VK_SAMPLER(srb.resources[j * 2 + 0])->GetVkSampler();
//                     imageInfo.imageView = TO_VK_TEXTURE(srb.resources[j * 2 + 1])->GetVkImageView();
//                     imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
//                     imageInfos[j]         = imageInfo;
//                 }
//                 writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
//                 writes[i].pImageInfo     = imageInfos;
//             }
//             break;
//             case RHIShaderResourceType::eImage:
//             {
//                 numDescriptors = srb.resources.size();
//                 VkDescriptorImageInfo* imageInfos =
//                     ALLOCA_ARRAY(VkDescriptorImageInfo, numDescriptors);
//                 for (uint32_t j = 0; j < numDescriptors; j++)
//                 {
//                     VkDescriptorImageInfo imageInfo{};
//                     imageInfo.sampler     = VK_NULL_HANDLE;
//                     imageInfo.imageView   = TO_VK_TEXTURE(srb.resources[j])->GetVkImageView();
//                     imageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
//                     imageInfos[j]         = imageInfo;
//                 }
//                 writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
//                 writes[i].pImageInfo     = imageInfos;
//             }
//             break;
//             case RHIShaderResourceType::eTextureBuffer:
//             {
//                 numDescriptors = srb.resources.size();
//                 VkDescriptorBufferInfo* bufferInfos =
//                     ALLOCA_ARRAY(VkDescriptorBufferInfo, numDescriptors);
//                 VkBufferView* bufferViews = ALLOCA_ARRAY(VkBufferView, numDescriptors);
//                 for (uint32_t j = 0; j < numDescriptors; j++)
//                 {
//                     VulkanBuffer* vulkanBuffer = TO_VK_BUFFER(srb.resources[j]);
//                     VkDescriptorBufferInfo bufferInfo{};
//                     bufferInfo.buffer = vulkanBuffer->GetVkBuffer();
//                     bufferInfo.range  = vulkanBuffer->GetRequiredSize();
//                     bufferViews[j]    = vulkanBuffer->GetVkBufferView();
//                     bufferInfos[j]    = bufferInfo;
//                 }
//                 writes[i].descriptorType   = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
//                 writes[i].pBufferInfo      = bufferInfos;
//                 writes[i].pTexelBufferView = bufferViews;
//             }
//             break;
//             case RHIShaderResourceType::eSamplerWithTextureBuffer:
//             {
//                 VERIFY_EXPR(srb.resources.size() % 2 == 0 && srb.resources.size() >= 2);
//                 numDescriptors = srb.resources.size() / 2;
//                 VkDescriptorImageInfo* imageInfos =
//                     ALLOCA_ARRAY(VkDescriptorImageInfo, numDescriptors);
//                 VkDescriptorBufferInfo* bufferInfos =
//                     ALLOCA_ARRAY(VkDescriptorBufferInfo, numDescriptors);
//                 VkBufferView* bufferViews = ALLOCA_ARRAY(VkBufferView, numDescriptors);
//                 for (uint32_t j = 0; j < numDescriptors; j++)
//                 {
//                     VkDescriptorImageInfo imageInfo{};
//                     imageInfo.sampler     = TO_VK_SAMPLER(srb.resources[j * 2 + 0])->GetVkSampler();
//                     imageInfo.imageView   = VK_NULL_HANDLE;
//                     imageInfo.imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
//                     imageInfos[j]         = imageInfo;
//
//                     VulkanBuffer* vulkanBuffer = TO_VK_BUFFER(srb.resources[j * 2 + 1]);
//                     VkDescriptorBufferInfo bufferInfo{};
//                     bufferInfo.buffer = vulkanBuffer->GetVkBuffer();
//                     bufferInfo.range  = vulkanBuffer->GetRequiredSize();
//                     bufferViews[j]    = vulkanBuffer->GetVkBufferView();
//                     bufferInfos[j]    = bufferInfo;
//                 }
//                 writes[i].descriptorType   = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
//                 writes[i].pImageInfo       = imageInfos;
//                 writes[i].pBufferInfo      = bufferInfos;
//                 writes[i].pTexelBufferView = bufferViews;
//             }
//             break;
//             case RHIShaderResourceType::eImageBuffer:
//             {
//                 numDescriptors = srb.resources.size();
//                 VkDescriptorBufferInfo* bufferInfos =
//                     ALLOCA_ARRAY(VkDescriptorBufferInfo, numDescriptors);
//                 VkBufferView* bufferViews = ALLOCA_ARRAY(VkBufferView, numDescriptors);
//                 for (uint32_t j = 0; j < numDescriptors; j++)
//                 {
//                     VulkanBuffer* vulkanBuffer = TO_VK_BUFFER(srb.resources[j]);
//                     VkDescriptorBufferInfo bufferInfo{};
//                     bufferInfo.buffer = vulkanBuffer->GetVkBuffer();
//                     bufferInfo.range  = vulkanBuffer->GetRequiredSize();
//                     bufferViews[j]    = vulkanBuffer->GetVkBufferView();
//                     bufferInfos[j]    = bufferInfo;
//                 }
//                 writes[i].descriptorType   = VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
//                 writes[i].pBufferInfo      = bufferInfos;
//                 writes[i].pTexelBufferView = bufferViews;
//             }
//             break;
//             case RHIShaderResourceType::eUniformBuffer:
//             {
//                 VulkanBuffer* vulkanBuffer         = TO_VK_BUFFER(srb.resources[0]);
//                 VkDescriptorBufferInfo* bufferInfo = ALLOCA_SINGLE(VkDescriptorBufferInfo);
//
//                 *bufferInfo        = {};
//                 bufferInfo->buffer = vulkanBuffer->GetVkBuffer();
//                 bufferInfo->range  = vulkanBuffer->GetRequiredSize();
//
//                 writes[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
//                 writes[i].pBufferInfo    = bufferInfo;
//             }
//             break;
//             case RHIShaderResourceType::eStorageBuffer:
//             {
//                 VulkanBuffer* vulkanBuffer         = TO_VK_BUFFER(srb.resources[0]);
//                 VkDescriptorBufferInfo* bufferInfo = ALLOCA_SINGLE(VkDescriptorBufferInfo);
//
//                 *bufferInfo        = {};
//                 bufferInfo->buffer = vulkanBuffer->GetVkBuffer();
//                 bufferInfo->range  = vulkanBuffer->GetRequiredSize();
//
//                 writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
//                 writes[i].pBufferInfo    = bufferInfo;
//             }
//             break;
//             case RHIShaderResourceType::eInputAttachment:
//             {
//                 numDescriptors = srb.resources.size();
//                 VkDescriptorImageInfo* imageInfos =
//                     ALLOCA_ARRAY(VkDescriptorImageInfo, numDescriptors);
//                 for (uint32_t j = 0; j < numDescriptors; j++)
//                 {
//                     VkDescriptorImageInfo imageInfo{};
//                     imageInfo.sampler     = VK_NULL_HANDLE;
//                     imageInfo.imageView   = TO_VK_TEXTURE(srb.resources[j])->GetVkImageView();
//                     imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
//                     imageInfos[j]         = imageInfo;
//                 }
//                 writes[i].descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
//                 writes[i].pImageInfo     = imageInfos;
//             }
//             break;
//             default:
//             {
//                 LOGE("Invalid descriptor type");
//             }
//         }
//         writes[i].descriptorCount = numDescriptors;
//     }
//     for (auto& write : writes)
//     {
//         VulkanDescriptorSet* descriptorSet =
//             reinterpret_cast<VulkanDescriptorSet*>(descriptorSetHandle.value);
//         write.dstSet = descriptorSet->descriptorSet;
//     }
//     vkUpdateDescriptorSets(m_device->GetVkHandle(), writes.size(), writes.data(), 0, nullptr);
// }
} // namespace zen