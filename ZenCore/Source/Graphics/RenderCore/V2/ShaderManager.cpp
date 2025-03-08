#include "Graphics/RHI/ShaderUtil.h"
#include "Graphics/RenderCore/V2/ShaderManager.h"
#include "Graphics/RenderCore/V2/RenderDevice.h"
#include "Common/Errors.h"
#include <fstream>

namespace zen::rc
{
std::vector<uint8_t> ShaderManager::LoadSpirvCode(const std::string& name)
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

void ShaderManager::Destroy()
{
    if (m_renderDevice != nullptr)
    {
        for (auto& kv : m_shaderCache)
        {
            m_renderDevice->GetRHI()->DestroyShader(kv.second);
        }
    }
}

// todo: load shaders based on config file (xml, json etc.)
void ShaderManager::LoadShaders(RenderDevice* renderDevice)
{
    m_renderDevice = renderDevice;
    // deferred rendering shaders
    {
        auto shaderGroupSpirv = MakeRefCountPtr<rhi::ShaderGroupSPIRV>();
        shaderGroupSpirv->SetStageSPIRV(rhi::ShaderStage::eVertex,
                                        LoadSpirvCode("SceneRenderer/offscreen.vert.spv"));
        shaderGroupSpirv->SetStageSPIRV(rhi::ShaderStage::eFragment,
                                        LoadSpirvCode("SceneRenderer/offscreen.frag.spv"));
        RequestShader("DeferredPBR", shaderGroupSpirv, renderDevice);
    }
    // env map irradiance shaders
    {
        auto shaderGroupSpirv = MakeRefCountPtr<rhi::ShaderGroupSPIRV>();
        shaderGroupSpirv->SetStageSPIRV(rhi::ShaderStage::eVertex,
                                        LoadSpirvCode("Environment/filtercube.vert.spv"));
        shaderGroupSpirv->SetStageSPIRV(rhi::ShaderStage::eFragment,
                                        LoadSpirvCode("Environment/irradiancecube.frag.spv"));
        RequestShader("EnvMapIrradiance", shaderGroupSpirv, renderDevice);
    }
    // env map prefiltered shaders
    {
        auto shaderGroupSpirv = MakeRefCountPtr<rhi::ShaderGroupSPIRV>();
        shaderGroupSpirv->SetStageSPIRV(rhi::ShaderStage::eVertex,
                                        LoadSpirvCode("Environment/filtercube.vert.spv"));
        shaderGroupSpirv->SetStageSPIRV(rhi::ShaderStage::eFragment,
                                        LoadSpirvCode("Environment/prefilterenvmap.frag.spv"));
        RequestShader("EnvMapPrefiltered", shaderGroupSpirv, renderDevice);
    }
    // env map brdf lut gen
    {
        auto shaderGroupSpirv = MakeRefCountPtr<rhi::ShaderGroupSPIRV>();
        shaderGroupSpirv->SetStageSPIRV(rhi::ShaderStage::eVertex,
                                        LoadSpirvCode("Environment/genbrdflut.vert.spv"));
        shaderGroupSpirv->SetStageSPIRV(rhi::ShaderStage::eFragment,
                                        LoadSpirvCode("Environment/genbrdflut.frag.spv"));
        RequestShader("EnvMapBRDFLutGen", shaderGroupSpirv, renderDevice);
    }

    // skybox render
    {
        auto shaderGroupSpirv = MakeRefCountPtr<rhi::ShaderGroupSPIRV>();
        shaderGroupSpirv->SetStageSPIRV(rhi::ShaderStage::eVertex,
                                        LoadSpirvCode("Environment/skybox.vert.spv"));
        shaderGroupSpirv->SetStageSPIRV(rhi::ShaderStage::eFragment,
                                        LoadSpirvCode("Environment/skybox.frag.spv"));
        RequestShader("SkyboxRender", shaderGroupSpirv, renderDevice);
    }
}

rhi::ShaderHandle ShaderManager::RequestShader(const std::string& name)
{
    if (!m_shaderCache.contains(name))
    {
        LOG_ERROR_AND_THROW("ShaderManager: Shader {} not in cache", name);
    }
    return m_shaderCache[name];
}

rhi::ShaderHandle ShaderManager::RequestShader(const std::string& name,
                                               rhi::ShaderGroupSPIRVPtr shaderGroupSpirv,
                                               RenderDevice* renderDevice)
{
    m_renderDevice = renderDevice;
    rhi::ShaderGroupInfo shaderGroupInfo{};
    rhi::ShaderUtil::ReflectShaderGroupInfo(shaderGroupSpirv, shaderGroupInfo);
    rhi::ShaderHandle shader = renderDevice->GetRHI()->CreateShader(shaderGroupInfo);
    // save to cache
    m_shaderCache[name] = shader;
    m_sgInfoCache[name] = shaderGroupInfo;

    return shader;
}

const rhi::ShaderGroupInfo& ShaderManager::GetShaderGroupInfo(const std::string& name)
{
    return m_sgInfoCache[name];
}
} // namespace zen::rc
  // e zen::rc