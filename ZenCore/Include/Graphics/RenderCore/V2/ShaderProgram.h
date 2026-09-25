#pragma once
#include "Graphics/RHI/RHIResource.h"
#include "Templates/HashMap.h"
#include "Templates/NameID.h"

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
    ShaderProgram(RenderDevice* pRenderDevice, NameID name);

    virtual ~ShaderProgram();

    NameID GetName() const
    {
        return m_name;
    }

    RHIShader* GetShader() const
    {
        return m_pShader;
    }

    const RHIShaderResourceDescriptorTable* GetSRDTable() const
    {
        return m_pShader->GetSRDTable();
    }

    uint32_t GetNumDescriptorSets() const
    {
        return m_pShader->GetSRDTable()->size();
    }

    RHIBuffer* GetUniformBufferHandle(NameID name)
    {
        VERIFY_EXPR(m_uniformBufferMap.contains(name) != false);

        return m_uniformBufferMap[name];
    }

    void UpdateUniformBuffer(NameID name, const uint8_t* pData, uint32_t offset);

    const HeapVector<RHIShaderResourceDescriptor>& GetStorageBufferSRDs() const
    {
        return m_storageBuffers;
    }

    const HeapVector<RHIShaderResourceDescriptor>& GetSampledTextureSRDs() const
    {
        return m_sampledTextures;
    }

    const HeapVector<RHIShaderResourceDescriptor>& GetStorageImageSRDs() const
    {
        return m_storageImages;
    }

    const RHIShaderResourceDescriptor* GetShaderResourceDescriptor(NameID name) const
    {
        return m_namedSRDLut.contains(name) != true ? nullptr : m_namedSRDLut.at(name);
    }

    // Reinitialization replaces the RHI shader only after creation succeeds.
    bool Init();

    bool Init(const HashMap<uint32_t, int>& specializationConstants);

protected:
    void ResolveShaderResources();

    void AddShaderStage(RHIShaderStage stage, const std::string& path)
    {
        m_stageSources[ToUnderlying(stage)] = path;
        m_stageFlags.SetFlag(static_cast<RHIShaderStageFlagBits>(1 << ToUnderlying(stage)));
    }

private:
    RenderDevice* m_pRenderDevice{nullptr};
    NameID m_name;
    std::string m_stageSources[ToUnderlying(RHIShaderStage::eMax)];
    BitField<RHIShaderStageFlagBits> m_stageFlags;

    RHIShader* m_pShader{nullptr};

    HashMap<NameID, RHIBuffer*> m_uniformBufferMap; // created from SRDs

    HeapVector<RHIShaderResourceDescriptor> m_storageBuffers;
    HeapVector<RHIShaderResourceDescriptor> m_sampledTextures;
    HeapVector<RHIShaderResourceDescriptor> m_storageImages;

    HashMap<NameID, const RHIShaderResourceDescriptor*> m_namedSRDLut;

    friend class GraphicsPassBuilder;
};

class ComputeFileSP : public ShaderProgram
{
public:
    ComputeFileSP(RenderDevice* device,
                  NameID name,
                  const std::string& path,
                  const HashMap<uint32_t, int>& specializationConstants = {}) :
        ShaderProgram(device, name)
    {
        AddShaderStage(RHIShaderStage::eCompute, path);
        Init(specializationConstants);
    }
};

class GBufferSP : public ShaderProgram
{
public:
    explicit GBufferSP(RenderDevice* pRenderDevice, bool dynamicGI = false) :
        ShaderProgram(pRenderDevice, dynamicGI ? "GBufferDynamicSP" : "GBufferSP")
    {
        AddShaderStage(RHIShaderStage::eVertex, "SceneRenderer/offscreen.vert.spv");
        AddShaderStage(RHIShaderStage::eFragment,
                       dynamicGI ? "SceneRenderer/offscreen_dynamic.frag.spv" :
                                   "SceneRenderer/offscreen.frag.spv");
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
    explicit DeferredLightingSP(RenderDevice* pRenderDevice, bool capture = false) :
        ShaderProgram(pRenderDevice, capture ? "DeferredLightingCaptureSP" : "DeferredLightingSP")
    {
        AddShaderStage(RHIShaderStage::eVertex, "SceneRenderer/deferred.vert.spv");
        AddShaderStage(RHIShaderStage::eFragment,
                       capture ? "SceneRenderer/deferred_capture.frag.spv" :
                                 "SceneRenderer/deferred.frag.spv");
        Init();
    }
};

class DeferredVoxelGISP : public ShaderProgram
{
public:
    explicit DeferredVoxelGISP(RenderDevice* device, bool capture = false) :
        ShaderProgram(device, capture ? "DeferredVoxelGICaptureSP" : "DeferredVoxelGISP")
    {
        AddShaderStage(RHIShaderStage::eVertex, "SceneRenderer/deferred.vert.spv");
        AddShaderStage(RHIShaderStage::eFragment,
                       capture ? "SceneRenderer/voxel_gi_capture.frag.spv" :
                                 "SceneRenderer/voxel_gi.frag.spv");
        Init();
    }
};

class DeferredDynamicVoxelGISP : public ShaderProgram
{
public:
    explicit DeferredDynamicVoxelGISP(RenderDevice* device, bool capture = false) :
        ShaderProgram(device,
                      capture ? "DeferredDynamicVoxelGICaptureSP" : "DeferredDynamicVoxelGISP")
    {
        AddShaderStage(RHIShaderStage::eVertex, "SceneRenderer/deferred.vert.spv");
        AddShaderStage(RHIShaderStage::eFragment,
                       capture ? "SceneRenderer/dynamic_voxel_gi_capture.frag.spv" :
                                 "SceneRenderer/dynamic_voxel_gi.frag.spv");
        Init();
    }
};

class SceneShadowSP : public ShaderProgram
{
public:
    explicit SceneShadowSP(RenderDevice* device) : ShaderProgram(device, "SceneShadowSP")
    {
        AddShaderStage(RHIShaderStage::eVertex, "ShadowMapping/scene_shadow.vert.spv");
        AddShaderStage(RHIShaderStage::eFragment, "ShadowMapping/scene_shadow.frag.spv");
        Init();
    }
};

class LightMarkerSP : public ShaderProgram
{
public:
    explicit LightMarkerSP(RenderDevice* device) : ShaderProgram(device, "LightMarkerSP")
    {
        AddShaderStage(RHIShaderStage::eVertex, "SceneRenderer/light_marker.vert.spv");
        AddShaderStage(RHIShaderStage::eFragment, "SceneRenderer/light_marker.frag.spv");
        Init();
    }
};

class EnvMapIrradianceSP : public ShaderProgram
{
public:
    explicit EnvMapIrradianceSP(RenderDevice* pRenderDevice) :
        ShaderProgram(pRenderDevice, "EnvMapIrradianceSP")
    {
        AddShaderStage(RHIShaderStage::eVertex, "Environment/filtercube.vert.spv");
        AddShaderStage(RHIShaderStage::eFragment, "Environment/irradiancecube.frag.spv");
        Init();
    }
};

class EnvMapPrefilteredSP : public ShaderProgram
{
public:
    explicit EnvMapPrefilteredSP(RenderDevice* pRenderDevice) :
        ShaderProgram(pRenderDevice, "EnvMapPrefilteredSP")
    {
        AddShaderStage(RHIShaderStage::eVertex, "Environment/filtercube.vert.spv");
        AddShaderStage(RHIShaderStage::eFragment, "Environment/prefilterenvmap.frag.spv");
        Init();
    }
};

class SkyboxRenderSP : public ShaderProgram
{
public:
    explicit SkyboxRenderSP(RenderDevice* pRenderDevice) :
        ShaderProgram(pRenderDevice, "SkyboxRenderSP")
    {
        AddShaderStage(RHIShaderStage::eVertex, "Environment/skybox.vert.spv");
        AddShaderStage(RHIShaderStage::eFragment, "Environment/skybox.frag.spv");
        Init();
    }
};

class EnvMapBRDFLutGenSP : public ShaderProgram
{
public:
    explicit EnvMapBRDFLutGenSP(RenderDevice* pRenderDevice) :
        ShaderProgram(pRenderDevice, "EnvMapBRDFLutGenSP")
    {
        AddShaderStage(RHIShaderStage::eVertex, "Environment/genbrdflut.vert.spv");
        AddShaderStage(RHIShaderStage::eFragment, "Environment/genbrdflut.frag.spv");
        Init();
    }
};

class VoxelizationSP : public ShaderProgram
{
public:
    explicit VoxelizationSP(RenderDevice* pRenderDevice, bool averagedReflectance = false) :
        ShaderProgram(pRenderDevice,
                      averagedReflectance ? "VoxelizationAveragedSP" : "VoxelizationSP")
    {
        AddShaderStage(RHIShaderStage::eVertex, "VoxelGI/voxelization.vert.spv");
        AddShaderStage(RHIShaderStage::eGeometry, "VoxelGI/voxelization.geom.spv");
        AddShaderStage(RHIShaderStage::eFragment,
                       averagedReflectance ? "VoxelGI/voxelization_averaged.frag.spv" :
                                             "VoxelGI/voxelization.frag.spv");
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
        uint32_t firstTriangle;
        uint32_t volumeDimension;
    } pushConstantsData;
};

class VoxelDrawSP : public ShaderProgram
{
public:
    explicit VoxelDrawSP(RenderDevice* pRenderDevice) : ShaderProgram(pRenderDevice, "VoxelDrawSP")
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
    explicit VoxelizationCompSP(RenderDevice* pRenderDevice) :
        ShaderProgram(pRenderDevice, "VoxelizationCompSP")
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
    explicit VoxelizationLargeTriangleCompSP(RenderDevice* pRenderDevice) :
        ShaderProgram(pRenderDevice, "VoxelizationLargeTriangleCompSP")
    {
        AddShaderStage(RHIShaderStage::eCompute, "VoxelGI/voxelization_large_triangles.comp.spv");
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
    explicit ResetDrawIndirectSP(RenderDevice* pRenderDevice) :
        ShaderProgram(pRenderDevice, "ResetDrawIndirectSP")
    {
        AddShaderStage(RHIShaderStage::eCompute, "VoxelGI/reset_draw_indirect.comp.spv");
        Init();
    }
};

class ResetComputeIndirectSP : public ShaderProgram
{
public:
    explicit ResetComputeIndirectSP(RenderDevice* pRenderDevice) :
        ShaderProgram(pRenderDevice, "ResetComputeIndirectSP")
    {
        AddShaderStage(RHIShaderStage::eCompute, "VoxelGI/reset_compute_indirect.comp.spv");
        Init();
    }
};

class ResetVoxelTextureSP : public ShaderProgram
{
public:
    explicit ResetVoxelTextureSP(RenderDevice* pRenderDevice) :
        ShaderProgram(pRenderDevice, "ResetVoxelTextureSP")
    {
        AddShaderStage(RHIShaderStage::eCompute, "VoxelGI/reset_voxel_texture.comp.spv");
        Init();
    }
};

class VoxelPreDrawSP : public ShaderProgram
{
public:
    explicit VoxelPreDrawSP(RenderDevice* pRenderDevice,
                            const HashMap<uint32_t, int>& specializationConstants = {}) :
        ShaderProgram(pRenderDevice, "VoxelPreDrawSP")
    {
        AddShaderStage(RHIShaderStage::eCompute, "VoxelGI/voxel_pre_draw.comp.spv");
        Init(specializationConstants);
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
    explicit VoxelDrawSP2(RenderDevice* pRenderDevice) :
        ShaderProgram(pRenderDevice, "VoxelDrawSP2")
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

class ShadowMapRenderSP : public ShaderProgram
{
public:
    explicit ShadowMapRenderSP(RenderDevice* pRenderDevice) :
        ShaderProgram(pRenderDevice, "ShadowMapRenderSP")
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

    ShaderProgram* CreateShaderProgram(RenderDevice* pRenderDevice, NameID name);

    void Destroy();

    void BuildShaderPrograms(RenderDevice* pRenderDevice);

    ShaderProgram* RequestShaderProgram(NameID name)
    {
        return m_programCache.contains(name) ? m_programCache[name] : nullptr;
    }

private:
    ShaderProgramManager()
    {
        m_programCache = {};
    }

    void StoreProgram(ShaderProgram* program);

    HashMap<NameID, ShaderProgram*> m_programCache;
};
} // namespace zen::rc
