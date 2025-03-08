#pragma once
#include "Graphics/RHI/RHIResource.h"
#include "Common/HashMap.h"

namespace zen::rc
{
class RenderDevice;

class ShaderManager
{
public:
    static ShaderManager& GetInstance()
    {
        static ShaderManager instance;
        return instance;
    }

    // must be called explicitly
    void Destroy();

    static std::vector<uint8_t> LoadSpirvCode(const std::string& name);

    void LoadShaders(RenderDevice* m_renderDevice);

    rhi::ShaderHandle RequestShader(const std::string& name);

    rhi::ShaderHandle RequestShader(const std::string& name,
                                    rhi::ShaderGroupSPIRVPtr shaderGroupSpirv,
                                    RenderDevice* m_renderDevice);

    const rhi::ShaderGroupInfo& GetShaderGroupInfo(const std::string& name);

private:
    ShaderManager() = default;

    RenderDevice* m_renderDevice{nullptr};
    HashMap<std::string, rhi::ShaderHandle> m_shaderCache;
    HashMap<std::string, rhi::ShaderGroupInfo> m_sgInfoCache;
};
} // namespace zen::rc