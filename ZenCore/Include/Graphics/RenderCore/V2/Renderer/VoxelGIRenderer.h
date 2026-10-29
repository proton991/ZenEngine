#pragma once
#include "Graphics/Shared/VoxelGI.h"
#include "Graphics/RenderCore/V2/RenderGraph/RenderGraph.h"
#include "Math/Math.h"
#include "Graphics/RenderCore/V2/SceneRayQuery.h"
#include "Graphics/RenderCore/V2/LightFixtureMasks.h"
#include "Graphics/RenderCore/V2/DistanceFieldScene.h"
#include "Utils/UniquePtr.h"
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
    bool                                                         UsesRayShadows(bool hardwareReady) const;
    void                                                         SetFeatureProvider(Feature feature, RayProvider provider);
    void                                                         SetAllFeatureProviders(RayProvider provider);
    enum class BounceSource : uint32_t
    {
        Auto, // Ray hits with either provider.
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
    // Opt in to one-frame voxel-radiance latency so updates can overlap receiver lighting.
    bool previousFrameRadiance{false};
    bool filter{true};
    // Indirect diffuse from GI rays or probes (sky light and bounce). Off lights diffuse from the
    // unoccluded environment, as without GI, while specular GI and direct lighting are unchanged.
    // The rays are still traced: it isolates the diffuse term rather than saving its cost.
    bool indirectDiffuse{true};
    bool specularOcclusion{true};
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
    // Compute GI rays (sky, specular, bounce, reflections and the sky cache) trace a GPU-built
    // sparse distance field covering every committed occluder (DistanceFieldScene). Cells per
    // axis: 32 or 64 (the builder's limit), each holding 8x8x8 distance samples over the scene bounds.
    uint32_t fieldResolution{64};
    // Diffuse and specular estimation: per-pixel stochastic rays with a full-resolution filter,
    // or screen probes (ScreenProbeGather) with half-resolution specular rays.
    // Reference accumulation and ray-count captures use per-pixel gather for diagnostics.
    enum class Gather : uint32_t
    {
        Pixel,
        Probes
    };
    Gather   gather{Gather::Pixel};
    uint32_t probeSpacing{16}; // Screen-probe tile side in pixels: 8 or 16.
    float    indirectIntensity{1.0f};
    float    coneAngleDegrees{60.0f};
    float    stepScale{1.0f};
    float    normalBiasVoxels{1.5f};
    float    maxDistanceGridLengths{1.7321f};
    uint32_t coneCount{6};
    uint32_t maxSteps{128};
    bool     shadows{true};
    // False forces maps. True uses HW shadow rays when HW RT is selected and ready; Compute uses maps.
    bool rtShadows{true};
    bool analyticLighting{true};
    bool environmentLighting{true};
    bool emissiveLighting{true};

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
    Vec4 indirect{1.0f, 0.0f, 0.0f, 0.0f}; // Indirect diffuse enabled; reserved.
};
static_assert(sizeof(VoxelGIUniformData) == 96);

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
    // Also records bounded distance-field construction, whether or not this frame composes GI; every
    // call needs OnVisibilityGraphExecuted with the frame's result.
    bool BuildRayQueryGraph();

    void BuildRenderGraph(SceneShadowRenderer* shadows = nullptr);

    void OnRenderGraphExecuted(RenderFrameResult result);

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
        return m_radianceGeneration + (m_recordedRadiance && !m_recordedDelayedRadiance ? 1 : 0);
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
        return (m_hardwareFeatureMask & (1u << static_cast<uint32_t>(feature))) != 0;
    }
    uint32_t GetHardwareFeatureMask() const
    {
        return m_hardwareFeatureMask;
    }
    // An enabled feature resolves to Compute while the distance field is still being built.
    bool IsPreparingField() const
    {
        return m_preparingField;
    }
    // Compute traces a published field that is still being completed: it predates committed edits, or
    // local volumes around lights are still building and those lights use the scene field.
    bool IsRefiningField() const;
    // True when an enabled GI feature resolves to Compute and traces the ready distance field.
    bool UsesComputeQueries() const;
    bool UsesFieldTraversal() const
    {
        return m_fieldActive;
    }
    // The field and, for near-light visibility of field hits, this frame's shadow maps.
    void BindFieldInputs(RDGPassDescBase& pass) const;
    // Null until Init; recreated when the field resolution changes.
    const DistanceFieldScene* GetDistanceField() const
    {
        return m_field.Get();
    }
    // Published visibility representation traced this frame: changes when a field generation is
    // published or Compute tracing starts or stops, so caches and histories built from another
    // visibility representation restart.
    uint64_t GetVisibilityGeneration() const
    {
        // Field serial, local volume revision and scene field generation; histories restart on a change.
        return m_fieldActive && m_field ? (m_fieldSerial << 48) | (uint64_t(m_field->GetLocalRevision() & 0xffffu) << 32)
                                              | (m_field->GetGeneration() & 0xffffffffu)
                                        : 0;
    }
    // Delivers this frame's result to construction work BuildRayQueryGraph recorded. Called for every
    // frame, including PBR fallback frames, before OnRenderGraphExecuted.
    void OnVisibilityGraphExecuted(RenderFrameResult result);
    bool UsesHardwareShadows() const
    {
        return m_settings.shadows && m_settings.rtShadows && UsesHardwareQueries(VoxelGISettings::Feature::Shadows);
    }
    bool UsesRayShadows() const
    {
        return m_settings.UsesRayShadows(m_hardwareQueries) || UsesHardwareShadows();
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
            && m_environmentResidualRows != nullptr && m_unshadowedMap != nullptr;
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

    // Diagnostic count of rays issued outside the hybrid trace (gi_ray_totals.glsl): two
    // totals per slot, Compute then HW RT. While a capture is set, the sky-cache, injection
    // and receiver-shadow passes select their counting variants. `totals` is zeroed from
    // `zeros` when the frame starts and copied to `readback` after its last shadow pass.
    enum class RayTotalSlot : uint32_t
    {
        SkyCache,
        Injection,
        DeferredShadows,
        ForwardShadows,
        Count
    };
    static constexpr uint64_t kRayTotalsBytes =
        static_cast<uint64_t>(RayTotalSlot::Count) * ZEN_GI_TOTAL_COUNT_WORDS * sizeof(uint32_t);

    void SetRayTotalsCapture(RHIBuffer* totals, RHIBuffer* zeros, RHIBuffer* readback)
    {
        m_rayTotals         = totals;
        m_rayTotalsZeros    = zeros;
        m_rayTotalsReadback = readback;
        m_rayTotalsCleared  = false;
        m_rayTotalsRecorded = false;
        m_countedSkyCache   = false;
        m_countedInjection  = false;
    }
    bool CountsRays() const
    {
        return m_rayTotals != nullptr;
    }
    void BindRayTotals(RDGPassDescBase& pass) const;
    // Records the readback; DeferredLightingRenderer calls it after the frame's last shadow pass.
    void ReadRayTotals();
    bool WereRayTotalsRecorded() const
    {
        return m_rayTotalsRecorded;
    }
    // Whether the counted frame recorded the sky-cache update or a radiance injection.
    bool CountedSkyCache() const
    {
        return m_countedSkyCache;
    }
    bool CountedInjection() const
    {
        return m_countedInjection;
    }

private:
    void LoadSettings();

    void BuildMipChain(const HeapVector<RHITextureView*>& views, NameID program, const char* tag);

    bool PrepareMipViews(RHITexture* texture, HeapVector<RHITextureView*>& views);

    void PrepareRadianceHistory(bool update);

    // Sizes the light-visibility caches for this frame; inactive caches are header-only placeholders.
    void PrepareLightCache(bool requested);

    void BindFrameData(RDGPassDescBase& pass) const;

    void BuildEnvironmentDistribution();

    DistanceFieldSceneSettings BuildFieldSettings() const;
    HeapVector<AABB>           BuildLocalFieldVolumes() const;
    // Enabled features that do not resolve to HW RT under the given HW feature mask.
    uint32_t ComputeFeatureMask(uint32_t hardwareMask) const;

    SceneRayQuery                 m_rayQuery;
    LightFixtureMasks             m_fixtureMasks; // Ray-traced shadows skip rays a lamp's own fixture blocks.
    UniquePtr<DistanceFieldScene> m_field;
    uint32_t                      m_fieldResolution{0};
    uint64_t                      m_fieldSerial{0};       // Distinguishes generations of recreated or reset owners.
    bool                          m_fieldActive{false};   // Compute GI rays trace the ready field this frame.
    bool                          m_fieldRecorded{false}; // BuildRayQueryGraph called the field owner this frame.
    bool                          m_preparingField{false};
    uint64_t                      m_skyVisibility{0};    // Visibility generation the sky cache was traced with.
    SceneShadowRenderer*          m_shadowMaps{nullptr}; // This frame's shadow maps, which field gathers read.
    RHITexture*                   m_unshadowedMap{nullptr};
    RHISampler*                   m_unshadowedSampler{nullptr};
    uint64_t                      m_recordedSkyVisibility{0};
    bool                  m_hardwareQueries{false};
    uint32_t              m_hardwareFeatureMask{0};
    const char*                   m_providerReason{"field_selected"};
    RenderDevice*         m_device{nullptr};
    RenderScene*          m_scene{nullptr};
    VoxelizerBase*        m_voxelizer{nullptr};
    RHITexture*           m_radiance{nullptr};
    // Base-level hit history normally; alternate full mip chain with previous-frame radiance enabled.
    RHITexture*                 m_previousRadiance{nullptr};
    HeapVector<RHITextureView*> m_previousRadianceMips;
    // Preserve the older base level before the alternate chain is overwritten on compute.
    RHITexture*                 m_hitRadiancePrevious{nullptr};
    bool                        m_previousRadianceInitialized{false};
    bool                        m_recordedPreviousRadianceInitialization{false};
    bool                        m_radianceValid{false};
    bool                        m_recordedDelayedRadiance{false};
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
    RHIBuffer*                  m_rayTotals{nullptr};
    RHIBuffer*                  m_rayTotalsZeros{nullptr};
    RHIBuffer*                  m_rayTotalsReadback{nullptr};
    bool                        m_rayTotalsCleared{false};
    bool                        m_rayTotalsRecorded{false};
    bool                        m_countedSkyCache{false};
    bool                        m_countedInjection{false};

    // Light-visibility cache of m_radiance, m_previousRadiance and m_hitRadiancePrevious
    // (light_visibility_cache.glsl): per-class masks of the scene lights reaching a cell's
    // surfaces, and the lights they were traced for. Header-only while inactive.
    RHIBuffer* m_lightCache{nullptr};
    RHIBuffer* m_previousLightCache{nullptr};
    RHIBuffer* m_hitLightCachePrevious{nullptr};
    uint64_t   m_lightCacheBytes{0};
    uint64_t   m_lightCacheSnapshotBytes{0};
    bool       m_lightCacheActive{false};
    // Whether the history buffers hold a cache the graph has written, and whether this frame copies one.
    bool m_lightCacheHistoryInitialized{false};
    bool m_recordedLightCacheHistoryInitialization{false};
};
} // namespace zen::rc
