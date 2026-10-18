#pragma once
#include "Graphics/RHI/RHIAccelerationStructure.h"
#include "Graphics/RenderCore/V2/RenderCoreDefs.h"
#include "Graphics/RenderCore/V2/RenderGraph/RDGPassCompiler.h"
#include "Graphics/RenderCore/V2/ResourceRetirement.h"
#include "Graphics/RenderCore/V2/RenderFrameState.h"

namespace zen::sg
{
class Mesh;
}
namespace zen::rc
{
class RenderScene;
class RenderDevice;

void BindSceneRayGeometry(RDGPassDescBase& pass, RenderDevice& device, RenderScene& scene, bool forwardMaterialBound);

// One full-scene query snapshot. Builds and shader consumers use the same raster
// buffers; publication follows successful graph execution, never command recording.
class SceneRayQuery
{
public:
    // Consecutive in-place refits before a BLAS is rebuilt, bounding BVH quality loss.
    static constexpr uint32_t kMaxRefits = 16;

    explicit SceneRayQuery(RenderDevice* device) : m_device(device) {}

    bool BuildRenderGraph(RenderScene& scene, uint64_t budgetBytes = 0);
    void BindInputs(RDGPassDescBase& pass, RenderScene& scene, bool forwardMaterialBound = false) const;
    void OnRenderGraphExecuted(RenderFrameResult result);
    void Destroy();

    bool        IsReady() const;
    uint64_t    GetGeneration() const;
    uint64_t    GetMemoryBytes() const;
    const char* GetReason() const
    {
        return m_reason;
    }

    // BLAS work recorded by the most recent BuildRenderGraph call.
    struct BuildStatistics
    {
        uint32_t built{0};   // Full builds, including periodic rebuilds of refit structures.
        uint32_t updated{0}; // In-place refits of deformed meshes.
        uint32_t reused{0};  // Unchanged meshes sharing the previous structure.
    };
    const BuildStatistics& GetBuildStatistics() const
    {
        return m_statistics;
    }

private:
    struct MeshBuild
    {
        const sg::Mesh*                              mesh{nullptr};
        HeapVector<RHIAccelerationStructureGeometry> geometries;
        RHIAccelerationStructureBuildSizes           sizes;
        RHIAccelerationStructure*                    structure{nullptr};
        uint32_t                                     metadataOffset{0};
        // Referenced vertex range and a hash of its committed positions. Buffer
        // identity alone cannot detect deformation of persistent scene storage.
        uint32_t vertexBegin{0};
        uint32_t vertexEnd{0};
        uint64_t positionHash{0};
        uint32_t refits{0};
        bool     needsBuild{true};
        bool     update{false}; // In-place refit of `structure`; otherwise a full build.
    };

    struct Generation
    {
        HeapVector<MeshBuild>     meshes;
        RHIAccelerationStructure* tlas{nullptr};
        RHIBuffer*                instances{nullptr};
        RHIBuffer*                geometryMetadata{nullptr};
        RHIBuffer*                instanceMetadata{nullptr};
        uint64_t                  geometryRevision{0};
        uint64_t                  surfaceRevision{0};
        uint64_t                  vertexIdentity{0};
        uint64_t                  vertexGeneration{0};
        uint64_t                  indexIdentity{0};
        uint64_t                  generation{0};
        uint32_t                  instanceCount{0};
        uint32_t                  slot{0};
        HeapVector<uint8_t>       instancePayload;
        HeapVector<uint8_t>       geometryMetadataPayload;
        HeapVector<uint8_t>       instanceMetadataPayload;
    };

    struct RetiredMemory
    {
        uint64_t           bytes{0};
        ResourceRetirement retirement;
    };

    bool       Prepare(RenderScene& scene, uint64_t budgetBytes);
    bool       PrepareMeshes(RenderScene& scene, HeapVector<glm::uvec4>& metadata);
    RHIBuffer* CreateInputBuffer(NameID name, uint64_t size, const void* data);
    bool       UpdateInput(RHIBuffer*&          buffer,
                           HeapVector<uint8_t>& payload,
                           NameID               name,
                           uint32_t             size,
                           const void*          data,
                           bool                 instances = false);
    void       RecordBuilds(RenderScene& scene);
    // Callers bracket releases with GetMemoryBytes()/RetireReleasedMemory().
    void              Release(Generation& generation);
    void              ReleaseGenerations();
    void              Cache(Generation& generation);
    void              RetireReleasedMemory(uint64_t previousBytes);
    const Generation& Current() const;

    RenderDevice* m_device;
    RenderScene*  m_scene{nullptr};
    Generation    m_active;
    Generation    m_pending;
    // An inactive snapshot per frame slot. At least two slots keep preparation
    // from uploading over the committed snapshot, including after a declined frame.
    Generation                m_cached[RenderFrameState::kMaxFramesInFlight];
    HeapVector<RetiredMemory> m_retired;
    // Grow-only build scratch shared by generations; the graph orders its reuse.
    RHIBuffer*      m_scratch{nullptr};
    uint64_t        m_scratchBytes{0};
    BuildStatistics m_statistics;
    bool            m_recorded{false};
    const char*     m_reason{"scene_not_ready"};
};
} // namespace zen::rc
