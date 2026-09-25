#pragma once
#include "Graphics/RenderCore/V2/GIVisibilityProvider.h"
#include "Graphics/RenderCore/V2/DynamicVoxelGIPlanning.h"
#include "Graphics/RenderCore/V2/SceneLighting.h"
#include "Graphics/Shared/DynamicVoxelGI.h"
#include "Graphics/RenderCore/V2/ComputeDispatch.h"
#include <chrono>

namespace zen::rc
{
class RenderDevice;
class RenderScene;
class SceneShadowRenderer;

struct StaticVoxelGIInputs
{
    GIGridUniform grid{};
    uint64_t listGeneration{0};
    RHIBuffer* occupiedList{nullptr};
    RHIBuffer* gridToList{nullptr};
    RHIBuffer* occupiedCount{nullptr};
    RHIBuffer* dynamicCount{nullptr};
    RHITexture* owner{nullptr};
    RHITexture* dynamicOwner{nullptr};
    RHIBuffer* dynamicMap{nullptr};
    // Zero extent selects all static receivers for estimator/reference diagnostics.
    glm::uvec2 viewportExtent{0};
    // Optional explicit views for native fixtures; normal frames use G-buffer output tags.
    RHITextureView* surfaceViews[5]{}; // position, normal, geometric normal, receiver, depth.
    // Caller advances this on discontinuities (teleport/removal), not continuous motion.
    uint64_t historyRevision{0};
    // Optional deterministic clock for tests; -1 uses the renderer's steady clock.
    double timeSeconds{-1};
    RHITexture* normal{nullptr};
    RHITexture* dynamicNormal{nullptr};
    RHITextureView* environment{nullptr};
    RHISampler* environmentSampler{nullptr};
    uint64_t environmentRevision{0};
    // Production analytic visibility uses the existing mesh shadow maps. Native
    // query/provider fixtures omit these and retain their explicit query oracle.
    const RenderScene* scene{nullptr};
    const SceneShadowRenderer* shadowMaps{nullptr};
};

class DynamicVoxelGIRenderer
{
public:
    explicit DynamicVoxelGIRenderer(RenderDevice* device) : m_device(device) {}
    bool Init(const DynamicVoxelGISettings& settings,
              bool averaged,
              uint64_t receiverSurfaceBytes = 0,
              uint64_t retiringBytes        = 0);
    bool BuildRenderGraph(const StaticVoxelGIInputs& inputs,
                          const GIVisibilityProvider& provider,
                          const SceneUniformData& scene,
                          float indirectGain,
                          bool shadows);
    void BindLightingInputs(RDGPassDescBase& pass) const;
    void OnRenderGraphExecuted(bool succeeded);
    void SetFiltering(GITemporalMode temporal, bool spatial);
    void SetLighting(bool analytic, bool environment, bool emissive);
    void Destroy();
    uint32_t GetCapacity() const
    {
        return m_uniform.volume.y;
    }
    uint32_t GetCacheStride() const
    {
        return m_work.cache.x != 0 ? GI_COMPACT_HIT_BYTES : sizeof(GIHit);
    }
    uint32_t GetRaysPerFace() const
    {
        return m_uniform.sampling.x;
    }
    // Reserved method peak includes class/cone resources and receiver attachments.
    uint64_t GetResourceBytes() const
    {
        return m_resourceBytes;
    }
    RHIBuffer* GetStatus() const
    {
        return m_status;
    }
    RHIBuffer* GetLightMask() const
    {
        return m_lightMask;
    }
    RHIBuffer* GetWorkCounts() const
    {
        return m_workCounts;
    }
    RHIBuffer* GetReceiverList(uint32_t objectClass) const
    {
        return m_receiverLists[objectClass == GI_DYNAMIC ? 1 : 0];
    }
    RHIBuffer* GetIndirectArguments() const
    {
        return m_indirect;
    }
    uint64_t GetCacheBuildBatches() const
    {
        return m_cacheBuildBatches;
    }
    // Bits rebuilt by the last recorded frame; zero means both mask passes were reused.
    uint32_t GetLightMaskUpdateBits() const
    {
        return m_lighting.enabled.w;
    }
    RHITexture* GetDynamicIrradiance(uint32_t face) const
    {
        return face < GI_FACE_COUNT ? m_dynamicRaw[face] : nullptr;
    }
    RHIBuffer* GetDynamicLightMask() const
    {
        return m_dynamicLightMask;
    }
    RHITexture* GetDynamicFilteredIrradiance(uint32_t face) const
    {
        return face < GI_FACE_COUNT ? m_dynamicFiltered[face] : nullptr;
    }
    RHITexture* GetHistoryIrradiance(uint32_t face, uint32_t objectClass) const
    {
        return face < GI_FACE_COUNT ? m_history[objectClass == GI_DYNAMIC ? 1 : 0][face] : nullptr;
    }
    const GIFilterUniform& GetFilterUniform() const
    {
        return m_filter;
    }
    RHITexture* GetSenderEnvironment(uint32_t objectClass) const
    {
        return m_senderEnvironment[objectClass == GI_DYNAMIC ? 1 : 0];
    }
    RHIBuffer* GetHistoryMetadata(uint32_t objectClass) const
    {
        return m_historyMetadata[objectClass == GI_DYNAMIC ? 1 : 0];
    }
    RHIBuffer* GetCache(uint32_t face) const
    {
        return face < GI_FACE_COUNT ? m_hits[face] : nullptr;
    }
    RHITexture* GetRawIrradiance(uint32_t face) const
    {
        return face < GI_FACE_COUNT ? m_raw[face] : nullptr;
    }
    RHITexture* GetPaddedIrradiance(uint32_t face) const
    {
        return face < GI_FACE_COUNT ? m_padded[face] : nullptr;
    }

private:
    struct LightMaskState
    {
        GIVisibilityInfo visibility{};
        SceneUniformData scene{};
        Vec4 minimumCellSize{};
        uint64_t listGeneration{0};
        bool shadows{false};
        bool analytic{false};
        bool dynamicInputs{false};
        bool meshLightVisibility{false};
    };
    void PrepareLightMasks(const StaticVoxelGIInputs& inputs,
                           const GIVisibilityInfo& visibility,
                           const SceneUniformData& scene,
                           bool shadows);
    bool BuildEnvironmentGraph(const StaticVoxelGIInputs& inputs,
                               const GIVisibilityProvider& provider,
                               const SceneUniformData& scene,
                               const HeapVector<ComputeDispatchChunk>& cells);
    bool BuildReceiverSelection(const StaticVoxelGIInputs& inputs,
                                const HeapVector<ComputeDispatchChunk>& cells);
    void BindFrameInputs(RDGPassDescBase& pass) const;
    void BuildFilterGraph(const StaticVoxelGIInputs& inputs,
                          const HeapVector<ComputeDispatchChunk>& cells,
                          uint32_t face);
    void BuildHistoryMetadata(const StaticVoxelGIInputs& inputs,
                              const HeapVector<ComputeDispatchChunk>& cells,
                              bool clear);
    RenderDevice* m_device;
    uint64_t m_resourceBytes{0};
    RHIBuffer* m_hits[GI_FACE_COUNT]{};
    RHITexture* m_raw[GI_FACE_COUNT]{};
    RHITexture* m_padded[GI_FACE_COUNT]{};
    RHIBuffer* m_lightMask{nullptr};
    // Persistent per-light Unknown bits survive frames that reuse mask data.
    RHIBuffer* m_lightMaskStatus{nullptr};
    LightMaskState m_lightMaskState{};
    bool m_lightMasksValid{false};
    RHIBuffer* m_status{nullptr};
    RHISampler* m_sampler{nullptr};
    RHISampler* m_surfaceSampler{nullptr};
    GIStaticUniform m_uniform{};
    glm::uvec4 m_staticQuerySettings{};
    GIWorkUniform m_work{};
    GIFilterUniform m_filter{};
    GILightingUniform m_lighting{};
    RHIBuffer* m_senderPositions[2]{nullptr, nullptr};
    RHITexture* m_senderEnvironment[2]{};
    uint64_t m_environmentGeneration{0};
    uint64_t m_recordedEnvironmentGeneration{0};
    uint64_t m_environmentRevision{0};
    uint64_t m_environmentResource{0};
    Vec3 m_environmentSettings{};
    bool m_environmentValid{false};
    RHITexture* m_history[2][GI_FACE_COUNT]{};
    RHITexture* m_dynamicFiltered[GI_FACE_COUNT]{};
    RHIBuffer* m_historyMetadata[2]{};
    std::chrono::steady_clock::time_point m_clockStart{std::chrono::steady_clock::now()};
    double m_lastTime{-1};
    double m_recordedTime{0};
    uint64_t m_historyRevision{0};
    uint64_t m_recordedHistoryRevision{0};
    bool m_historyValid{false};
    bool m_recordedFrame{false};
    RHITexture* m_dynamicRaw[GI_FACE_COUNT]{};
    RHIBuffer* m_dynamicLightMask{nullptr};
    RHIBuffer* m_receiverFlags[2]{};
    RHIBuffer* m_receiverLists[2]{};
    RHIBuffer* m_workCounts{nullptr};
    RHIBuffer* m_indirect{nullptr};
    HeapVector<ComputeDispatchChunk> m_workChunks;
    uint32_t m_cacheEnd{0};
    uint32_t m_recordedCacheEnd{0};
    uint64_t m_cacheBuildBatches{0};
    bool m_recordedCacheBatch{false};
    uint64_t m_generation{0};
    uint64_t m_listGeneration{0};
    uint64_t m_recordedListGeneration{0};
    uint64_t m_recordedGeneration{0};
    GIVisibilityBackend m_backend{GIVisibilityBackend::eVoxelDDA};
    GIVisibilityBackend m_recordedBackend{GIVisibilityBackend::eVoxelDDA};
};
} // namespace zen::rc
