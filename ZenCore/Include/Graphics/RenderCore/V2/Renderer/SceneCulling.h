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
    glm::uvec4 indices{0};  // node, material, reserved
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

inline bool SupportsIndirectFirstInstance(const RHIGPUInfo& gpu)
{
    return gpu.supportDrawIndirectFirstInstance && gpu.maxDrawIndirectCount != 0;
}

// Gate shader-module creation as well as draw selection: native validation can
// reject oversized interfaces before any graphics pipeline is requested.
inline bool SupportsIndirectGBufferShaders(const RHIGPUInfo& gpu)
{
    return gpu.maxVertexOutputComponents >= ZEN_SCENE_CULL_VARYING_COMPONENTS
        && gpu.maxFragmentInputComponents >= ZEN_SCENE_CULL_VARYING_COMPONENTS;
}

inline bool SupportsGBufferCulling(const RHIGPUInfo& gpu)
{
    return SupportsIndirectFirstInstance(gpu) && SupportsIndirectGBufferShaders(gpu);
}

struct SceneCullingDrawCache
{
    FrameBufferRing             draws;
    HeapVector<GBufferDrawData> contents;
    // Ordinary frames neither allocate nor upload previous-model matrices.
    FrameBufferRing  previousModels;
    HeapVector<Mat4> previousModelContents;

    void Destroy(RenderDevice& device);
};

// On success consumes desc and records the compute producer and graphics consumer.
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
