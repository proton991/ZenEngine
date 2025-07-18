#include "Graphics/RHI/ShaderUtil.h"
#include "Graphics/RenderCore/V2/ShaderProgram.h"
#include "Graphics/RenderCore/V2/RenderDevice.h"
#include "Common/Errors.h"
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
    m_renderDevice->GetRHI()->DestroyShader(m_shader);
}

void ShaderProgram::UpdateUniformBuffer(const std::string& name,
                                        const uint8_t* data,
                                        uint32_t offset)
{
    if (m_uniformBuffers.contains(name))
    {
        m_renderDevice->UpdateBuffer(m_uniformBuffers[name], m_uniformBufferSizes[name], data,
                                     offset);
    }
}

void ShaderProgram::Init()
{
    auto shaderGroupSpirv = MakeRefCountPtr<rhi::ShaderGroupSPIRV>();
    for (const auto& kv : m_stages)
    {
        shaderGroupSpirv->SetStageSPIRV(kv.first, LoadSpirvCode(kv.second));
    }
    rhi::ShaderGroupInfo shaderGroupInfo{};
    rhi::ShaderUtil::ReflectShaderGroupInfo(shaderGroupSpirv, shaderGroupInfo);
    shaderGroupInfo.name = m_name;

    m_shader = m_renderDevice->GetRHI()->CreateShader(shaderGroupInfo);
    m_SRDs   = std::move(shaderGroupInfo.SRDs);

    for (auto& setSRD : m_SRDs)
    {
        for (const auto& srd : setSRD)
        {
            if (srd.type == rhi::ShaderResourceType::eUniformBuffer)
            {
                m_uniformBuffers[srd.name] =
                    m_renderDevice->CreateUniformBuffer(srd.blockSize, nullptr);
                m_uniformBufferSizes[srd.name] = srd.blockSize;
            }
        }
    }
}

void ShaderProgram::Init(const HashMap<uint32_t, int>& specializationConstants)
{
    auto shaderGroupSpirv = MakeRefCountPtr<rhi::ShaderGroupSPIRV>();
    for (const auto& kv : m_stages)
    {
        shaderGroupSpirv->SetStageSPIRV(kv.first, LoadSpirvCode(kv.second));
    }
    rhi::ShaderGroupInfo shaderGroupInfo{};
    rhi::ShaderUtil::ReflectShaderGroupInfo(shaderGroupSpirv, shaderGroupInfo);

    if (!specializationConstants.empty())
    {
        // set specialization constants
        for (auto& spc : shaderGroupInfo.specializationConstants)
        {
            switch (spc.type)
            {

                case rhi::ShaderSpecializationConstantType::eBool:
                    spc.boolValue = static_cast<bool>(specializationConstants.at(spc.constantId));
                    break;
                case rhi::ShaderSpecializationConstantType::eInt:
                    spc.intValue = specializationConstants.at(spc.constantId);
                    break;
                case rhi::ShaderSpecializationConstantType::eFloat:
                    spc.floatValue = static_cast<float>(specializationConstants.at(spc.constantId));
                    break;
                default: break;
            }
        }
    }

    m_shader = m_renderDevice->GetRHI()->CreateShader(shaderGroupInfo);
    m_SRDs   = std::move(shaderGroupInfo.SRDs);

    for (auto& setSRD : m_SRDs)
    {
        for (const auto& srd : setSRD)
        {
            if (srd.type == rhi::ShaderResourceType::eUniformBuffer)
            {
                m_uniformBuffers[srd.name] =
                    m_renderDevice->CreateUniformBuffer(srd.blockSize, nullptr);
            }
        }
    }
}

VoxelizationProgram::VoxelizationProgram(RenderDevice* renderDevice) :
    ShaderProgram(renderDevice, "VoxelizationSP")
{
    AddShaderStage(rhi::ShaderStage::eVertex, "VoxelGI/voxelization.vert.spv");
    AddShaderStage(rhi::ShaderStage::eGeometry, "VoxelGI/voxelization.geom.spv");
    AddShaderStage(rhi::ShaderStage::eFragment, "VoxelGI/voxelization.frag.spv");
    Init();
}

VoxelDrawShaderProgram::VoxelDrawShaderProgram(RenderDevice* renderDevice) :
    ShaderProgram(renderDevice, "VoxelDrawSP")
{
    AddShaderStage(rhi::ShaderStage::eVertex, "VoxelGI/draw_voxels.vert.spv");
    AddShaderStage(rhi::ShaderStage::eGeometry, "VoxelGI/draw_voxels.geom.spv");
    AddShaderStage(rhi::ShaderStage::eFragment, "VoxelGI/draw_voxels.frag.spv");
    Init();
}

VoxelizationCompShaderProgram::VoxelizationCompShaderProgram(RenderDevice* renderDevice) :
    ShaderProgram(renderDevice, "VoxelizationCompSP")
{
    AddShaderStage(rhi::ShaderStage::eCompute, "VoxelGI/voxelization.comp.spv");
    Init();
}

VoxelPreDrawShaderProgram::VoxelPreDrawShaderProgram(RenderDevice* renderDevice) :
    ShaderProgram(renderDevice, "VoxelPreDrawSP")
{
    AddShaderStage(rhi::ShaderStage::eCompute, "VoxelGI/voxel_pre_draw.comp.spv");
    Init();
}

ResetDrawIndirectShaderProgram::ResetDrawIndirectShaderProgram(RenderDevice* renderDevice) :
    ShaderProgram(renderDevice, "ResetDrawIndirectSP")
{
    AddShaderStage(rhi::ShaderStage::eCompute, "VoxelGI/reset_draw_indirect.comp.spv");
    Init();
}

ResetCompIndirectShaderProgram::ResetCompIndirectShaderProgram(RenderDevice* renderDevice) :
    ShaderProgram(renderDevice, "ResetCompIndirectSP")
{
    AddShaderStage(rhi::ShaderStage::eCompute, "VoxelGI/reset_compute_indirect.comp.spv");
    Init();
}

ResetVoxelTextureShaderProgram::ResetVoxelTextureShaderProgram(RenderDevice* renderDevice) :
    ShaderProgram(renderDevice, "ResetVoxelTextureSP")
{
    AddShaderStage(rhi::ShaderStage::eCompute, "VoxelGI/reset_voxel_texture.comp.spv");
    Init();
}

VoxelDrawShaderProgram2::VoxelDrawShaderProgram2(RenderDevice* renderDevice) :
    ShaderProgram(renderDevice, "VoxelDrawSP2")
{
    AddShaderStage(rhi::ShaderStage::eVertex, "VoxelGI/voxel_vis.vert.spv");
    AddShaderStage(rhi::ShaderStage::eFragment, "VoxelGI/voxel_vis.frag.spv");
    Init();
}

ShadowMapRenderProgram::ShadowMapRenderProgram(RenderDevice* renderDevice) :
    ShaderProgram(renderDevice, "ShadowMapRenderSP")
{
    AddShaderStage(rhi::ShaderStage::eVertex, "ShadowMapping/evsm.vert.spv");
    AddShaderStage(rhi::ShaderStage::eFragment, "ShadowMapping/evsm.frag.spv");
    Init();
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
        ShaderProgram* shaderProgram             = new GBufferShaderProgram(renderDevice);
        m_programCache[shaderProgram->GetName()] = shaderProgram;
    }
    {
        ShaderProgram* shaderProgram             = new DeferredLightingShaderProgram(renderDevice);
        m_programCache[shaderProgram->GetName()] = shaderProgram;
    }
    {
        ShaderProgram* shaderProgram             = new EnvMapIrradianceProgram(renderDevice);
        m_programCache[shaderProgram->GetName()] = shaderProgram;
    }
    {
        ShaderProgram* shaderProgram             = new EnvMapPrefilteredProgram(renderDevice);
        m_programCache[shaderProgram->GetName()] = shaderProgram;
    }
    {
        ShaderProgram* shaderProgram             = new SkyboxRenderProgram(renderDevice);
        m_programCache[shaderProgram->GetName()] = shaderProgram;
    }
    {
        ShaderProgram* shaderProgram             = new EnvMapBRDFLutGenProgram(renderDevice);
        m_programCache[shaderProgram->GetName()] = shaderProgram;
    }
    if (renderDevice->GetGPUInfo().supportGeometryShader)
    {
        {
            ShaderProgram* shaderProgram             = new VoxelizationProgram(renderDevice);
            m_programCache[shaderProgram->GetName()] = shaderProgram;
        }
        {
            ShaderProgram* shaderProgram             = new VoxelDrawShaderProgram(renderDevice);
            m_programCache[shaderProgram->GetName()] = shaderProgram;
        }
    }
    {
        ShaderProgram* shaderProgram             = new ResetCompIndirectShaderProgram(renderDevice);
        m_programCache[shaderProgram->GetName()] = shaderProgram;
    }
    {
        ShaderProgram* shaderProgram             = new ResetDrawIndirectShaderProgram(renderDevice);
        m_programCache[shaderProgram->GetName()] = shaderProgram;
    }
    {
        ShaderProgram* shaderProgram             = new ResetVoxelTextureShaderProgram(renderDevice);
        m_programCache[shaderProgram->GetName()] = shaderProgram;
    }
    {
        ShaderProgram* shaderProgram             = new VoxelizationCompShaderProgram(renderDevice);
        m_programCache[shaderProgram->GetName()] = shaderProgram;
    }
    {
        ShaderProgram* shaderProgram             = new VoxelPreDrawShaderProgram(renderDevice);
        m_programCache[shaderProgram->GetName()] = shaderProgram;
    }
    {
        ShaderProgram* shaderProgram             = new VoxelDrawShaderProgram2(renderDevice);
        m_programCache[shaderProgram->GetName()] = shaderProgram;
    }
    {
        ShaderProgram* shaderProgram             = new ShadowMapRenderProgram(renderDevice);
        m_programCache[shaderProgram->GetName()] = shaderProgram;
    }
}
} // namespace zen::rc