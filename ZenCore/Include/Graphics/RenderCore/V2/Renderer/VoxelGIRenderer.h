#pragma once
#include "Graphics/Shared/VoxelGI.h"
#include "Graphics/RenderCore/V2/RenderGraph/RenderGraph.h"
#include "Math/Math.h"
#include "Graphics/RenderCore/V2/SceneRayQuery.h"
#include "Graphics/RenderCore/V2/DistanceFieldScene.h"
#include "ZenFoundation/Utils/UniquePtr.h"
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
        Hardware
    };
    RayProvider rayProvider{RayProvider::Auto};
    // Auto resolves the whole tier by device capability; explicit choices select HW RT or Compute.
    uint32_t GetHardwareFeatureMask() const;
    bool     UsesRayShadows(bool hardwareReady) const;
    bool     UsesShadowReuse() const
    {
        return screenProbeGI ? nativeShadowReuse : shadowReuse;
    }
    uint32_t samples{4};
    uint32_t accelerationStructureBudgetMB{0}; // Explicit query allocation cap; zero leaves the P8 budget unset.
    uint32_t referenceSamples{0};              // Diagnostic: 0, 64, 256, 1024 or 4096; static mean, no spatial filter.
    uint32_t referenceTileStride{1};           // Reference diagnostic: 1 (dense), 8 (one 8x8 tile per 64x64 stratum).
    uint32_t samplingPhase{0};                 // Independent GI sample sequence for matched-camera replays.
    uint32_t historyFrames{32};
    // Pixel GI history/trace resolution divisor; final lighting remains at the view resolution.
    // 1, 2 or 4, or 0 for automatic (ResolvePixelResolutionDivisor). Reference and ray-count diagnostics use 1.
    uint32_t pixelResolutionDivisor{1};
    bool     temporal{true};
    bool     restirGI{false}; // HW per-pixel reservoir reuse; opt-in until acceptance measurements pass.
    bool     restirSpatial{true};
    uint32_t restirSpatialSamples{4};       // Spatial neighbors per signal, 1..4.
    bool     restirSpatialVisibility{true}; // Visibility rays for the pairwise MIS weights of spatial reuse.
    bool     restirSpecular{true};          // Rough lobes (roughness >= 0.3).
    // The GI denoiser (GIDenoise passes) always runs outside reference diagnostics. Off skips its
    // three spatial a-trous passes and the output stabilizer; temporal accumulation and history
    // fix still run.
    bool filter{true};
    bool radianceCache{false};        // Experimental HW secondary-hit cache; exact shading at zero capacity.
    bool nativeProbeBenchmark{false}; // Diagnostic: 64 cache-only rays per 16x16 tile, plus 25% extra probes.
    bool worldRadianceCache{false};   // Directional multi-bounce cache; retained pixel GI stays available for comparison.
    bool worldCacheFeedback{true};    // Disable only for matching the one-bounce reference contract.
    // HW RT probes shorten history only where measured lighting changed (global: every change resets all).
    bool     localHistoryResponse{true};
    uint32_t probeSamplingPattern{0}; // World anchor, current screen tile, or retained birth tile.
    bool     glossySurfaceHistory{false};
    bool     roughFootprintHistory{false};
    // One additional indirect record per cell seeds new pixel or experimental probe histories.
    bool     worldCacheSeed{false};
    uint32_t worldCacheTileSize{8};
    uint32_t worldCacheHistory{128};
    uint32_t worldCacheUnusedFrames{240};
    bool     screenProbeGI{false}; // Native-resolution gather from world-anchored screen probes.
    // RT-pipeline direct shadows from a 4x4 coarse grid, with exact rays where neighbours disagree.
    // nativeShadowReuse serves screen probes and shadowReuse the per-pixel methods (UsesShadowReuse).
    bool nativeShadowReuse{true};
    bool shadowReuse{false};
    bool dataDrivenAntiLag{true};    // Per-signal clamp deviation shortens slow history.
    bool youngReceiverSamples{true}; // Double diffuse samples below four accepted frames.
    bool bicubicHistory{true};       // Catmull-Rom history resampling when all twelve taps are valid.
    bool antiFirefly{true};          // History fix limits texels under four frames to their brightest matching neighbour.
    // Reduced-resolution GI texels take the mean shading normal of their pixel block's face and roughness
    // widened by the normals' spread, instead of one pixel's. No effect at native GI resolution.
    bool footprintReceivers{true};
    bool denoiserValidation{false}; // Diagnostic images and capture channels; no lighting change.
    bool rayTracingPipeline{false}; // Prototype: qualify pipeline ports before switching the HW tier.
    // Indirect diffuse from GI rays (sky light and bounce). Off lights diffuse from the
    // unoccluded environment, as without GI, while specular GI and direct lighting are unchanged.
    // The rays are still traced: it isolates the diffuse term rather than saving its cost.
    bool indirectDiffuse{true};
    bool specularOcclusion{true};
    enum class Reflections : uint32_t
    {
        Auto, // Reflected hit lighting: actual surfaces with HW RT, cached shading with Compute.
        Off,
        On
    };
    Reflections reflections{Reflections::On};
    // Compute GI rays (sky, specular, bounce, reflections and the sky cache) trace a GPU-built
    // sparse distance field covering every committed occluder (DistanceFieldScene). Cells per
    // axis: 32 or 64 (the builder's limit), each holding 8x8x8 distance samples over the scene bounds.
    uint32_t fieldResolution{64};
    float    indirectIntensity{1.0f};
    float    normalBiasVoxels{1.5f};
    bool     shadows{true};
    // False forces maps. True uses HW shadow rays when HW RT is selected and ready; Compute uses maps.
    bool rtShadows{true};
    bool analyticLighting{true};
    bool environmentLighting{true};
    bool emissiveLighting{true};

    bool operator==(const VoxelGISettings&) const = default;
};

bool ValidateVoxelGISettings(const VoxelGISettings& settings);

// The per-pixel GI divisor for an output height. An explicit divisor is kept; automatic keeps at least
// 540 GI lines: quarter per axis at 2160 lines and above, half from 1080, full below.
uint32_t ResolvePixelResolutionDivisor(uint32_t requested, uint32_t outputHeight);

// Why the configured HW tier cannot run on this device, or nullptr when it can. The HW tier needs usable
// ray queries; with rayTracingPipeline it also needs RT pipelines that trace closest-hit shadow rays.
// Capability decides, never device or driver identity.
const char* GetHardwareTierUnavailableReason(const VoxelGISettings& settings, const RHIGPUInfo& gpu);

// Whether a SceneRayQuery reason is a failure that the next frame will not fix: missing features, the
// acceleration-structure budget, or scene preparation or hit-stream allocation failures. A pending
// build and a failed frame are retried.
bool IsPersistentHardwareTierFailure(const char* reason);

bool LoadVoxelGISettings(const platform::ConfigLoader& config, VoxelGISettings& output, const VoxelGISettings& defaults = {});

struct VoxelGIUniformData
{
    Vec4 gridMinVoxelSize;
    Vec4 volume;
    Vec4 cone;
    Vec4 limits;
    Vec4 lighting{1.0f, 1.0f, 1.0f, 0.0f}; // Analytic, environment, emissive contributions; ray-hit bounce enabled.
    Vec4 indirect{1.0f, 0.0f, 0.0f, 0.0f}; // Diffuse enabled, HW feature mask, forward ray samples, frame bits.
};
static_assert(sizeof(VoxelGIUniformData) == 96);

class VoxelGIRenderer
{
public:
    VoxelGIRenderer(RenderDevice* device, VoxelizerBase* voxelizer, const VoxelGISettings* settings = nullptr);

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

    // The HW tier was chosen by capability (RayProvider::Auto resolved by RendererServer). A persistent
    // HW failure then selects the Compute tier until the settings, the scene or this choice change;
    // an explicitly selected HW tier stays unavailable instead. RayProvider::Auto is always automatic.
    void SetAutomaticTier(bool automatic);

    // The persistent HW failure that moved an automatic tier to Compute, or nullptr.
    const char* GetTierFallbackReason() const
    {
        return m_tierFallback;
    }

    const VoxelGISettings& GetSettings() const
    {
        return m_settings;
    }

    void BindLightingInputs(RDGPassDescBase& pass, bool environment = false) const;
    void BindSecondaryBRDF(RDGPassDescBase& pass) const;
    void BindRayInputs(RDGPassDescBase& pass) const;
    void BindHitRadianceInputs(RDGPassDescBase& pass) const;
    void BindNativeFieldInputs(RDGPassDescBase& pass, bool environment = true) const;

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
    bool     UsesRayTracingPipeline() const;
    uint32_t GetHardwareFeatureMask() const
    {
        return m_hardwareFeatureMask;
    }
    // HW-only transport shades actual hits without voxel lighting.
    bool UsesVoxelLighting() const
    {
        return !m_hardwareQueries;
    }
    // The Compute tier is waiting for its first published distance field.
    bool IsPreparingField() const
    {
        return m_preparingField;
    }
    // Compute traces a published field that is still being completed: it predates committed edits, or
    // local volumes around lights are still building and those lights use the scene field.
    bool IsRefiningField() const;
    // True when the Compute tier traces the ready distance field.
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
        return m_settings.UsesRayShadows(m_hardwareQueries);
    }
    bool UsesRayShadows() const
    {
        return UsesHardwareShadows();
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
    void BindHardwareRayInputs(RDGPassDescBase& pass, bool environment = true, bool occluders = false) const;
    void BindShadowInputs(RDGPassDescBase& pass, SceneShadowRenderer* shadows, bool forwardMaterialBound = false) const;

    void BindEnvironmentSamplingInputs(RDGPassDescBase& pass) const;

    // Order-2 spherical-harmonic projection of the sampled environment (EnvironmentSampling.h).
    void BindEnvironmentHarmonics(RDGPassDescBase& pass) const;

    bool IsInitialized() const
    {
        return m_radiance != nullptr && m_skyIrradiance != nullptr && m_environmentColumns != nullptr
            && m_environmentRows != nullptr && m_environmentHarmonics != nullptr && m_environmentLuminance != nullptr
            && m_environmentTiles != nullptr && m_environmentSources != nullptr && m_environmentResidualColumns != nullptr
            && m_environmentResidualRows != nullptr && m_unshadowedMapView != nullptr;
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

    void      BuildEnvironmentDistribution();
    void      BuildLightOccluders();
    bool      PrepareNativeDynamicGeometry();
    RDGBuffer m_lightOccluders;

    DistanceFieldSceneSettings BuildFieldSettings() const;
    HeapVector<AABB>           BuildLocalFieldVolumes() const;

    SceneRayQuery                 m_rayQuery;
    UniquePtr<DistanceFieldScene> m_field;
    uint32_t                      m_fieldResolution{0};
    uint64_t                      m_fieldSerial{0};       // Distinguishes generations of recreated or reset owners.
    bool                          m_fieldActive{false};   // Compute GI rays trace the ready field this frame.
    bool                          m_fieldRecorded{false}; // BuildRayQueryGraph called the field owner this frame.
    bool                          m_preparingField{false};
    uint64_t                      m_skyVisibility{0};    // Visibility generation the sky cache was traced with.
    SceneShadowRenderer*          m_shadowMaps{nullptr}; // This frame's shadow maps, which field gathers read.
    RHITexture*                   m_unshadowedMap{nullptr};
    RHITextureView*               m_unshadowedMapView{nullptr};
    RHISampler*                   m_unshadowedSampler{nullptr};
    uint64_t                      m_recordedSkyVisibility{0};
    bool                          m_hardwareQueries{false};
    bool                          m_requestedHardware{false}; // The previous frame requested the HW tier.
    bool                          m_automaticTier{false};
    const char*                   m_tierFallback{nullptr};
    uint32_t                      m_hardwareFeatureMask{0};
    const char*                   m_providerReason{"field_selected"};
    RenderDevice*                 m_device{nullptr};
    RenderScene*                  m_scene{nullptr};
    FrameBufferRing               m_nativeDynamicGeometry;
    uint64_t                      m_nativeDynamicRevision{0};
    uint32_t                      m_nativeDynamicCount{0};
    VoxelizerBase*                m_voxelizer{nullptr};
    RHITexture*                   m_radiance{nullptr};
    RHITexture*                   m_emptyLighting{nullptr}; // Valid unused legacy texture bindings in HW-only shaders.
    bool                          m_emptyLightingInitialized{false};
    bool                          m_recordedEmptyLighting{false};
    bool                          m_recordedEnvironmentDistribution{false};
    uint32_t                      m_frame{0};
    // Base-level snapshot used by the old denoiser to correct changes in cached hit lighting.
    RHITexture*                 m_previousRadiance{nullptr};
    bool                        m_previousRadianceInitialized{false};
    bool                        m_recordedPreviousRadianceInitialization{false};
    bool                        m_radianceValid{false};
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
    uint64_t                    m_geometryRevision{0};
    uint64_t                    m_lightingRevision{0};
    uint64_t                    m_environmentRevision{0};
    uint64_t                    m_recordedGeometry{0};
    uint64_t                    m_recordedLighting{0};
    uint64_t                    m_recordedEnvironment{0};
    uint64_t                    m_distributionTextureRevision{0}, m_recordedTextureRevision{0};
    uint64_t                    m_radianceGeneration{0};
    bool                        m_recordedRadiance{false};
    RHIBuffer*                  m_rayTotals{nullptr};
    RHIBuffer*                  m_rayTotalsZeros{nullptr};
    RHIBuffer*                  m_rayTotalsReadback{nullptr};
    bool                        m_rayTotalsCleared{false};
    bool                        m_rayTotalsRecorded{false};
    bool                        m_countedSkyCache{false};
    bool                        m_countedInjection{false};

    // Light-visibility cache of m_radiance and m_previousRadiance
    // (light_visibility_cache.glsl): per-class masks of the scene lights reaching a cell's
    // surfaces, and the lights they were traced for. Header-only while inactive.
    RHIBuffer* m_lightCache{nullptr};
    RHIBuffer* m_previousLightCache{nullptr};
    uint64_t   m_lightCacheBytes{0};
    bool       m_lightCacheActive{false};
    // Whether the history buffers hold a cache the graph has written, and whether this frame copies one.
    bool m_lightCacheHistoryInitialized{false};
    bool m_recordedLightCacheHistoryInitialization{false};
};
} // namespace zen::rc
