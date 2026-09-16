#include "Graphics/RHI/RHIShaderUtil.h"
#include "Graphics/RenderCore/V2/ShaderProgram.h"
#include "Graphics/RenderCore/V2/RenderDevice.h"
#include "Memory/Memory.h"
#include "Utils/Errors.h"

namespace zen::rc
{
ShaderProgram::ShaderProgram(RenderDevice* pRenderDevice, NameID name) :
    m_pRenderDevice(pRenderDevice), m_name(name)
{}

ShaderProgram::~ShaderProgram()
{
    if (m_pShader != nullptr)
    {
        if (m_pRenderDevice != nullptr)
        {
            m_pRenderDevice->DeferReleaseResource(m_pShader);
        }
        else
        {
            m_pShader->ReleaseReference();
        }
    }
}

void ShaderProgram::UpdateUniformBuffer(NameID name, const uint8_t* pData, uint32_t offset)
{
    if (m_uniformBufferMap.contains(name))
    {
        m_pRenderDevice->UpdateBuffer(m_uniformBufferMap[name], m_namedSRDLut[name]->blockSize,
                                      pData, offset);
    }
}

bool ShaderProgram::Init()
{
    return Init({});
}

bool ShaderProgram::Init(const HashMap<uint32_t, int>& specializationConstants)
{
    bool result{};

    if (GDynamicRHI == nullptr)
    {
        LOGE("Shader '{}' initialization failed: missing RHI", m_name.CStr());

        result = false;
    }
    else
    {
        RHIShaderCreateInfo info{};
        std::ranges::copy(m_stageSources, info.spirvFileName);
        info.stageFlags              = m_stageFlags;
        info.name                    = m_name;
        info.specializationConstants = specializationConstants;
        RHIShader* shader            = GDynamicRHI->CreateShader(info);

        if (shader == nullptr)
        {
            LOGE("Shader '{}' initialization failed", m_name.CStr());

            result = false;
        }
        else
        {
            RHIShader* previous = m_pShader;
            m_pShader           = shader;
            ResolveShaderResources();

            if (previous != nullptr)
            {
                if (m_pRenderDevice != nullptr)
                {
                    m_pRenderDevice->DeferReleaseResource(previous);
                }
                else
                {
                    previous->ReleaseReference();
                }
            }

            result = true;
        }
    }

    return result;
}

void ShaderProgram::ResolveShaderResources()
{
    m_namedSRDLut.clear();
    m_storageBuffers.clear();
    m_sampledTextures.clear();
    m_storageImages.clear();

    if (m_pShader == nullptr)
    {
        return;
    }

    m_storageBuffers.reserve(m_pShader->GetSRDCountByType(RHIShaderResourceType::eStorageBuffer));
    m_sampledTextures.reserve(
        m_pShader->GetSRDCountByType(RHIShaderResourceType::eSamplerWithTexture));
    m_storageImages.reserve(m_pShader->GetSRDCountByType(RHIShaderResourceType::eImage));

    const RHIShaderResourceDescriptorTable* SRDTable = m_pShader->GetSRDTable();

    for (SmallVector<RHIShaderResourceDescriptor> const& setSRD : *SRDTable)
    {
        for (RHIShaderResourceDescriptor const& srd : setSRD)
        {
            m_namedSRDLut[srd.name] = &srd;

            if (srd.type == RHIShaderResourceType::eStorageBuffer)
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

ShaderProgram* ShaderProgramManager::CreateShaderProgram(RenderDevice* pRenderDevice, NameID name)
{
    ShaderProgram* pShaderProgram = ZEN_NEW() ShaderProgram(pRenderDevice, name);
    StoreProgram(pShaderProgram);

    return pShaderProgram;
}

void ShaderProgramManager::StoreProgram(ShaderProgram* program)
{
    const std::pair<HashMap<NameID, ShaderProgram*>::iterator, bool> itResult =
        m_programCache.try_emplace(program->GetName(), program);
    HashMap<NameID, ShaderProgram*>::iterator it = itResult.first;
    bool inserted                                = itResult.second;

    if (!inserted && it->second != program)
    {
        ShaderProgram* previous = it->second;
        it->second              = program;
        ZEN_DELETE(previous);
    }
}

void ShaderProgramManager::Destroy()
{
    for (std::pair<const NameID, ShaderProgram*>& kv : m_programCache)
    {
        ZEN_DELETE(kv.second);
    }

    m_programCache.clear();
}

void ShaderProgramManager::BuildShaderPrograms(RenderDevice* pRenderDevice)
{
    {
        ShaderProgram* pShaderProgram = ZEN_NEW() GBufferSP(pRenderDevice);
        StoreProgram(pShaderProgram);
    }

    {
        ShaderProgram* pShaderProgram = ZEN_NEW() DeferredLightingSP(pRenderDevice);
        StoreProgram(pShaderProgram);
    }

    {
        ShaderProgram* pShaderProgram = ZEN_NEW() EnvMapIrradianceSP(pRenderDevice);
        StoreProgram(pShaderProgram);
    }

    {
        ShaderProgram* pShaderProgram = ZEN_NEW() EnvMapPrefilteredSP(pRenderDevice);
        StoreProgram(pShaderProgram);
    }

    {
        ShaderProgram* pShaderProgram = ZEN_NEW() SkyboxRenderSP(pRenderDevice);
        StoreProgram(pShaderProgram);
    }

    {
        ShaderProgram* pShaderProgram = ZEN_NEW() EnvMapBRDFLutGenSP(pRenderDevice);
        StoreProgram(pShaderProgram);
    }

    if (ResolveVoxelizerMode(platform::ConfigLoader::GetInstance().GetVoxelizerMode(),
                             pRenderDevice->GetGPUInfo()) == platform::VoxelizerMode::eGeometry)
    {
        {
            ShaderProgram* pShaderProgram = ZEN_NEW() VoxelizationSP(pRenderDevice);
            StoreProgram(pShaderProgram);
        }

        {
            ShaderProgram* pShaderProgram = ZEN_NEW() VoxelDrawSP(pRenderDevice);
            StoreProgram(pShaderProgram);
        }
    }

    {
        ShaderProgram* pShaderProgram = ZEN_NEW() ResetComputeIndirectSP(pRenderDevice);
        StoreProgram(pShaderProgram);
    }

    {
        ShaderProgram* pShaderProgram = ZEN_NEW() ResetDrawIndirectSP(pRenderDevice);
        StoreProgram(pShaderProgram);
    }

    {
        ShaderProgram* pShaderProgram = ZEN_NEW() ResetVoxelTextureSP(pRenderDevice);
        StoreProgram(pShaderProgram);
    }

    {
        ShaderProgram* pShaderProgram = ZEN_NEW() VoxelizationCompSP(pRenderDevice);
        StoreProgram(pShaderProgram);
    }

    {
        ShaderProgram* pShaderProgram = ZEN_NEW() VoxelizationLargeTriangleCompSP(pRenderDevice);
        StoreProgram(pShaderProgram);
    }

    {
        ShaderProgram* pShaderProgram = ZEN_NEW() VoxelPreDrawSP(pRenderDevice);
        StoreProgram(pShaderProgram);
    }

    {
        ShaderProgram* pShaderProgram = ZEN_NEW() VoxelDrawSP2(pRenderDevice);
        StoreProgram(pShaderProgram);
    }

    {
        ShaderProgram* pShaderProgram = ZEN_NEW() ShadowMapRenderSP(pRenderDevice);
        StoreProgram(pShaderProgram);
    }

    {
        ShaderProgram* pShaderProgram = ZEN_NEW() VoxelInjectRadianceSP(pRenderDevice);
        StoreProgram(pShaderProgram);
    }
}
} // namespace zen::rc
