#pragma once
#include <utility>

#include "Graphics/RHI/RHIResource.h"
#include "Common/HashMap.h"

namespace zen::rc
{
class RenderDevice;

class ShaderProgram
{
public:
    ShaderProgram(RenderDevice* renderDevice, std::string name);

    virtual ~ShaderProgram();

    const std::string& GetName() const
    {
        return m_name;
    }

    const rhi::ShaderHandle& GetShaderHandle() const
    {
        return m_shader;
    }

    const std::vector<std::vector<rhi::ShaderResourceDescriptor>>& GetSRDs() const
    {
        return m_SRDs;
    }

    const rhi::BufferHandle& GetUniformBufferHandle(const std::string& name)
    {
        VERIFY_EXPR(m_uniformBuffers.contains(name) != false);

        return m_uniformBuffers[name];
    }

    void UpdateUniformBuffer(const std::string& name, const uint8_t* data, uint32_t offset);

protected:
    void Init();

    void Init(const HashMap<uint32_t, int>& m_specializationConstants);

    void AddShaderStage(rhi::ShaderStage stage, const std::string& path)
    {
        m_stages[stage] = path;
    }

private:
    RenderDevice* m_renderDevice{nullptr};
    std::string m_name;
    HashMap<rhi::ShaderStage, std::string> m_stages; // stage -> path
    std::vector<std::vector<rhi::ShaderResourceDescriptor>> m_SRDs;
    rhi::ShaderHandle m_shader;
    HashMap<std::string, rhi::BufferHandle> m_uniformBuffers; // created from SRDs
    HashMap<std::string, uint32_t> m_uniformBufferSizes;      // created from SRDs

    friend class GraphicsPassBuilder;
};

class GBufferShaderProgram : public ShaderProgram
{
public:
    explicit GBufferShaderProgram(RenderDevice* renderDevice) :
        ShaderProgram(renderDevice, "GBufferSP")
    {
        AddShaderStage(rhi::ShaderStage::eVertex, "SceneRenderer/offscreen.vert.spv");
        AddShaderStage(rhi::ShaderStage::eFragment, "SceneRenderer/offscreen.frag.spv");
        Init();
    }
};

class DeferredLightShaderProgram : public ShaderProgram
{
public:
    explicit DeferredLightShaderProgram(RenderDevice* renderDevice) :
        ShaderProgram(renderDevice, "DeferredLightingSP")
    {
        AddShaderStage(rhi::ShaderStage::eVertex, "SceneRenderer/deferred.vert.spv");
        AddShaderStage(rhi::ShaderStage::eFragment, "SceneRenderer/deferred.frag.spv");
        Init();
    }
};

class EnvMapIrradianceProgram : public ShaderProgram
{
public:
    explicit EnvMapIrradianceProgram(RenderDevice* renderDevice) :
        ShaderProgram(renderDevice, "EnvMapIrradianceSP")
    {
        AddShaderStage(rhi::ShaderStage::eVertex, "Environment/filtercube.vert.spv");
        AddShaderStage(rhi::ShaderStage::eFragment, "Environment/irradiancecube.frag.spv");
        Init();
    }
};

class EnvMapPrefilteredProgram : public ShaderProgram
{
public:
    explicit EnvMapPrefilteredProgram(RenderDevice* renderDevice) :
        ShaderProgram(renderDevice, "EnvMapPrefilteredSP")
    {
        AddShaderStage(rhi::ShaderStage::eVertex, "Environment/filtercube.vert.spv");
        AddShaderStage(rhi::ShaderStage::eFragment, "Environment/prefilterenvmap.frag.spv");
        Init();
    }
};

class SkyboxRenderProgram : public ShaderProgram
{
public:
    explicit SkyboxRenderProgram(RenderDevice* renderDevice) :
        ShaderProgram(renderDevice, "SkyboxRenderSP")
    {
        AddShaderStage(rhi::ShaderStage::eVertex, "Environment/skybox.vert.spv");
        AddShaderStage(rhi::ShaderStage::eFragment, "Environment/skybox.frag.spv");
        Init();
    }
};

class EnvMapBRDFLutGenProgram : public ShaderProgram
{
public:
    explicit EnvMapBRDFLutGenProgram(RenderDevice* renderDevice) :
        ShaderProgram(renderDevice, "EnvMapBRDFLutGenSP")
    {
        AddShaderStage(rhi::ShaderStage::eVertex, "Environment/genbrdflut.vert.spv");
        AddShaderStage(rhi::ShaderStage::eFragment, "Environment/genbrdflut.frag.spv");
        Init();
    }
};

class VoxelizationProgram : public ShaderProgram
{
public:
    explicit VoxelizationProgram(RenderDevice* renderDevice);

    const uint8_t* GetVoxelConfigData() const
    {
        return reinterpret_cast<const uint8_t*>(&voxelConfigData);
    }

    struct VoxelConfigData
    {
        Mat4 viewProjectionMatrices[3];  // view projection matrices for 3 axes
        Mat4 viewProjectionMatricesI[3]; // inverse view projection matrices for 3 axes
        Vec4 worldMinPointScale;
    } voxelConfigData;

    struct PushConstantData
    {
        uint32_t nodeIndex;
        uint32_t materialIndex;
        uint32_t flagStaticVoxels;
        uint32_t volumeDimension;
    } pushConstantsData;
};

class VoxelDrawShaderProgram : public ShaderProgram
{
public:
    explicit VoxelDrawShaderProgram(RenderDevice* renderDevice);

    const uint8_t* GetVoxelInfoData() const
    {
        return reinterpret_cast<const uint8_t*>(&voxelInfo);
    }

    struct PushConstantData
    {
        uint32_t volumeDimension;
        //        Vec4 colorChannels;
    } pushConstantsData;

    struct VoxelInfo
    {
        Mat4 modelViewProjection;
        Vec4 frustumPlanes[6];
        Vec4 worldMinPointVoxelSize;
    } voxelInfo;
};

class ShaderProgramManager
{
public:
    static ShaderProgramManager& GetInstance()
    {
        static ShaderProgramManager instance;
        return instance;
    }

    ShaderProgram* CreateShaderProgram(RenderDevice* renderDevice, const std::string& name);

    void Destroy();

    void BuildShaderPrograms(RenderDevice* renderDevice);

    ShaderProgram* RequestShaderProgram(const std::string& name)
    {
        return m_programCache.contains(name) ? m_programCache[name] : nullptr;
    }

private:
    ShaderProgramManager()
    {
        m_programCache = {};
    }

    HashMap<std::string, ShaderProgram*> m_programCache;
};
} // namespace zen::rc