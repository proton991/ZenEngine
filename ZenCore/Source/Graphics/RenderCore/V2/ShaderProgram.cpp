#include "Graphics/RHI/RHIShaderUtil.h"
#include "Graphics/RenderCore/V2/ShaderProgram.h"
#include "Graphics/RenderCore/V2/RenderDevice.h"
#include "Utils/Errors.h"
#include <fstream>

namespace zen::rc
{
static std::vector<uint8_t> LoadSpirvCode(const std::string& name)
{
    const auto path = std::string(SPV_SHADER_PATH) + name;
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    bool opened = file.is_open();
    VERIFY_EXPR_MSG_F(file.is_open(), "Failed to load shader file {}", path);
    //find what the size of the file is by looking up the location of the cursor
    //because the cursor is at the end, it gives the size directly in bytes
    size_t fileSize = (size_t)file.tellg();

    //spir-v expects the buffer to be on uint32, so make sure to reserve an int vector big enough for the entire file
    std::vector<uint8_t> buffer(fileSize / sizeof(uint8_t));

    //put file cursor at beginning
    file.seekg(0);

    //load the entire file into the buffer
    file.read((char*)buffer.data(), fileSize);

    //now that the file is loaded into the buffer, we can close it
    file.close();

    return buffer;
}

ShaderProgram::ShaderProgram(RenderDevice* renderDevice, std::string name) :
    m_renderDevice(renderDevice), m_name(std::move(name))
{}

ShaderProgram::~ShaderProgram()
{
    GDynamicRHI->DestroyShader(m_shader);
}

void ShaderProgram::UpdateUniformBuffer(const std::string& name,
                                        const uint8_t* data,
                                        uint32_t offset)
{
    if (m_uniformBufferMap.contains(name))
    {
        m_renderDevice->UpdateBuffer(m_uniformBufferMap[name], m_uniformBufferSizes[name], data,
                                     offset);
    }
}

void ShaderProgram::Init()
{
    // auto shaderGroupSpirv = MakeRefCountPtr<RHIShaderGroupSPIRV>();
    // for (const auto& kv : m_stages)
    // {
    //     shaderGroupSpirv->SetStageSPIRV(kv.first, LoadSpirvCode(kv.second));
    // }
    // RHIShaderGroupInfo shaderGroupInfo{};
    // RHIShaderUtil::ReflectShaderGroupInfo(shaderGroupSpirv, shaderGroupInfo);
    // shaderGroupInfo.name = m_name;

    RHIShaderCreateInfo shaderCreateInfo{};

    for (const auto& kv : m_stages)
    {
        shaderCreateInfo.spirvFileName[ToUnderlying(kv.first)] = kv.second;
        shaderCreateInfo.stageFlags.SetFlag(
            static_cast<RHIShaderStageFlagBits>(1 << ToUnderlying(kv.first)));

        // shaderGroupSpirv->SetStageSPIRV(kv.first, LoadSpirvCode(kv.second));
    }
    shaderCreateInfo.name = m_name;

    m_shader = GDynamicRHI->CreateShader(shaderCreateInfo);

    m_shader->GetShaderResourceDescriptorTable(m_SRDTable);
    // m_SRDs   = std::move(shaderGroupInfo.SRDs);

    for (auto& setSRD : m_SRDTable)
    {
        for (const auto& srd : setSRD)
        {
            if (srd.type == RHIShaderResourceType::eUniformBuffer)
            {
                m_uniformBufferMap[srd.name] =
                    m_renderDevice->CreateUniformBuffer(srd.blockSize, nullptr, srd.name);
                m_uniformBufferSizes[srd.name] = srd.blockSize;
                m_uniformBuffers.emplace_back(srd);
            }
            else if (srd.type == RHIShaderResourceType::eStorageBuffer)
            {
                m_storageBuffers.emplace_back(srd);
            }
            else if (srd.type == RHIShaderResourceType::eImage)
            {
                m_storageImages.emplace_back(srd);
            }
            else if (srd.type == RHIShaderResourceType::eTexture ||
                     srd.type == RHIShaderResourceType::eSamplerWithTexture)
            {
                m_sampledTextures.emplace_back(srd);
            }
        }
    }
}

void ShaderProgram::Init(const HashMap<uint32_t, int>& specializationConstants)
{
    // RHIShaderGroupSPIRVPtr shaderGroupSpirv = MakeRefCountPtr<RHIShaderGroupSPIRV>();
    // for (const auto& kv : m_stages)
    // {
    //     shaderGroupSpirv->SetStageSPIRV(kv.first, LoadSpirvCode(kv.second));
    // }
    // RHIShaderGroupInfo shaderGroupInfo{};
    // RHIShaderUtil::ReflectShaderGroupInfo(shaderGroupSpirv, shaderGroupInfo);
    //
    // if (!specializationConstants.empty())
    // {
    //     // set specialization constants
    //     for (auto& spc : m_shader->GetSpecializationConstants())
    //     {
    //         switch (spc.type)
    //         {
    //
    //             case RHIShaderSpecializationConstantType::eBool:
    //                 spc.boolValue = static_cast<bool>(specializationConstants.at(spc.constantId));
    //                 break;
    //             case RHIShaderSpecializationConstantType::eInt:
    //                 spc.intValue = specializationConstants.at(spc.constantId);
    //                 break;
    //             case RHIShaderSpecializationConstantType::eFloat:
    //                 spc.floatValue = static_cast<float>(specializationConstants.at(spc.constantId));
    //                 break;
    //             default: break;
    //         }
    //     }
    // }
    RHIShaderCreateInfo shaderCreateInfo{};

    for (const auto& kv : m_stages)
    {
        shaderCreateInfo.spirvFileName[ToUnderlying(kv.first)] = kv.second;
        shaderCreateInfo.stageFlags.SetFlag(
            static_cast<RHIShaderStageFlagBits>(1 << ToUnderlying(kv.first)));

        // shaderGroupSpirv->SetStageSPIRV(kv.first, LoadSpirvCode(kv.second));
    }
    shaderCreateInfo.name                    = m_name;
    shaderCreateInfo.specializationConstants = specializationConstants;

    m_shader = GDynamicRHI->CreateShader(shaderCreateInfo);
    // m_shader = GDynamicRHI->CreateShader(shaderGroupInfo);
    m_shader->GetShaderResourceDescriptorTable(m_SRDTable);

    for (auto& setSRD : m_SRDTable)
    {
        for (const auto& srd : setSRD)
        {
            if (srd.type == RHIShaderResourceType::eUniformBuffer)
            {
                m_uniformBufferMap[srd.name] =
                    m_renderDevice->CreateUniformBuffer(srd.blockSize, nullptr, srd.name);
                m_uniformBufferSizes[srd.name] = srd.blockSize;
                m_uniformBuffers.emplace_back(srd);
            }
            else if (srd.type == RHIShaderResourceType::eStorageBuffer)
            {
                m_storageBuffers.emplace_back(srd);
            }
            else if (srd.type == RHIShaderResourceType::eImage)
            {
                m_storageImages.emplace_back(srd);
            }
            else if (srd.type == RHIShaderResourceType::eTexture ||
                     srd.type == RHIShaderResourceType::eSamplerWithTexture)
            {
                m_sampledTextures.emplace_back(srd);
            }
        }
    }
}

ShaderProgram* ShaderProgramManager::CreateShaderProgram(RenderDevice* renderDevice,
                                                         const std::string& name)
{
    ShaderProgram* shaderProgram = new ShaderProgram(renderDevice, name);
    m_programCache[name]         = shaderProgram;
    return shaderProgram;
}

void ShaderProgramManager::Destroy()
{
    for (auto& kv : m_programCache)
    {
        delete kv.second;
    }
}

void ShaderProgramManager::BuildShaderPrograms(RenderDevice* renderDevice)
{
    {
        ShaderProgram* shaderProgram             = new GBufferSP(renderDevice);
        m_programCache[shaderProgram->GetName()] = shaderProgram;
    }
    {
        ShaderProgram* shaderProgram             = new DeferredLightingSP(renderDevice);
        m_programCache[shaderProgram->GetName()] = shaderProgram;
    }
    {
        ShaderProgram* shaderProgram             = new EnvMapIrradianceSP(renderDevice);
        m_programCache[shaderProgram->GetName()] = shaderProgram;
    }
    {
        ShaderProgram* shaderProgram             = new EnvMapPrefilteredSP(renderDevice);
        m_programCache[shaderProgram->GetName()] = shaderProgram;
    }
    {
        ShaderProgram* shaderProgram             = new SkyboxRenderSP(renderDevice);
        m_programCache[shaderProgram->GetName()] = shaderProgram;
    }
    {
        ShaderProgram* shaderProgram             = new EnvMapBRDFLutGenSP(renderDevice);
        m_programCache[shaderProgram->GetName()] = shaderProgram;
    }
    if (renderDevice->GetGPUInfo().supportGeometryShader)
    {
        {
            ShaderProgram* shaderProgram             = new VoxelizationSP(renderDevice);
            m_programCache[shaderProgram->GetName()] = shaderProgram;
        }
        {
            ShaderProgram* shaderProgram             = new VoxelDrawSP(renderDevice);
            m_programCache[shaderProgram->GetName()] = shaderProgram;
        }
    }
    {
        ShaderProgram* shaderProgram             = new ResetComputeIndirectSP(renderDevice);
        m_programCache[shaderProgram->GetName()] = shaderProgram;
    }
    {
        ShaderProgram* shaderProgram             = new ResetDrawIndirectSP(renderDevice);
        m_programCache[shaderProgram->GetName()] = shaderProgram;
    }
    {
        ShaderProgram* shaderProgram             = new ResetVoxelTextureSP(renderDevice);
        m_programCache[shaderProgram->GetName()] = shaderProgram;
    }
    {
        ShaderProgram* shaderProgram             = new VoxelizationCompSP(renderDevice);
        m_programCache[shaderProgram->GetName()] = shaderProgram;
    }
    {
        ShaderProgram* shaderProgram = new VoxelizationLargeTriangleCompSP(renderDevice);
        m_programCache[shaderProgram->GetName()] = shaderProgram;
    }
    {
        ShaderProgram* shaderProgram             = new VoxelPreDrawSP(renderDevice);
        m_programCache[shaderProgram->GetName()] = shaderProgram;
    }
    {
        ShaderProgram* shaderProgram             = new VoxelDrawSP2(renderDevice);
        m_programCache[shaderProgram->GetName()] = shaderProgram;
    }
    {
        ShaderProgram* shaderProgram             = new ShadowMapRenderSP(renderDevice);
        m_programCache[shaderProgram->GetName()] = shaderProgram;
    }
    {
        ShaderProgram* shaderProgram             = new VoxelInjectRadianceSP(renderDevice);
        m_programCache[shaderProgram->GetName()] = shaderProgram;
    }
}
} // namespace zen::rc