#pragma once
#include "Graphics/RenderCore/V2/RenderGraph/RenderGraph.h"
#include "Math/Math.h"

namespace zen::rc
{
class RenderDevice;
class RenderScene;
class VoxelizerBase;

struct VoxelGISettings
{
    float indirectIntensity{1.0f};
    float coneAngleDegrees{60.0f};
    float stepScale{1.0f};
    float normalBiasVoxels{1.5f};
    float maxDistanceGridLengths{1.7321f};
    uint32_t coneCount{6};
    uint32_t maxSteps{128};
    bool shadows{true};
};

struct VoxelGIUniformData
{
    Vec4 gridMinVoxelSize;
    Vec4 volume;
    Vec4 cone;
    Vec4 limits;
};
static_assert(sizeof(VoxelGIUniformData) == 64);

class VoxelGIRenderer
{
public:
    VoxelGIRenderer(RenderDevice* device, VoxelizerBase* voxelizer);
    bool Init();
    void BuildRenderGraph();
    void OnRenderGraphExecuted(bool succeeded);
    void SetRenderScene(RenderScene* scene);
    void Destroy();
    bool SetSettings(const VoxelGISettings& settings);
    const VoxelGISettings& GetSettings() const
    {
        return m_settings;
    }
    void BindLightingInputs(RDGPassDescBase& pass) const;
    bool IsInitialized() const
    {
        return m_radiance != nullptr && m_skyIrradiance != nullptr;
    }
    RHITexture* GetRadianceTexture() const
    {
        return m_radiance;
    }

private:
    void LoadSettings();
    void BuildMipChain(const HeapVector<RHITextureView*>& views, NameID program, const char* tag);
    bool PrepareMipViews(RHITexture* texture, HeapVector<RHITextureView*>& views);
    void BindFrameData(RDGPassDescBase& pass) const;

    RenderDevice* m_device{nullptr};
    RenderScene* m_scene{nullptr};
    VoxelizerBase* m_voxelizer{nullptr};
    RHITexture* m_radiance{nullptr};
    RHITexture* m_skyIrradiance{nullptr};
    HeapVector<RHITextureView*> m_radianceMips;
    HeapVector<RHITextureView*> m_albedoMips;
    VoxelGISettings m_settings;
    VoxelGIUniformData m_uniforms{};
    uint64_t m_geometryRevision{0};
    uint64_t m_lightingRevision{0};
    uint64_t m_environmentRevision{0};
    uint64_t m_recordedGeometry{0};
    uint64_t m_recordedLighting{0};
    uint64_t m_recordedEnvironment{0};
};
} // namespace zen::rc
