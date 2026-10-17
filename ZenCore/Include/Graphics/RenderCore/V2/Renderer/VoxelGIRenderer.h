#pragma once
#include "Graphics/RenderCore/V2/RenderGraph/RenderGraph.h"
#include "Math/Math.h"
#include "Graphics/RenderCore/V2/SceneRayQuery.h"
#include "Graphics/RenderCore/V2/SoftwareSceneRayQuery.h"
#include "SceneGraph/AABB.h"
#include <array>

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
        Hardware
    };
    RayProvider rayProvider{RayProvider::Auto};
    // Auto is retained for old configs/capture tools only; feature controls resolve to Compute or HW RT.
    enum class Feature : uint32_t
    {
        Sky,
        SpecularOcclusion,
        SkyCache,
        Bounce,
        Reflections,
        Shadows,
        Count
    };
    std::array<RayProvider, static_cast<size_t>(Feature::Count)> featureProviders{};
    RayProvider                                                  GetFeatureProvider(Feature feature) const;
    uint32_t                                                     GetHardwareFeatureMask() const;
    bool                                                         UsesAdaptiveSampling() const;
    void                                                         SetFeatureProvider(Feature feature, RayProvider provider);
    void                                                         SetAllFeatureProviders(RayProvider provider);
    enum class BounceSource : uint32_t
    {
        Auto, // Triangle hits with either traversal backend.
        Cone,
        Rays
    };
    BounceSource bounceSource{BounceSource::Auto};
    uint32_t     samples{4};
    // Both triangle backends retain six rays for reset/dark/uncertain receivers;
    // fully settled low-variance histories use the four-ray base.
    bool     adaptiveSampling{true};
    uint32_t accelerationStructureBudgetMB{0}; // Explicit query allocation cap; zero leaves the P8 budget unset.
    uint32_t referenceSamples{0};              // Diagnostic: 0, 256, 1024 or 4096; static running mean, no spatial filter.
    uint32_t historyFrames{32};
    bool     temporal{true};
    bool     filter{true};
    bool     specularOcclusion{true};
    // Diffuse (sky, source shadows, ray-hit bounce) rays per receiver. Half traces one rotating
    // leader per 2x2 block and reconstructs the others; glossy specular stays at full resolution.
    // Opt-in: half resolution fails the frozen P0 limits on several shipping views
    // (Doc/HybridGI/NsightOptimizations.md), so full resolution remains the default.
    enum class DiffuseResolution : uint32_t
    {
        Auto, // Half with the Compute trace; full with HW RT.
        Full,
        Half
    };
    DiffuseResolution diffuseResolution{DiffuseResolution::Full};
    enum class Reflections : uint32_t
    {
        Auto, // Radiance-cache reflections with either traversal backend.
        Off,
        On
    };
    Reflections reflections{Reflections::On};
    float       indirectIntensity{1.0f};
    float       coneAngleDegrees{60.0f};
    float       stepScale{1.0f};
    float       normalBiasVoxels{1.5f};
    float       maxDistanceGridLengths{1.7321f};
    uint32_t    coneCount{6};
    uint32_t    maxSteps{128};
    bool        shadows{true};
    bool        rtShadows{true}; // Use triangle shadow rays; false explicitly selects shadow maps.
    bool        analyticLighting{true};
    bool        environmentLighting{true};
    bool        emissiveLighting{true};

    bool operator==(const VoxelGISettings&) const = default;
};

bool ValidateVoxelGISettings(const VoxelGISettings& settings);

bool LoadVoxelGISettings(const platform::ConfigLoader& config, VoxelGISettings& output, const VoxelGISettings& defaults = {});

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

    // Select/build the provider before shadow-map preparation; call once per frame before BuildRenderGraph.
    bool BuildRayQueryGraph();
    void BindSoftwareRayInputs(RDGPassDescBase& pass, bool forwardMaterialBound = false) const;

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

    bool HasPreviousRadiance() const
    {
        return m_previousRadiance != nullptr;
    }

    bool UsesHardwareQueries() const
    {
        return m_hardwareQueries;
    }
    bool UsesHardwareQueries(VoxelGISettings::Feature feature) const
    {
        return m_hardwareQueries && m_settings.GetFeatureProvider(feature) == VoxelGISettings::RayProvider::Hardware;
    }
    uint32_t GetHardwareFeatureMask() const
    {
        return m_hardwareQueries ? m_settings.GetHardwareFeatureMask() : 0;
    }
    // True when an enabled feature resolves to Compute and traces the software BVH.
    bool UsesSoftwareQueries() const;
    bool UsesHardwareShadows() const
    {
        return m_settings.shadows && m_settings.rtShadows && UsesHardwareQueries(VoxelGISettings::Feature::Shadows);
    }
    bool UsesRayShadows() const
    {
        return m_settings.shadows && m_settings.rtShadows;
    }
    // The resolved bounce source; valid once BuildRayQueryGraph has selected the provider.
    bool UsesRayBounce() const
    {
        return m_settings.bounceSource != VoxelGISettings::BounceSource::Cone;
    }
    // Resolved reflection enablement; reflections need the specular-occlusion rays they reuse.
    bool UsesReflections() const
    {
        return m_settings.specularOcclusion && m_settings.reflections != VoxelGISettings::Reflections::Off;
    }
    const char* GetProviderReason() const
    {
        return m_providerReason;
    }
    const SceneRayQuery& GetSceneRayQuery() const
    {
        return m_rayQuery;
    }
    const SoftwareSceneRayQuery& GetSoftwareRayQuery() const
    {
        return m_softwareRayQuery;
    }
    void BindHardwareRayInputs(RDGPassDescBase& pass) const;
    void BindShadowInputs(RDGPassDescBase& pass, SceneShadowRenderer* shadows, bool forwardMaterialBound = false) const;

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

    SceneRayQuery         m_rayQuery;
    SoftwareSceneRayQuery m_softwareRayQuery;
    bool                  m_hardwareQueries{false};
    const char*           m_providerReason{"software_bvh_selected"};
    RenderDevice*         m_device{nullptr};
    RenderScene*          m_scene{nullptr};
    VoxelizerBase*        m_voxelizer{nullptr};
    RHITexture*           m_radiance{nullptr};
    // Base level before the latest injection, for ray-hit bounce/reflection history; 1x1x1 otherwise.
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
