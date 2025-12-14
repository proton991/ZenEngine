#pragma once
#include <utility>

#include "Graphics/RHI/RHIResource.h"
#include "Templates/HashMap.h"

namespace zen::rc
{
class RenderDevice;

// refactor: move to RenderScene, create sg::Light
struct Attenuation
{
    float constant{0.0f};
    float linear{0.0f};
    float quadratic{0.0f};
    float _padding{0.0f}; // padding
};

struct Light
{
    glm::vec3 position{0.0f};
    float angleInnerCone{0.0f};

    glm::vec3 direction{0.0f};
    float angleOuterCone{0.0f};

    glm::vec3 diffuse{0.0f};
    uint32_t shadowingMethod{0};

    Attenuation attenuation;
};

class ShaderProgram
{
public:
    ShaderProgram(RenderDevice* renderDevice, std::string name);

    virtual ~ShaderProgram();

    const std::string& GetName() const
    {
        return m_name;
    }

    RHIShader* GetShader() const
    {
        return m_shader;
    }

    const RHIShaderResourceDescriptorTable& GetSRDTable() const
    {
        return m_SRDTable;
    }

    uint32_t GetNumDescriptorSets() const
    {
        return m_SRDTable.size();
    }

    RHIBuffer* GetUniformBufferHandle(const std::string& name)
    {
        VERIFY_EXPR(m_uniformBufferMap.contains(name) != false);

        return m_uniformBufferMap[name];
    }

    void UpdateUniformBuffer(const std::string& name, const uint8_t* data, uint32_t offset);

    const auto& GetUniformBufferSRDs() const
    {
        return m_uniformBuffers;
    }

    const auto& GetStorageBufferSRDs() const
    {
        return m_storageBuffers;
    }

    const auto& GetSampledTextureSRDs() const
    {
        return m_sampledTextures;
    }

    const auto& GetStorageImageSRDs() const
    {
        return m_storageImages;
    }

protected:
    void Init();

    void Init(const HashMap<uint32_t, int>& m_specializationConstants);

    void AddShaderStage(RHIShaderStage stage, const std::string& path)
    {
        m_stages[stage] = path;
    }

private:
    RenderDevice* m_renderDevice{nullptr};
    std::string m_name;
    HashMap<RHIShaderStage, std::string> m_stages; // stage -> path
    RHIShaderResourceDescriptorTable m_SRDTable;
    RHIShader* m_shader;

    HashMap<std::string, RHIBuffer*> m_uniformBufferMap; // created from SRDs
    HashMap<std::string, uint32_t> m_uniformBufferSizes;      // created from SRDs

    std::vector<RHIShaderResourceDescriptor> m_uniformBuffers;
    std::vector<RHIShaderResourceDescriptor> m_storageBuffers;
    std::vector<RHIShaderResourceDescriptor> m_sampledTextures;
    std::vector<RHIShaderResourceDescriptor> m_storageImages;

    friend class GraphicsPassBuilder;
};

class GBufferSP : public ShaderProgram
{
public:
    explicit GBufferSP(RenderDevice* renderDevice) : ShaderProgram(renderDevice, "GBufferSP")
    {
        AddShaderStage(RHIShaderStage::eVertex, "SceneRenderer/offscreen.vert.spv");
        AddShaderStage(RHIShaderStage::eFragment, "SceneRenderer/offscreen.frag.spv");
        Init();
    }

    struct PushConstantsData
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
        AddShaderStage(RHIShaderStage::eVertex, "SceneRenderer/deferred.vert.spv");
        AddShaderStage(RHIShaderStage::eFragment, "SceneRenderer/deferred.frag.spv");
        Init();
    }
};

class EnvMapIrradianceSP : public ShaderProgram
{
public:
    explicit EnvMapIrradianceSP(RenderDevice* renderDevice) :
        ShaderProgram(renderDevice, "EnvMapIrradianceSP")
    {
        AddShaderStage(RHIShaderStage::eVertex, "Environment/filtercube.vert.spv");
        AddShaderStage(RHIShaderStage::eFragment, "Environment/irradiancecube.frag.spv");
        Init();
    }
};

class EnvMapPrefilteredSP : public ShaderProgram
{
public:
    explicit EnvMapPrefilteredSP(RenderDevice* renderDevice) :
        ShaderProgram(renderDevice, "EnvMapPrefilteredSP")
    {
        AddShaderStage(RHIShaderStage::eVertex, "Environment/filtercube.vert.spv");
        AddShaderStage(RHIShaderStage::eFragment, "Environment/prefilterenvmap.frag.spv");
        Init();
    }
};

class SkyboxRenderSP : public ShaderProgram
{
public:
    explicit SkyboxRenderSP(RenderDevice* renderDevice) :
        ShaderProgram(renderDevice, "SkyboxRenderSP")
    {
        AddShaderStage(RHIShaderStage::eVertex, "Environment/skybox.vert.spv");
        AddShaderStage(RHIShaderStage::eFragment, "Environment/skybox.frag.spv");
        Init();
    }
};

class EnvMapBRDFLutGenSP : public ShaderProgram
{
public:
    explicit EnvMapBRDFLutGenSP(RenderDevice* renderDevice) :
        ShaderProgram(renderDevice, "EnvMapBRDFLutGenSP")
    {
        AddShaderStage(RHIShaderStage::eVertex, "Environment/genbrdflut.vert.spv");
        AddShaderStage(RHIShaderStage::eFragment, "Environment/genbrdflut.frag.spv");
        Init();
    }
};

class VoxelizationSP : public ShaderProgram
{
public:
    explicit VoxelizationSP(RenderDevice* renderDevice) :
        ShaderProgram(renderDevice, "VoxelizationSP")
    {
        AddShaderStage(RHIShaderStage::eVertex, "VoxelGI/voxelization.vert.spv");
        AddShaderStage(RHIShaderStage::eGeometry, "VoxelGI/voxelization.geom.spv");
        AddShaderStage(RHIShaderStage::eFragment, "VoxelGI/voxelization.frag.spv");
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

    struct PushConstantsData
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
        AddShaderStage(RHIShaderStage::eVertex, "VoxelGI/draw_voxels.vert.spv");
        AddShaderStage(RHIShaderStage::eGeometry, "VoxelGI/draw_voxels.geom.spv");
        AddShaderStage(RHIShaderStage::eFragment, "VoxelGI/draw_voxels.frag.spv");
        Init();
    }

    const uint8_t* GetVoxelInfoData() const
    {
        return reinterpret_cast<const uint8_t*>(&voxelInfo);
    }

    struct PushConstantsData
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
        AddShaderStage(RHIShaderStage::eCompute, "VoxelGI/voxelization.comp.spv");
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

    struct PushConstantsData
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
        AddShaderStage(RHIShaderStage::eCompute,
                       "VoxelGI/voxelization_large_triangles.comp.spv");
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

    struct PushConstantsData
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
        AddShaderStage(RHIShaderStage::eCompute, "VoxelGI/reset_draw_indirect.comp.spv");
        Init();
    }
};

class ResetComputeIndirectSP : public ShaderProgram
{
public:
    explicit ResetComputeIndirectSP(RenderDevice* renderDevice) :
        ShaderProgram(renderDevice, "ResetComputeIndirectSP")
    {
        AddShaderStage(RHIShaderStage::eCompute, "VoxelGI/reset_compute_indirect.comp.spv");
        Init();
    }
};

class ResetVoxelTextureSP : public ShaderProgram
{
public:
    explicit ResetVoxelTextureSP(RenderDevice* renderDevice) :
        ShaderProgram(renderDevice, "ResetVoxelTextureSP")
    {
        AddShaderStage(RHIShaderStage::eCompute, "VoxelGI/reset_voxel_texture.comp.spv");
        Init();
    }
};

class VoxelPreDrawSP : public ShaderProgram
{
public:
    explicit VoxelPreDrawSP(RenderDevice* renderDevice) :
        ShaderProgram(renderDevice, "VoxelPreDrawSP")
    {
        AddShaderStage(RHIShaderStage::eCompute, "VoxelGI/voxel_pre_draw.comp.spv");
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
        AddShaderStage(RHIShaderStage::eVertex, "VoxelGI/voxel_vis.vert.spv");
        AddShaderStage(RHIShaderStage::eFragment, "VoxelGI/voxel_vis.frag.spv");
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

const uint32_t MAX_DIRECTIONAL_LIGHTS = 3;
const uint32_t MAX_POINT_LIGHTS       = 6;
const uint32_t MAX_SPOT_LIGHTS        = 6;

enum LightType
{
    DIRECTIONAL_LIGHT = 0,
    POINT_LIGHT       = 1,
    SPOT_LIGHT        = 2,
};
class VoxelInjectRadianceSP : public ShaderProgram
{
public:
    explicit VoxelInjectRadianceSP(RenderDevice* renderDevice) :
        ShaderProgram(renderDevice, "VoxelInjectRadianceSP")
    {
        AddShaderStage(RHIShaderStage::eCompute, "VoxelGI/inject_radiance.comp.spv");
        Init();
    }

    // const uint8_t* GetSceneInfoData() const
    // {
    //     return reinterpret_cast<const uint8_t*>(&sceneInfo);
    // }

    const uint8_t* GetLightInfoData() const
    {
        return reinterpret_cast<const uint8_t*>(&lightInfo);
    }

    // struct SceneInfo
    // {
    //     float voxelSize;
    //     float voxelScale;
    //     Vec3 worldMinPoint;
    //     int volumeDimension;
    // } sceneInfo;

    struct LightInfo
    {
        Light directionalLight[MAX_DIRECTIONAL_LIGHTS];
        Light pointLight[MAX_POINT_LIGHTS];
        Light spotLight[MAX_SPOT_LIGHTS];

        // uint32_t lightTypeCount[3]{};
        Mat4 lightViewProjection{1.0f};

        float lightBleedingReduction{0.0f};
        Vec2 exponents{0.0f};
        float _padding{0.0f};
    } lightInfo;

    struct PushConstantsData
    {
        float traceShadowHit;
        uint32_t normalWeightedLambert;
        float voxelSize;
        float voxelScale;
        Vec3 worldMinPoint;
        int volumeDimension;
    } pushConstantsData;
};

class ShadowMapRenderSP : public ShaderProgram
{
public:
    explicit ShadowMapRenderSP(RenderDevice* renderDevice) :
        ShaderProgram(renderDevice, "ShadowMapRenderSP")
    {
        AddShaderStage(RHIShaderStage::eVertex, "ShadowMapping/evsm.vert.spv");
        AddShaderStage(RHIShaderStage::eFragment, "ShadowMapping/evsm.frag.spv");
        Init();
    }


    const uint8_t* GetLightInfoData() const
    {
        return reinterpret_cast<const uint8_t*>(&lightInfo);
    }

    struct PushConstantsData
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