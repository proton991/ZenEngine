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
VulkanUniformBufferUsage CountUniformBufferDescriptors(
    const RHIShaderResourceDescriptorTable& srdTable)
{
    VulkanUniformBufferUsage usage{};

    uint32_t perStageCounts[ToUnderlying(RHIShaderStage::eMax)] = {};

    for (const SmallVector<RHIShaderResourceDescriptor>& setSRDs : srdTable)
    {
        for (const RHIShaderResourceDescriptor& srd : setSRDs)
        {
            if (srd.type == RHIShaderResourceType::eUniformBuffer && !srd.bindless)
            {
                usage.dynamicCount += srd.arraySize;

                for (uint32_t stage = 0; stage < ToUnderlying(RHIShaderStage::eMax); ++stage)
                {
                    if (srd.stageFlags.HasFlag(
                            RHIShaderStageToFlagBits(static_cast<RHIShaderStage>(stage))))
                    {
                        perStageCounts[stage] += srd.arraySize;
                    }
                }
            }
        }
    }

    for (uint32_t count : perStageCounts)
    {
        usage.maxPerStageCount = std::max(usage.maxPerStageCount, count);
    }

    return usage;
}

bool UniformBuffersFitLimits(const VulkanUniformBufferUsage& usage,
                             const VkPhysicalDeviceLimits& limits)
{
    // Dynamic uniform buffers also count against the general uniform-buffer limits.
    return usage.dynamicCount <= limits.maxDescriptorSetUniformBuffersDynamic &&
        usage.dynamicCount <= limits.maxDescriptorSetUniformBuffers &&
        usage.maxPerStageCount <= limits.maxPerStageDescriptorUniformBuffers;
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
    return GVulkanRHI->GetResourceFactory()->CreatePipeline(createInfo);
}

RHIPipeline* VulkanRHI::CreatePipeline(const RHIGfxPipelineCreateInfo& createInfo)
{
    return GVulkanRHI->GetResourceFactory()->CreatePipeline(createInfo);
}

void VulkanRHI::DestroyPipeline(RHIPipeline* pPipeline)
{
    pPipeline->ReleaseReference();
}

RHIShader* VulkanRHI::CreateShader(const RHIShaderCreateInfo& createInfo)
{
    return GVulkanRHI->GetResourceFactory()->CreateShader(createInfo);
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

    bool created = pShader->LoadSpirvFiles();

    if (created)
    {
        pShader->Init();

        created = pShader->m_fitsDeviceLimits && pShader->m_pipelineLayout != VK_NULL_HANDLE;
    }

    if (!created)
    {
        pShader->ReleaseReference();

        pShader = nullptr;
    }

    return pShader;
}

bool VulkanShader::LoadSpirvFiles()
{
    bool loaded = true;
    for (uint32_t i = 0; i < ToUnderlying(RHIShaderStage::eMax) && loaded; ++i)
    {
        const RHIShaderStage stage = static_cast<RHIShaderStage>(i);
        if (m_shaderGroupSPIRV->HasShaderStage(stage))
        {
            platform::FileLoadError error = platform::FileLoadError::eNone;
            HeapVector<uint8_t> code =
                platform::FileSystem::LoadSpvFile(m_spirvFileName[i], &error);
            loaded = error == platform::FileLoadError::eNone;
            if (loaded)
            {
                m_shaderGroupSPIRV->SetStageSPIRV(stage, std::move(code));
            }
        }
    }
    return loaded;
}

void VulkanShader::Init()
{
    RHIShaderGroupInfo sgInfo{};
    RHIShaderUtil::ReflectShaderGroupInfo(m_shaderGroupSPIRV, sgInfo);
    sgInfo.name = m_name;
    m_SRDTable  = sgInfo.SRDTable;

    if (m_SRDTable.size() <= kGlobalBindlessHeapIndex)
    {
        m_SRDTable.resize(kGlobalBindlessHeapIndex + 1);
    }

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

            spc.bits = m_specializationConstants.at(spc.constantId).bits;
        }
    }

    // Create specialization info from tracked state. This is shared by all shaders.
    const HeapVector<RHIShaderSpecializationConstant>& specConstants =
        sgInfo.specializationConstants;

    if (!specConstants.empty())
    {
        m_spcMapEntries.resize(specConstants.size());
        m_specializationData.resize(specConstants.size());

        for (uint32_t i = 0; i < specConstants.size(); i++)
        {
            VkSpecializationMapEntry& entry = m_spcMapEntries[i];
            // Vulkan scalar specialization values occupy four bytes, including VkBool32.
            entry.constantID = specConstants[i].constantId;
            entry.offset     = i * sizeof(uint32_t);
            entry.size       = sizeof(uint32_t);

            m_specializationData[i] =
                specConstants[i].type == RHIShaderSpecializationConstantType::eBool ?
                (specConstants[i].bits != 0 ? VK_TRUE : VK_FALSE) :
                specConstants[i].bits;
        }
    }

    m_specializationInfo.mapEntryCount = static_cast<uint32_t>(m_spcMapEntries.size());
    m_specializationInfo.pMapEntries   = m_spcMapEntries.empty() ? nullptr : m_spcMapEntries.data();
    m_specializationInfo.dataSize      = m_specializationData.size() * sizeof(uint32_t);
    m_specializationInfo.pData =
        m_specializationData.empty() ? nullptr : m_specializationData.data();

    // Reject an over-limit layout before creating native objects. Drivers need not reject
    // one; only the validation layer reports it.
    const VulkanUniformBufferUsage uniformUsage = CountUniformBufferDescriptors(m_SRDTable);

    const VkPhysicalDeviceLimits limits =
        GVulkanRHI->GetDevice()->GetPhysicalDeviceProperties().limits;

    m_fitsDeviceLimits = UniformBuffersFitLimits(uniformUsage, limits);

    if (m_fitsDeviceLimits)
    {
        InitNativeObjects(sgInfo);
    }
    else
    {
        LOGE("Shader '{}' exceeds device uniform-buffer limits: {} dynamic descriptors per "
             "layout (limit {}), {} in one stage (limit {})",
             m_name.CStr(), uniformUsage.dynamicCount,
             std::min(limits.maxDescriptorSetUniformBuffersDynamic,
                      limits.maxDescriptorSetUniformBuffers),
             uniformUsage.maxPerStageCount, limits.maxPerStageDescriptorUniformBuffers);
    }
}

void VulkanShader::InitNativeObjects(const RHIShaderGroupInfo& sgInfo)
{
    bool ready = true;

    m_stageCreateInfos.reserve(m_shaderGroupSPIRV->GetStageCount());

    for (uint32_t i = 0; ready && i < ToUnderlying(RHIShaderStage::eMax); i++)
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
            const VkResult result =
                vkCreateShaderModule(GVulkanRHI->GetVkDevice(), &shaderModuleCI, nullptr, &module);
            ready = result == VK_SUCCESS && module != VK_NULL_HANDLE;

            if (!ready)
            {
                LOGE("vkCreateShaderModule failed: {}", GetResultString(result));

                break;
            }

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

    const size_t setCount = m_SRDTable.size();
    HeapVector<HeapVector<VkDescriptorSetLayoutBinding>> dsBindings(setCount);
    m_descriptorSetInfos.resize(setCount);
    m_descriptorSetLayouts.resize(setCount);

    for (uint32_t i = 0; ready && i < setCount; i++)
    {
        // Set 0 is reserved for the same global heap in every pipeline layout.
        if (i == kGlobalBindlessHeapIndex)
        {
            const VkDescriptorSetLayout dsLayout =
                GVulkanRHI->GetBindlessDescriptorPoolManager()->GetGlobalBindlessLayout();
            ready = dsLayout != VK_NULL_HANDLE;

            m_descriptorSetLayouts[i]             = dsLayout;
            m_descriptorSetInfos[i].layoutId      = 0;
            m_descriptorSetInfos[i].poolKey       = {};
            m_descriptorSetInfos[i].variableCount = 0;
            m_hasGlobalBindlessSet                = true;

            continue;
        }

        uint32_t setVariableCount = 0;
        bool needUABFlag          = false;
        VulkanDescriptorPoolKey setPoolKey{};
        HeapVector<VkDescriptorBindingFlags> bindingFlags;

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
        const VkResult result =
            vkCreateDescriptorSetLayout(GVulkanRHI->GetVkDevice(), &layoutCI, nullptr, &dsLayout);
        ready = result == VK_SUCCESS && dsLayout != VK_NULL_HANDLE;

        if (!ready)
        {
            LOGE("vkCreateDescriptorSetLayout failed: {}", GetResultString(result));

            break;
        }

        m_descriptorSetLayouts[i]             = dsLayout;
        m_descriptorSetInfos[i].poolKey       = setPoolKey;
        m_descriptorSetInfos[i].ownsLayout    = true;
        m_descriptorSetInfos[i].variableCount = setVariableCount;
        m_descriptorSetInfos[i].layoutId =
            GVulkanRHI->GetDescriptorPoolManager2()->GetOrCreateLayoutId(layoutCI,
                                                                         MakeVecView(bindingFlags));
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

    if (ready)
    {
        const VkResult result = vkCreatePipelineLayout(GVulkanRHI->GetVkDevice(), &pipelineLayoutCI,
                                                       nullptr, &m_pipelineLayout);
        ready                 = result == VK_SUCCESS;

        if (!ready)
        {
            LOGE("vkCreatePipelineLayout failed: {}", GetResultString(result));

            if (m_pipelineLayout != VK_NULL_HANDLE)
            {
                vkDestroyPipelineLayout(GVulkanRHI->GetVkDevice(), m_pipelineLayout, nullptr);

                m_pipelineLayout = VK_NULL_HANDLE;
            }
        }
    }

    m_pushConstantsStageFlags =
        ShaderStageFlagsBitsToVkShaderStageFlags(sgInfo.pushConstants.stageFlags);

    const NameID debugName(fmt::format("{}_PipelineLayout", sgInfo.name.CStr()));
    if (ready)
    {
        GVulkanRHI->GetDevice()->SetObjectName(VK_OBJECT_TYPE_PIPELINE_LAYOUT,
                                               reinterpret_cast<uint64_t>(m_pipelineLayout),
                                               debugName);
    }
    for (uint32_t set = 0; set < m_SRDTable.size(); ++set)
    {
        SmallVector<DynamicOffsetSlot>& slots = m_dynamicOffsetSlots[set];

        for (const RHIShaderResourceDescriptor& descriptor : m_SRDTable[set])
        {
            if (descriptor.type == RHIShaderResourceType::eUniformBuffer)
            {
                slots.push_back({descriptor.binding,
                                 descriptor.bindless ? GetDescriptorSetVariableCount(set) :
                                                       descriptor.arraySize});
            }
        }

        std::sort(slots.begin(), slots.end(),
                  [](const DynamicOffsetSlot& left, const DynamicOffsetSlot& right) {
                      return left.binding < right.binding;
                  });
    }
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

static bool ValidateGraphicsPipeline(const RHIGfxPipelineCreateInfo& createInfo)
{
    const RHIRenderingLayout* layout = createInfo.pRenderingLayout;
    bool valid                       = createInfo.pShader != nullptr &&
        createInfo.pShader->GetResourceType() == RHIResourceType::eShader && layout != nullptr &&
        layout->numColorRenderTargets <= MAX_NUM_COLOR_ATTACHMENTS;

    if (valid)
    {
        // Layouts may describe formats without owning concrete textures/views.
        const SampleCount samples = createInfo.states.multiSampleState.sampleCount;

        for (uint32_t i = 0; i < layout->numColorRenderTargets; ++i)
        {
            const RHIRenderTarget& target = layout->colorRenderTargets[i];
            valid &= target.format != DataFormat::eUndefined && target.numSamples == samples &&
                !(FormatIsDepthOnly(target.format) || FormatIsStencilOnly(target.format) ||
                  FormatIsDepthStencil(target.format));
        }

        if (layout->hasDepthStencilRT)
        {
            const RHIRenderTarget& target = layout->depthStencilRenderTarget;
            valid &= target.numSamples == samples &&
                (FormatIsDepthOnly(target.format) || FormatIsStencilOnly(target.format) ||
                 FormatIsDepthStencil(target.format));
        }

        for (uint32_t index : createInfo.states.colorBlendState.attachmentsMask)
        {
            valid &= index < layout->numColorRenderTargets;
        }
    }

    if (!valid)
    {
        LOGE("Graphics pipeline requires a compatible shader and rendering layout");
    }

    return valid;
}

VulkanPipeline* VulkanPipeline::CreateObject(const RHIGfxPipelineCreateInfo& createInfo)
{
    VulkanPipeline* pGfxPipeline = nullptr;

    if (ValidateGraphicsPipeline(createInfo))
    {
        pGfxPipeline =
            VersatileResource::AllocMem<VulkanPipeline>(GVulkanRHI->GetResourceAllocator());

        new (pGfxPipeline) VulkanPipeline(createInfo);

        pGfxPipeline->InitGraphics(*createInfo.pRenderingLayout);

        if (!pGfxPipeline->m_initialized)
        {
            pGfxPipeline->ReleaseReference();

            pGfxPipeline = nullptr;
        }
    }

    return pGfxPipeline;
}

VulkanPipeline* VulkanPipeline::CreateObject(const RHIComputePipelineCreateInfo& createInfo)
{
    VulkanPipeline* pCompPipeline = nullptr;

    if (createInfo.pShader != nullptr &&
        createInfo.pShader->GetResourceType() == RHIResourceType::eShader)
    {
        VulkanShader* shader = static_cast<VulkanShader*>(createInfo.pShader);

        if (shader->GetNumShaderStages() == 1 &&
            shader->GetStageCreateInfoData()[0].stage == VK_SHADER_STAGE_COMPUTE_BIT)
        {
            pCompPipeline =
                VersatileResource::AllocMem<VulkanPipeline>(GVulkanRHI->GetResourceAllocator());

            new (pCompPipeline) VulkanPipeline(createInfo);

            pCompPipeline->Init();

            if (!pCompPipeline->m_initialized)
            {
                pCompPipeline->ReleaseReference();

                pCompPipeline = nullptr;
            }
        }
    }

    if (pCompPipeline == nullptr)
    {
        LOGE("Compute pipeline creation failed");
    }

    return pCompPipeline;
}

void VulkanPipeline::Init()
{
    InitCompute();
    m_bindPoint = VK_PIPELINE_BIND_POINT_COMPUTE;
}

void VulkanPipeline::Destroy()
{
    vkDestroyPipeline(GVulkanRHI->GetVkDevice(), m_vkPipeline, nullptr);
    this->~VulkanPipeline();
    VersatileResource::Free(GVulkanRHI->GetResourceAllocator(), this);
}

void VulkanPipeline::InitGraphics(const RHIRenderingLayout& layout)
{
    // Input Assembly
    VkPipelineInputAssemblyStateCreateInfo IAStateCI;
    InitVkStruct(IAStateCI, VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO);
    IAStateCI.topology               = ToVkPrimitiveTopology(m_gfxStates.primitiveType);
    IAStateCI.primitiveRestartEnable = VK_FALSE;

    // Viewport State
    VkPipelineViewportStateCreateInfo VPStateCI;
    InitVkStruct(VPStateCI, VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO);
    VPStateCI.scissorCount       = 1;
    VPStateCI.viewportCount      = 1;
    const Rect2<int>& renderArea = layout.renderArea;
    const VkViewport viewport{static_cast<float>(renderArea.minX),
                              static_cast<float>(renderArea.minY),
                              static_cast<float>(renderArea.Width()),
                              static_cast<float>(renderArea.Height()),
                              0.0f,
                              1.0f};
    const VkRect2D scissor{
        {static_cast<int32_t>(renderArea.minX), static_cast<int32_t>(renderArea.minY)},
        {static_cast<uint32_t>(renderArea.Width()), static_cast<uint32_t>(renderArea.Height())}};
    VPStateCI.pViewports = &viewport;
    VPStateCI.pScissors  = &scissor;

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
    // Vulkan consumes one 32-bit word per 32 samples, in sample-index order.
    const VkSampleMask sampleMasks[] = {
        static_cast<VkSampleMask>(multisampleState.sampleMasks),
        static_cast<VkSampleMask>(multisampleState.sampleMasks >> 32)};
    VkPipelineMultisampleStateCreateInfo MSStateCI;
    InitVkStruct(MSStateCI, VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO);
    MSStateCI.rasterizationSamples  = ToVkSampleCountFlagBits(multisampleState.sampleCount);
    MSStateCI.sampleShadingEnable   = multisampleState.enableSampleShading;
    MSStateCI.minSampleShading      = multisampleState.minSampleShading;
    MSStateCI.alphaToCoverageEnable = multisampleState.enableAlphaToCoverage;
    MSStateCI.alphaToOneEnable      = multisampleState.enableAlphaToOne;
    MSStateCI.pSampleMask           = sampleMasks;

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
    // Attachment indices are render-target locations; unmasked targets disable color writes.
    vkCBAttStates.resize(layout.numColorRenderTargets);

    for (uint32_t i : colorBlendState.attachmentsMask)
    {
        VERIFY_EXPR(i < vkCBAttStates.size());
        vkCBAttStates[i].blendEnable = colorBlendState.attachments[i].enableBlend;

        vkCBAttStates[i].srcColorBlendFactor =
            ToVkBlendFactor(colorBlendState.attachments[i].srcColorBlendFactor);
        vkCBAttStates[i].dstColorBlendFactor =
            ToVkBlendFactor(colorBlendState.attachments[i].dstColorBlendFactor);
        vkCBAttStates[i].colorBlendOp = ToVkBlendOp(colorBlendState.attachments[i].colorBlendOp);

        vkCBAttStates[i].srcAlphaBlendFactor =
            ToVkBlendFactor(colorBlendState.attachments[i].srcAlphaBlendFactor);
        vkCBAttStates[i].dstAlphaBlendFactor =
            ToVkBlendFactor(colorBlendState.attachments[i].dstAlphaBlendFactor);
        vkCBAttStates[i].alphaBlendOp   = ToVkBlendOp(colorBlendState.attachments[i].alphaBlendOp);
        vkCBAttStates[i].colorWriteMask = colorBlendState.attachments[i].colorWriteMask;
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
    HeapVector<VkDynamicState> vkDynamicStates;
    vkDynamicStates.reserve(numDynamicStates);

    for (uint32_t i : m_gfxStates.dynamicStates.enabledStates)
    {
        vkDynamicStates.push_back(ToVkDynamicState(static_cast<RHIDynamicState>(i)));
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
    pipelineCI.subpass             = 0;

    HeapVector<VkFormat> colorAttachmentFormats;
    VkPipelineRenderingCreateInfoKHR renderingCI;

    pipelineCI.renderPass = VK_NULL_HANDLE;
    colorAttachmentFormats.reserve(layout.numColorRenderTargets);

    for (uint32_t i = 0; i < layout.numColorRenderTargets; i++)
    {
        VkFormat colorFormat = ToVkFormat(layout.colorRenderTargets[i].format);
        colorAttachmentFormats.emplace_back(colorFormat);
    }

    InitVkStruct(renderingCI, VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR);
    renderingCI.colorAttachmentCount    = layout.numColorRenderTargets;
    renderingCI.pColorAttachmentFormats = colorAttachmentFormats.data();

    if (layout.hasDepthStencilRT)
    {
        const DataFormat format = layout.depthStencilRenderTarget.format;
        const BitField<RHITextureAspectFlagBits> aspects =
            layout.depthStencilRenderTarget.GetAspects();
        if (aspects.HasFlag(RHITextureAspectFlagBits::eDepth))
        {
            renderingCI.depthAttachmentFormat = ToVkFormat(format);
        }
        if (aspects.HasFlag(RHITextureAspectFlagBits::eStencil))
        {
            renderingCI.stencilAttachmentFormat = ToVkFormat(format);
        }
    }

    pipelineCI.pNext = &renderingCI;

    const VkResult result = vkCreateGraphicsPipelines(GVulkanRHI->GetVkDevice(),
                                                      GVulkanRHI->GetDevice()->GetPipelineCache(),
                                                      1, &pipelineCI, nullptr, &m_vkPipeline);

    m_initialized = result == VK_SUCCESS && m_vkPipeline != VK_NULL_HANDLE;

    if (!m_initialized)
    {
        LOGE("vkCreateGraphicsPipelines failed: {}", GetResultString(result));
    }

    m_pushConstantsStageFlags = pShader->GetPushConstantsStageFlags();
}

void VulkanPipeline::InitCompute()
{
    VulkanShader* pShader = TO_VK_SHADER(m_pShader);

    VkComputePipelineCreateInfo pipelineCI{};
    pipelineCI.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineCI.stage  = pShader->GetStageCreateInfoData()[0];
    pipelineCI.layout = pShader->GetVkPipelineLayout();

    const VkResult result = vkCreateComputePipelines(GVulkanRHI->GetVkDevice(),
                                                     GVulkanRHI->GetDevice()->GetPipelineCache(), 1,
                                                     &pipelineCI, nullptr, &m_vkPipeline);

    m_initialized = result == VK_SUCCESS && m_vkPipeline != VK_NULL_HANDLE;

    if (!m_initialized)
    {
        LOGE("vkCreateComputePipelines failed: {}", GetResultString(result));
    }
    m_pushConstantsStageFlags = pShader->GetPushConstantsStageFlags();
}

} // namespace zen
