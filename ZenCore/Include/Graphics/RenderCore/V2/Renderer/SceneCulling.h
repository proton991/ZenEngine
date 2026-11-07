#pragma once

#include "Graphics/RenderCore/V2/FrameBufferRing.h"
#include "Graphics/RenderCore/V2/RenderGraph/RDGDefs.h"
#include "Graphics/Shared/SceneCulling.h"

namespace zen::rc
{
class RenderDevice;
class RenderGraph;
class RenderScene;
class HybridGIRenderer;
struct RDGGraphicsPassDesc;

// std430 layout shared with SceneRenderer/gbuffer_draw.glsl. One record per
// candidate submesh; bounds cover its committed geometry.
struct GBufferDrawData
{
    Vec4       boundsMin{0.0f};
    Vec4       boundsMax{0.0f};
    glm::uvec4 geometry{0}; // index count, first index, reserved
    glm::uvec4 indices{0};  // node, reserved. Materials are per batch.
};
static_assert(sizeof(GBufferDrawData) == 64);

struct FrustumCullUniformData
{
    Mat4 projectionView{1.0f};
};
static_assert(sizeof(FrustumCullUniformData) == 64);

enum class SceneCullingStatus
{
    eNotUsed,
    eDisabled,
    eEmpty,
    eCulled,
    eUnsupportedFirstInstance,
    eVaryingLimit,
    eBufferLimit,
    eDispatchLimit,
    eUploadFailed,
    eAllocationFailed
};

struct SceneCullingResult
{
    SceneCullingStatus status{SceneCullingStatus::eNotUsed};
    uint32_t           candidateCount{0}; // Zero when the device cannot cull: draws are not grouped for it.
    uint64_t           uploadedBytes{0};
};

const char* SceneCullingStatusName(SceneCullingStatus status);
const char* SceneCullingFallbackReason(SceneCullingStatus status);

inline bool SupportsGBufferCulling(const RHIGPUInfo& gpu)
{
    return gpu.supportDrawIndirectFirstInstance && gpu.maxDrawIndirectCount != 0;
}

// Only hybrid receivers need a varying beyond the guaranteed 64 components. Gate shader-module
// creation as well as draw selection: native validation can reject oversized interfaces
// before any graphics pipeline is requested.
inline bool SupportsHybridGBufferCulling(const RHIGPUInfo& gpu)
{
    return SupportsGBufferCulling(gpu) && gpu.maxVertexOutputComponents >= ZEN_SCENE_CULL_VARYING_COMPONENTS
        && gpu.maxFragmentInputComponents >= ZEN_SCENE_CULL_VARYING_COMPONENTS;
}

// A contiguous command range sharing one pipeline and material.
struct SceneCullingBatch
{
    NameID             shader;
    RHIPolygonCullMode cullMode{RHIPolygonCullMode::eDisabled};
    uint32_t           materialIndex{0};
    uint32_t           firstDraw{0};
    uint32_t           drawCount{0};
};

// Committed scene state that candidates, their order and their batches depend on.
struct SceneCullingDrawKey
{
    uint64_t scene{0};
    uint64_t geometryRevision{0};
    uint64_t surfaceRevision{0};
    uint64_t renderables{0};
    bool     hybrid{false};
    bool     preserveDrawOrder{false};

    bool operator==(const SceneCullingDrawKey&) const = default;
};

struct SceneCullingDrawCache
{
    FrameBufferRing             draws;
    HeapVector<GBufferDrawData> contents;
    // Ordinary frames neither allocate nor upload previous-model matrices.
    FrameBufferRing  previousModels;
    HeapVector<Mat4> previousModelContents;
    // Valid for key. A matching key skips grouping, record building and the draw upload.
    HeapVector<SceneCullingBatch> batches;
    SceneCullingDrawKey           key;

    void Destroy(RenderDevice& device);
};

// On success consumes desc and records the compute producer and graphics consumer.
// Indirect draws batch by pipeline and material, with one material push constant per batch.
// Materials keep their order of first use, so a material's later draws move up to its first
// one; preserveDrawOrder keeps direct order and batches only consecutive compatible draws.
// Unsupported capabilities/limits or failed preparation leave desc for direct drawing.
// Call once per frame after BeginFrame, before consumers, like other FrameBufferRings.
// pArguments optionally receives the transient argument buffer, valid only in this graph.
SceneCullingResult AddCulledGBufferPass(RenderDevice&           device,
                                        SceneCullingDrawCache&  cache,
                                        RenderGraph&            graph,
                                        RDGGraphicsPassDesc&    desc,
                                        const RenderScene&      scene,
                                        const HybridGIRenderer* hybrid            = nullptr,
                                        bool                    preserveDrawOrder = false,
                                        RDGBuffer*              pArguments        = nullptr);
} // namespace zen::rc
