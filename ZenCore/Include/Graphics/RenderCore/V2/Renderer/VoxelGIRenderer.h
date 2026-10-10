#pragma once
#include "Graphics/RenderCore/V2/RenderGraph/RenderGraph.h"
#include "Math/Math.h"
#include "Graphics/RenderCore/V2/SceneRayQuery.h"
#include "SceneGraph/AABB.h"

namespace zen::platform
{
class ConfigLoader;
}

namespace zen::rc
{
class RenderDevice;
class RenderScene;
class VoxelizerBase;
class SceneShadowRenderer;

struct VoxelGISettings
{
    enum class RayProvider : uint32_t
    {
        Auto,
        Voxel,
        Hardware,
        Legacy
    };
    RayProvider rayProvider{RayProvider::Auto};
    enum class BounceSource : uint32_t
    {
        Auto, // Ray hits with hardware queries (P5 passed on the RT tier); cones on the compute tier until P8.
        Cone,
        Rays
    };
    BounceSource bounceSource{BounceSource::Auto};
    uint32_t     samples{4};
    uint32_t     accelerationStructureBudgetMB{0}; // Explicit query allocation cap; zero leaves the P8 budget unset.
    uint32_t     referenceSamples{0};              // Diagnostic: 0, 1024 or 4096; static running mean, no spatial filter.
    uint32_t     historyFrames{32};
    bool         temporal{true};
    bool         filter{true};
    bool         specularOcclusion{true};
    float        indirectIntensity{1.0f};
    float        coneAngleDegrees{60.0f};
    float        stepScale{1.0f};
    float        normalBiasVoxels{1.5f};
    float        maxDistanceGridLengths{1.7321f};
    uint32_t     coneCount{6};
    uint32_t     maxSteps{128};
    bool         shadows{true};
    bool         analyticLighting{true};
    bool         environmentLighting{true};
    bool         emissiveLighting{true};

    bool operator==(const VoxelGISettings&) const = default;
};

bool ValidateVoxelGISettings(const VoxelGISettings& settings);

bool LoadVoxelGISettings(const platform::ConfigLoader& config, VoxelGISettings& output);

struct VoxelGIUniformData
{
    Vec4 gridMinVoxelSize;
    Vec4 volume;
    Vec4 cone;
    Vec4 limits;
    Vec4 lighting{1.0f, 1.0f, 1.0f, 0.0f}; // Analytic, environment, emissive contributions; ray-hit bounce enabled.
};
static_assert(sizeof(VoxelGIUniformData) == 80);

// Conservative base-level occupied-cell range, with an exclusive upper bound.
struct VoxelGIVisibilityBounds
{
    Vec4 minimum;
    Vec4 maximum;
};
static_assert(sizeof(VoxelGIVisibilityBounds) == 32);

VoxelGIVisibilityBounds BuildVoxelGIVisibilityBounds(const sg::AABB& geometryBounds,
                                                     const Vec4&     gridMinimumSize,
                                                     uint32_t        resolution);

class VoxelGIRenderer
{
public:
    VoxelGIRenderer(RenderDevice* device, VoxelizerBase* voxelizer);

    bool Init();

    void BuildRenderGraph(SceneShadowRenderer* shadows = nullptr);

    void OnRenderGraphExecuted(bool succeeded);

    void SetRenderScene(RenderScene* scene);

    void Destroy();

    bool SetSettings(const VoxelGISettings& settings);

    const VoxelGISettings& GetSettings() const
    {
        return m_settings;
    }

    void BindLightingInputs(RDGPassDescBase& pass) const;
    void BindRayInputs(RDGPassDescBase& pass) const;
    void BindHitRadianceInputs(RDGPassDescBase& pass) const;

    uint64_t GetRadianceGeneration() const
    {
        return m_radianceGeneration + (m_recordedRadiance ? 1 : 0);
    }

    bool UsesHardwareQueries() const
    {
        return m_hardwareQueries;
    }
    // The resolved bounce source; valid once BuildRenderGraph has selected the provider.
    bool UsesRayBounce() const
    {
        return m_settings.bounceSource == VoxelGISettings::BounceSource::Rays
            || (m_settings.bounceSource == VoxelGISettings::BounceSource::Auto && m_hardwareQueries);
    }
    const char* GetProviderReason() const
    {
        return m_providerReason;
    }
    const SceneRayQuery& GetSceneRayQuery() const
    {
        return m_rayQuery;
    }
    void BindHardwareRayInputs(RDGPassDescBase& pass) const;

    void BindEnvironmentSamplingInputs(RDGPassDescBase& pass) const;

    // Order-2 spherical-harmonic projection of the sampled environment (EnvironmentSampling.h).
    void BindEnvironmentHarmonics(RDGPassDescBase& pass) const;

    bool IsInitialized() const
    {
        return m_radiance != nullptr && m_skyIrradiance != nullptr && m_environmentColumns != nullptr
            && m_environmentRows != nullptr && m_environmentHarmonics != nullptr && m_environmentLuminance != nullptr
            && m_environmentTiles != nullptr && m_environmentSources != nullptr && m_environmentResidualColumns != nullptr
            && m_environmentResidualRows != nullptr;
    }

    RHITexture* GetRadianceTexture() const
    {
        return m_radiance;
    }

    // RGB is irradiance. Alpha records 0 inactive, 1 owner surface, 2 center fallback.
    RHITexture* GetSkyIrradianceTexture() const
    {
        return m_skyIrradiance;
    }

private:
    void LoadSettings();

    void BuildMipChain(const HeapVector<RHITextureView*>& views, NameID program, const char* tag);

    bool PrepareMipViews(RHITexture* texture, HeapVector<RHITextureView*>& views);

    void BindFrameData(RDGPassDescBase& pass) const;

    void BuildEnvironmentDistribution();

    SceneRayQuery               m_rayQuery;
    bool                        m_hardwareQueries{false};
    const char*                 m_providerReason{"voxel_selected"};
    RenderDevice*               m_device{nullptr};
    RenderScene*                m_scene{nullptr};
    VoxelizerBase*              m_voxelizer{nullptr};
    RHITexture*                 m_radiance{nullptr};
    // Base level as it was before the latest injection, for ray-hit bounce history; 1x1x1 otherwise.
    RHITexture*                 m_previousRadiance{nullptr};
    RHITexture*                 m_skyIrradiance{nullptr};
    RHIBuffer*                  m_environmentColumns{nullptr};
    RHIBuffer*                  m_environmentRows{nullptr};
    RHIBuffer*                  m_environmentHarmonics{nullptr};
    RHIBuffer*                  m_environmentLuminance{nullptr};
    RHIBuffer*                  m_environmentTiles{nullptr};
    RHIBuffer*                  m_environmentSources{nullptr};
    RHIBuffer*                  m_environmentResidualColumns{nullptr};
    RHIBuffer*                  m_environmentResidualRows{nullptr};
    HeapVector<RHITextureView*> m_radianceMips;
    HeapVector<RHITextureView*> m_albedoMips;
    VoxelGISettings             m_settings;
    VoxelGIUniformData          m_uniforms{};
    VoxelGIVisibilityBounds     m_visibilityBounds{};
    uint64_t                    m_geometryRevision{0};
    uint64_t                    m_lightingRevision{0};
    uint64_t                    m_environmentRevision{0};
    uint64_t                    m_recordedGeometry{0};
    uint64_t                    m_recordedLighting{0};
    uint64_t                    m_recordedEnvironment{0};
    uint64_t                    m_radianceGeneration{0};
    bool                        m_recordedRadiance{false};
};
} // namespace zen::rc
