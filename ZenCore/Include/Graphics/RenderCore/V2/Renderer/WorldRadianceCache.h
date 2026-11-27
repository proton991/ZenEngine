#pragma once
#include "Graphics/RenderCore/V2/RenderGraph/RenderGraph.h"
#include "Graphics/RenderCore/V2/RenderCoreDefs.h"
#include "Math/Math.h"
#include "Graphics/RenderCore/V2/SceneLighting.h"

namespace zen::rc
{
class RenderDevice;
class RenderScene;
class VoxelGIRenderer;
struct HybridGIUniformData;

struct WorldSurfaceEdit
{
    glm::uvec4 stamp{}; // frame, 0 for bounds or 1 for material, renderable or material index
    Vec4       minimum{}, maximum{};
};
struct WorldSurfaceEdits
{
    glm::uvec4       log{}; // next write, count, most recent overwritten frame
    WorldSurfaceEdit entries[32]{};
    void             Add(const WorldSurfaceEdit& edit);
};
struct WorldCacheFrameData
{
    glm::uvec4        schedule{0};
    glm::uvec4        lightCount{0};
    Vec4              localLights[MaxSceneLights + 1]{};
    WorldSurfaceEdits surfaceEdits;
    glm::uvec4        movingLightIndices[(MaxSceneLights + 4u) / 4u]{};
};
static_assert(sizeof(WorldCacheFrameData) == 32 + 16 * (MaxSceneLights + 1) + 16 + 32 * 48 + 16 * ((MaxSceneLights + 4u) / 4u));
// Global lighting response and per-light probe guides shared by the cache and screen probes. The settled
// and moving light-edit logs each hold 32 edits of nine vectors; the moving log adds one header vector.
constexpr uint32_t WorldCacheLightingBytes = (7u + MaxSceneLights + 1u + 2u * ((MaxSceneLights + 4u) / 4u) + 1u
                                              + MaxSceneLights / 2u + 4u * (MaxSceneLights + 1u) + 2u * 9u * 32u + 24u)
                                           * sizeof(Vec4);

class WorldRadianceCache
{
public:
    explicit WorldRadianceCache(RenderDevice* device);
    ~WorldRadianceCache();
    bool Prepare(VoxelGIRenderer& voxelGI, RenderScene& scene, glm::uvec2 extent, bool reset);
    void Record(VoxelGIRenderer& voxelGI, RenderScene& scene, const HybridGIUniformData& receiver, RHISampler* sampler);
    void Bind(RDGPassDescBase& pass) const;
    bool HasMovingLights() const
    {
        return m_movingLights;
    }
    bool MovingLayoutChanged() const
    {
        return m_movingLayoutChanged;
    }
    void       CopySeedCounts() const;
    glm::uvec4 ReadSeedCounts() const;
    void       BindLighting(RDGPassDescBase& pass) const;
    void       OnRenderGraphExecuted(RenderFrameResult result);
    void       Destroy();
    uint32_t   GetCapacity() const
    {
        return m_capacity;
    }
    bool IsRecorded() const
    {
        return m_recorded;
    }
    uint64_t   GetRequiredBytes() const;
    RDGTexture RecordDebug(const HybridGIUniformData& receiver, RHISampler* sampler, glm::uvec2 extent) const;

private:
    bool          Allocate(RenderScene& scene, bool reset, uint32_t records, uint64_t pathBytes, uint32_t unusedFrames);
    RenderDevice* m_device;
    RHIBuffer*    m_seedReadback{nullptr};
    RHIBuffer*    m_buffers[5]{};
    glm::uvec2    m_tiles{0};
    uint32_t      m_capacity{0};
    uint32_t      m_records{6}; // Six directional records, plus the seed record for VoxelGISettings::worldCacheSeed.
    uint32_t      m_frame{0};
    uint32_t      m_lightingPolicy{UINT32_MAX}, m_lightingEpoch{0};
    uint64_t      m_geometry{0}, m_recordedGeometry{0}, m_surface{0};
    bool          m_needsClear{true};
    bool          m_recorded{false};
    struct Uniforms
    {
        glm::uvec4 limits{0};
        Vec4       camera{0};
        Vec4       previousCamera{0};
        Vec4       grid{0};
    } m_cache;
    static_assert(sizeof(Uniforms) == 64);
    struct CasterSnapshot
    {
        sg::AABB bounds;
        uint64_t revision{0};
    };
    HeapVector<CasterSnapshot> m_casters;
    bool                       m_movingLights{false};
    bool                       m_movingLayoutChanged{false};
    WorldSurfaceEdits          m_submittedEdits{};
    void                       RecordSurfaceEdits(const RenderScene& scene);
    WorldCacheFrameData        m_frameData{};
};
} // namespace zen::rc
