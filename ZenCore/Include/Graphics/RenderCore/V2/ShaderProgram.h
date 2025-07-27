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

class GBufferSP : public ShaderProgram
{
public:
    explicit GBufferSP(RenderDevice* renderDevice) : ShaderProgram(renderDevice, "GBufferSP")
    {
        AddShaderStage(rhi::ShaderStage::eVertex, "SceneRenderer/offscreen.vert.spv");
        AddShaderStage(rhi::ShaderStage::eFragment, "SceneRenderer/offscreen.frag.spv");
        Init();
    }

    struct PushConstantData
    {
        uint32_t nodeIndex;
        uint32_t materialIndex;
    } pushConstantsData;
};

class DeferredLightingSP : public ShaderProgram
{
public:
    explicit DeferredLightingSP(RenderDevice* renderDevice) :
        ShaderProgram(renderDevice, "DeferredLightingSP")
    {
        AddShaderStage(rhi::ShaderStage::eVertex, "SceneRenderer/deferred.vert.spv");
        AddShaderStage(rhi::ShaderStage::eFragment, "SceneRenderer/deferred.frag.spv");
        Init();
    }
};

class EnvMapIrradianceSP : public ShaderProgram
{
public:
    explicit EnvMapIrradianceSP(RenderDevice* renderDevice) :
        ShaderProgram(renderDevice, "EnvMapIrradianceSP")
    {
        AddShaderStage(rhi::ShaderStage::eVertex, "Environment/filtercube.vert.spv");
        AddShaderStage(rhi::ShaderStage::eFragment, "Environment/irradiancecube.frag.spv");
        Init();
    }
};

class EnvMapPrefilteredSP : public ShaderProgram
{
public:
    explicit EnvMapPrefilteredSP(RenderDevice* renderDevice) :
        ShaderProgram(renderDevice, "EnvMapPrefilteredSP")
    {
        AddShaderStage(rhi::ShaderStage::eVertex, "Environment/filtercube.vert.spv");
        AddShaderStage(rhi::ShaderStage::eFragment, "Environment/prefilterenvmap.frag.spv");
        Init();
    }
};

class SkyboxRenderSP : public ShaderProgram
{
public:
    explicit SkyboxRenderSP(RenderDevice* renderDevice) :
        ShaderProgram(renderDevice, "SkyboxRenderSP")
    {
        AddShaderStage(rhi::ShaderStage::eVertex, "Environment/skybox.vert.spv");
        AddShaderStage(rhi::ShaderStage::eFragment, "Environment/skybox.frag.spv");
        Init();
    }
};

class EnvMapBRDFLutGenSP : public ShaderProgram
{
public:
    explicit EnvMapBRDFLutGenSP(RenderDevice* renderDevice) :
        ShaderProgram(renderDevice, "EnvMapBRDFLutGenSP")
    {
        AddShaderStage(rhi::ShaderStage::eVertex, "Environment/genbrdflut.vert.spv");
        AddShaderStage(rhi::ShaderStage::eFragment, "Environment/genbrdflut.frag.spv");
        Init();
    }
};

class VoxelizationSP : public ShaderProgram
{
public:
    explicit VoxelizationSP(RenderDevice* renderDevice) :
        ShaderProgram(renderDevice, "VoxelizationSP")
    {
        AddShaderStage(rhi::ShaderStage::eVertex, "VoxelGI/voxelization.vert.spv");
        AddShaderStage(rhi::ShaderStage::eGeometry, "VoxelGI/voxelization.geom.spv");
        AddShaderStage(rhi::ShaderStage::eFragment, "VoxelGI/voxelization.frag.spv");
        Init();
    }

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

class VoxelDrawSP : public ShaderProgram
{
public:
    explicit VoxelDrawSP(RenderDevice* renderDevice) : ShaderProgram(renderDevice, "VoxelDrawSP")
    {
        AddShaderStage(rhi::ShaderStage::eVertex, "VoxelGI/draw_voxels.vert.spv");
        AddShaderStage(rhi::ShaderStage::eGeometry, "VoxelGI/draw_voxels.geom.spv");
        AddShaderStage(rhi::ShaderStage::eFragment, "VoxelGI/draw_voxels.frag.spv");
        Init();
    }

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

class VoxelizationCompSP : public ShaderProgram
{
public:
    explicit VoxelizationCompSP(RenderDevice* renderDevice) :
        ShaderProgram(renderDevice, "VoxelizationCompSP")
    {
        AddShaderStage(rhi::ShaderStage::eCompute, "VoxelGI/voxelization.comp.spv");
        Init();
    }

    const uint8_t* GetSceneInfoData() const
    {
        return reinterpret_cast<const uint8_t*>(&sceneInfo);
    }

    struct SceneInfo
    {
        Vec4 aabbMin;
        Vec4 aabbMax;
    } sceneInfo;

    struct PushConstantData
    {
        uint32_t nodeIndex;
        uint32_t triangleCount;
        uint32_t largeTriangleThreshold;
    } pushConstantsData;
};

class VoxelizationLargeTriangleCompSP : public ShaderProgram
{
public:
    explicit VoxelizationLargeTriangleCompSP(RenderDevice* renderDevice) :
        ShaderProgram(renderDevice, "VoxelizationLargeTriangleCompSP")
    {
        AddShaderStage(rhi::ShaderStage::eCompute, "VoxelGI/voxelization_large_triangles.comp.spv");
        Init();
    }

    const uint8_t* GetSceneInfoData() const
    {
        return reinterpret_cast<const uint8_t*>(&sceneInfo);
    }

    struct SceneInfo
    {
        Vec4 aabbMin;
        Vec4 aabbMax;
    } sceneInfo;

    struct PushConstantData
    {
        uint32_t nodeIndex;
        uint32_t triangleCount;
        uint32_t largeTriangleThreshold;
    } pushConstantsData;
};

class ResetDrawIndirectSP : public ShaderProgram
{
public:
    explicit ResetDrawIndirectSP(RenderDevice* renderDevice) :
        ShaderProgram(renderDevice, "ResetDrawIndirectSP")
    {
        AddShaderStage(rhi::ShaderStage::eCompute, "VoxelGI/reset_draw_indirect.comp.spv");
        Init();
    }
};

class ResetComputeIndirectSP : public ShaderProgram
{
public:
    explicit ResetComputeIndirectSP(RenderDevice* renderDevice) :
        ShaderProgram(renderDevice, "ResetComputeIndirectSP")
    {
        AddShaderStage(rhi::ShaderStage::eCompute, "VoxelGI/reset_compute_indirect.comp.spv");
        Init();
    }
};

class ResetVoxelTextureSP : public ShaderProgram
{
public:
    explicit ResetVoxelTextureSP(RenderDevice* renderDevice) :
        ShaderProgram(renderDevice, "ResetVoxelTextureSP")
    {
        AddShaderStage(rhi::ShaderStage::eCompute, "VoxelGI/reset_voxel_texture.comp.spv");
        Init();
    }
};

class VoxelPreDrawSP : public ShaderProgram
{
public:
    explicit VoxelPreDrawSP(RenderDevice* renderDevice) :
        ShaderProgram(renderDevice, "VoxelPreDrawSP")
    {
        AddShaderStage(rhi::ShaderStage::eCompute, "VoxelGI/voxel_pre_draw.comp.spv");
        Init();
    }


    const uint8_t* GetSceneInfoData() const
    {
        return reinterpret_cast<const uint8_t*>(&sceneInfo);
    }

    struct SceneInfo
    {
        Vec4 aabbMin;
        Vec4 aabbMax;
    } sceneInfo;
};

class VoxelDrawSP2 : public ShaderProgram
{
public:
    explicit VoxelDrawSP2(RenderDevice* renderDevice) : ShaderProgram(renderDevice, "VoxelDrawSP2")
    {
        AddShaderStage(rhi::ShaderStage::eVertex, "VoxelGI/voxel_vis.vert.spv");
        AddShaderStage(rhi::ShaderStage::eFragment, "VoxelGI/voxel_vis.frag.spv");
        Init();
    }

    const uint8_t* GetTransformData() const
    {
        return reinterpret_cast<const uint8_t*>(&transformData);
    }
    struct TransformData
    {
        Mat4 modelMatrix;
        Mat4 viewMatrix;
        Mat4 projMatrix;
    } transformData;
};

class ShadowMapRenderSP : public ShaderProgram
{
public:
    explicit ShadowMapRenderSP(RenderDevice* renderDevice) :
        ShaderProgram(renderDevice, "ShadowMapRenderSP")
    {
        AddShaderStage(rhi::ShaderStage::eVertex, "ShadowMapping/evsm.vert.spv");
        AddShaderStage(rhi::ShaderStage::eFragment, "ShadowMapping/evsm.frag.spv");
        Init();
    }


    const uint8_t* GetLightInfoData() const
    {
        return reinterpret_cast<const uint8_t*>(&lightInfo);
    }

    struct PushConstantData
    {
        Vec2 exponents;
        uint32_t nodeIndex;
        uint32_t materialIndex;
        float alphaCutoff;
    } pushConstantsData;

    struct LightInfo
    {
        Mat4 lightViewProjection;
    } lightInfo;
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