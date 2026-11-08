#include "Graphics/RenderCore/V2/Renderer/DeferredLightingRenderer.h"
#include "Graphics/RenderCore/V2/Renderer/VoxelGIRenderer.h"
#include "Graphics/RenderCore/V2/Renderer/SceneShadowRenderer.h"
#include "Graphics/RenderCore/V2/Renderer/RendererUtils.h"
#include "Graphics/RenderCore/V2/Renderer/GBuffer.h"
#include "Graphics/RenderCore/V2/Renderer/SceneCulling.h"
#include "Graphics/RenderCore/V2/Renderer/HybridGIRenderer.h"
#include "Graphics/RenderCore/V2/Renderer/SkyboxRenderer.h"
#include "Graphics/RenderCore/V2/Renderer/RendererServer.h"
#include "Graphics/RenderCore/V2/RenderScene.h"
#include "Graphics/RenderCore/V2/RenderDevice.h"
#include "Platform/ConfigLoader.h"
#include "Graphics/RenderCore/V2/RenderResource.h"
#include "Graphics/RenderCore/V2/ShaderProgram.h"
#include "Graphics/RenderCore/V2/ComputeDispatch.h"
#include "Graphics/RenderCore/V2/VoxelResourcePlanning.h"
#include "Graphics/Shared/LightingCapture.h"
#include "SceneGraph/Camera.h"
#include <cmath>
#include <algorithm>
#include <cstring>

namespace zen::rc
{
namespace
{
// One candidate's draw and its path-specific data, so reordering cannot separate them.
struct GBufferDrawItem
{
    SceneMeshDraw      draw;
    HybridReceiverDraw receiver; // Direct hybrid draws only.
    sg::AABB           bounds;   // Indirect draws only.
};

void RecordGBufferDraws(RDGPassCmdEncoder& encoder, const HeapVector<GBufferDrawItem>& items)
{
    GBufferSP::PushConstantsData constants{};

    for (const GBufferDrawItem& item : items)
    {
        constants.nodeIndex     = item.draw.nodeIndex;

        constants.materialIndex = item.draw.materialIndex;

        encoder.SetPushConstants(constants);

        encoder.DrawIndexed(item.draw.indexCount, 1, item.draw.firstIndex, 0, 0);
    }
}

struct GBufferDrawGroup
{
    NameID                      shader;
    RHIPolygonCullMode          cullMode{RHIPolygonCullMode::eDisabled};
    HeapVector<GBufferDrawItem> items;
    uint32_t                    pipelineVariant{0};
};

using GBufferDrawGroups = HeapVector<GBufferDrawGroup>;

// Batches need each material's draws adjacent. Ordering materials by first use moves draws as
// little as possible from the direct path, whose order decides overdraw and coplanar winners.
void GroupDrawsByFirstMaterialUse(GBufferDrawGroup& group)
{
    HashMap<uint32_t, uint32_t> firstUse;
    for (uint32_t index = 0; index < group.items.size(); ++index)
    {
        firstUse.try_emplace(group.items[index].draw.materialIndex, index);
    }
    std::stable_sort(group.items.begin(), group.items.end(),
                     [&firstUse](const GBufferDrawItem& lhs, const GBufferDrawItem& rhs) {
                         return firstUse.at(lhs.draw.materialIndex) < firstUse.at(rhs.draw.materialIndex);
                     });
}

NameID GBufferShaderName(bool hybrid, bool masked, bool indirect, uint32_t uvSetCount)
{
    const char* const shaders[2][2][2] = {
        {{"GBufferSP", "GBufferIndirectSP"}, {"GBufferMaskedSP", "GBufferMaskedIndirectSP"}},
        {{"HybridReceiverSP", "HybridReceiverIndirectSP"}, {"HybridReceiverMaskedSP", "HybridReceiverMaskedIndirectSP"}}};
    return MaterialUVShaderName(shaders[hybrid][masked][indirect], uvSetCount);
}

// Opaque before masked, with separate two-sided, mirrored and ordinary pipelines.
GBufferDrawGroups GroupGBufferDraws(const RenderScene&      scene,
                                    const HybridGIRenderer* hybrid,
                                    bool                    preserveDrawOrder,
                                    bool                    indirect = false)
{
    GBufferDrawGroups groups(preserveDrawOrder ? 0 : 6);
    for (const sg::Node* node : scene.GetRenderableNodes())
    {
        const uint32_t instance = node->GetRenderableIndex();
        if (scene.GetInstanceMask(instance) != 0)
        {
            const bool                      mirrored = scene.GetInstanceOrientation(instance) < 0.0f;
            const HeapVector<sg::SubMesh*>& meshes   = node->GetComponent<sg::Mesh>()->GetSubMeshes();
            for (uint32_t subMeshIndex = 0; subMeshIndex < meshes.size(); ++subMeshIndex)
            {
                const sg::SubMesh*      mesh  = meshes[subMeshIndex];
                const uint32_t          index = mesh->GetMaterial()->index;
                const sg::MaterialData& material =
                    index < scene.GetMaterialsData().size() ? scene.GetMaterialsData()[index] : mesh->GetMaterial()->data;
                const bool core        = material.textureSampling.z > 0.0f;
                const bool forwardOnly = (material.surfaceProperties.y == static_cast<float>(sg::AlphaMode::Blend) && !core)
                                      || material.sheenColorTransmission.w > 0.0f
                                      || (hybrid != nullptr && material.diffuseTransmissionColorFactor.w > 0.0f);
                if (mesh->topology == sg::MeshTopology::Triangles && !forwardOnly)
                {
                    const bool masked = core || material.surfaceProperties.y == static_cast<float>(sg::AlphaMode::Mask);
                    const RHIPolygonCullMode cullMode = material.materialProperties.z >= 0.5f ? RHIPolygonCullMode::eDisabled
                                                      : mirrored                              ? RHIPolygonCullMode::eFront
                                                                                              : RHIPolygonCullMode::eBack;
                    const NameID shader     = GBufferShaderName(hybrid != nullptr, masked, indirect, scene.GetUVSetCount());
                    uint32_t     groupIndex = (masked ? 3u : 0u) + static_cast<uint32_t>(cullMode);
                    if (preserveDrawOrder)
                    {
                        if (groups.empty() || groups.back().shader != shader || groups.back().cullMode != cullMode)
                        {
                            groups.emplace_back();
                        }
                        groupIndex = static_cast<uint32_t>(groups.size() - 1);
                    }
                    GBufferDrawGroup& group = groups[groupIndex];
                    group.shader            = shader;
                    group.cullMode          = cullMode;
                    GBufferDrawItem item{{instance, index, mesh->GetIndexCount(), mesh->GetFirstIndex()}};
                    if (indirect)
                    {
                        item.bounds = scene.GetSubMeshWorldBounds(instance, subMeshIndex);
                    }
                    else if (hybrid != nullptr)
                    {
                        item.receiver = hybrid->ReceiverDraw(instance, index);
                    }
                    group.items.push_back(item);
                }
            }
        }
    }
    if (indirect && !preserveDrawOrder)
    {
        for (GBufferDrawGroup& group : groups)
        {
            GroupDrawsByFirstMaterialUse(group);
        }
    }
    return groups;
}

void RecordHybridDraws(RDGPassCmdEncoder& encoder, const HeapVector<GBufferDrawItem>& items)
{
    for (const GBufferDrawItem& item : items)
    {
        encoder.SetPushConstants(item.receiver);
        encoder.DrawIndexed(item.draw.indexCount, 1, item.draw.firstIndex, 0, 0);
    }
}

void RecordGBufferGroups(RDGPassCmdEncoder& encoder, const GBufferDrawGroups& groups, bool hybrid)
{
    for (const GBufferDrawGroup& group : groups)
    {
        if (!group.items.empty())
        {
            encoder.SetPipelineVariant(group.pipelineVariant);
            if (hybrid)
            {
                RecordHybridDraws(encoder, group.items);
            }
            else
            {
                RecordGBufferDraws(encoder, group.items);
            }
        }
    }
}

uint32_t DeclareGBufferPipeline(RDGGraphicsPassDesc& desc, NameID shader, RHIPolygonCullMode cullMode)
{
    uint32_t pipelineVariant = 0;
    if (shader != desc.shaderProgramName || cullMode != desc.pipelineStates.rasterizationState.cullMode)
    {
        // Ordered captures may revisit a group; reuse its declared pipeline.
        for (uint32_t index = 0; index < desc.pipelineVariants.size(); ++index)
        {
            const RDGGraphicsPipelineVariant& variant = desc.pipelineVariants[index];
            if (variant.shaderProgramName == shader && variant.cullMode == cullMode)
            {
                pipelineVariant = index + 1;
                break;
            }
        }
        if (pipelineVariant == 0)
        {
            pipelineVariant = desc.AddPipelineVariant(shader, cullMode);
        }
    }
    return pipelineVariant;
}

void DeclareGBufferPipelines(RDGGraphicsPassDesc& desc, GBufferDrawGroups& groups)
{
    for (GBufferDrawGroup& group : groups)
    {
        if (!group.items.empty())
        {
            group.pipelineVariant = DeclareGBufferPipeline(desc, group.shader, group.cullMode);
        }
    }
}

struct GBufferIndirectBatch
{
    uint32_t pipelineVariant;
    uint32_t materialIndex;
    uint32_t firstDraw;
    uint32_t drawCount;
};

void RecordCulledGBufferDraws(RDGPassCmdEncoder&                      encoder,
                              const HeapVector<GBufferIndirectBatch>& batches,
                              RDGBuffer                               arguments,
                              uint32_t                                maxDrawCount)
{
    for (const GBufferIndirectBatch& batch : batches)
    {
        encoder.SetPipelineVariant(batch.pipelineVariant);
        // A push constant keeps material reads and branches uniform across the batch.
        encoder.SetPushConstants(batch.materialIndex);
        uint32_t first     = batch.firstDraw;
        uint32_t remaining = batch.drawCount;
        while (remaining != 0)
        {
            const uint32_t count = std::min(remaining, maxDrawCount);
            encoder.DrawIndexedIndirect(arguments, first * sizeof(DrawIndexedIndirectCommand), count,
                                        sizeof(DrawIndexedIndirectCommand));
            first     += count;
            remaining -= count;
        }
    }
}

HeapVector<SceneCullingBatch> BuildIndirectBatches(const GBufferDrawGroups& groups)
{
    HeapVector<SceneCullingBatch> batches;
    uint32_t                      first = 0;
    for (const GBufferDrawGroup& group : groups)
    {
        for (const GBufferDrawItem& item : group.items)
        {
            const uint32_t materialIndex = item.draw.materialIndex;
            if (batches.empty() || batches.back().shader != group.shader || batches.back().cullMode != group.cullMode
                || batches.back().materialIndex != materialIndex)
            {
                batches.push_back({group.shader, group.cullMode, materialIndex, first, 1});
            }
            else
            {
                ++batches.back().drawCount;
            }
            ++first;
        }
    }
    return batches;
}

HeapVector<GBufferIndirectBatch> DeclareIndirectBatches(RDGGraphicsPassDesc& desc, const HeapVector<SceneCullingBatch>& batches)
{
    HeapVector<GBufferIndirectBatch> declared;
    declared.reserve(batches.size());
    for (const SceneCullingBatch& batch : batches)
    {
        declared.push_back({DeclareGBufferPipeline(desc, batch.shader, batch.cullMode), batch.materialIndex, batch.firstDraw,
                            batch.drawCount});
    }
    return declared;
}

SceneCullingStatus ValidateCullingDevice(const RHIGPUInfo& gpu, bool hybrid)
{
    SceneCullingStatus status = SceneCullingStatus::eCulled;
    if (!SupportsGBufferCulling(gpu))
    {
        status = SceneCullingStatus::eUnsupportedFirstInstance;
    }
    else if (hybrid && !SupportsHybridGBufferCulling(gpu))
    {
        status = SceneCullingStatus::eVaryingLimit;
    }
    return status;
}

SceneCullingStatus ValidateCullingResources(uint64_t candidateCount, const RHIGPUInfo& gpu, uint64_t& bytes)
{
    SceneCullingStatus status = SceneCullingStatus::eCulled;
    if (candidateCount == 0)
    {
        status = SceneCullingStatus::eEmpty;
    }
    else if (ValidateGIStorageBuffer(candidateCount, sizeof(GBufferDrawData), gpu, bytes) != GIResourceStatus::eSuccess)
    {
        status = SceneCullingStatus::eBufferLimit;
    }
    // Draw records and previous matrices are both 64 bytes. Indirect arguments
    // are smaller (20 bytes), so the same validation covers all three buffers.
    static_assert(sizeof(GBufferDrawData) == sizeof(Mat4));
    static_assert(sizeof(DrawIndexedIndirectCommand) < sizeof(GBufferDrawData));
    return status;
}

bool BuildCullingDispatches(uint32_t count, const RHIGPUInfo& gpu, HeapVector<ComputeDispatchChunk>& chunks)
{
    bool     valid = true;
    uint32_t first = 0;
    while (valid && first < count)
    {
        ComputeDispatchChunk chunk;
        valid = BuildComputeDispatchChunk(first, count - first, ZEN_SCENE_CULL_GROUP_SIZE, gpu, chunk);
        if (valid)
        {
            chunks.push_back(chunk);
            first += chunk.itemCount;
        }
    }
    return valid;
}

HeapVector<GBufferDrawData> BuildGBufferDrawRecords(const GBufferDrawGroups& groups, uint32_t count)
{
    HeapVector<GBufferDrawData> data;
    data.reserve(count);
    for (const GBufferDrawGroup& group : groups)
    {
        for (const GBufferDrawItem& item : group.items)
        {
            data.push_back({Vec4(item.bounds.GetMin(), 0.0f), Vec4(item.bounds.GetMax(), 0.0f),
                            glm::uvec4(item.draw.indexCount, item.draw.firstIndex, 0, 0),
                            glm::uvec4(item.draw.nodeIndex, 0, 0, 0)});
        }
    }
    return data;
}

// Previous transforms change without a committed revision, so they are rebuilt every frame.
HeapVector<Mat4> BuildPreviousModels(const HeapVector<GBufferDrawData>& records, const HybridGIRenderer& hybrid)
{
    HeapVector<Mat4> models;
    models.reserve(records.size());
    for (const GBufferDrawData& record : records)
    {
        models.push_back(hybrid.PreviousModel(record.indices.x));
    }
    return models;
}

template <typename T> bool UploadCullingData(RenderDevice&    device,
                                             FrameBufferRing& buffer,
                                             HeapVector<T>&   contents,
                                             HeapVector<T>&   data,
                                             uint32_t         bytes,
                                             NameID           name,
                                             uint64_t&        uploadedBytes)
{
    const bool changed = contents.size() != data.size() || std::memcmp(contents.data(), data.data(), bytes) != 0;
    if (changed)
    {
        buffer.MarkDirty(0, bytes);
    }
    const uint64_t previousGeneration = buffer.GetGeneration();
    const bool     valid              = buffer.Update(device, data.data(), bytes, name);
    if (valid)
    {
        if (changed)
        {
            contents = std::move(data);
        }
        if (buffer.GetGeneration() != previousGeneration)
        {
            uploadedBytes += bytes;
        }
    }
    return valid;
}

void RecordCullingDispatches(RDGPassCmdEncoder& encoder, const HeapVector<ComputeDispatchChunk>& chunks)
{
    for (const ComputeDispatchChunk& chunk : chunks)
    {
        const glm::uvec4 range(chunk.firstItem, chunk.itemCount, 0, 0);
        encoder.SetPushConstants(range);
        encoder.Dispatch(chunk.groups.x, chunk.groups.y, chunk.groups.z);
    }
}

void AddGBufferCullPass(RenderGraph&                     graph,
                        const RenderScene&               scene,
                        RHIBuffer*                       drawData,
                        RDGBuffer                        arguments,
                        HeapVector<ComputeDispatchChunk> chunks)
{
    const sg::CameraUniformData& camera = *reinterpret_cast<const sg::CameraUniformData*>(scene.GetCameraUniformData());
    const FrustumCullUniformData uniforms{camera.projViewMatrix};
    RDGComputePassDesc           cull;
    cull.SetShaderProgramName("SceneFrustumCullSP");
    cull.SetPassTag("GBufferFrustumCull");
    // Each chunk writes a disjoint command range and only reads scene metadata.
    cull.independentDispatches = true;
    // Do not request async compute: graphics immediately consumes the arguments.
    cull.BindValue("uFrustumCull", uniforms);
    cull.BindStorageBuffer("GBufferDrawBuffer", drawData);
    // All chunks belong to one pass and collectively write every command slot.
    cull.BindStorageBuffer("IndirectBuffer", arguments, RDGContentGuarantee::eFullWrite);
    graph.AddComputePass(std::move(cull)).RecordPassCommands([chunks = std::move(chunks)](RDGPassCmdEncoder& encoder) {
        RecordCullingDispatches(encoder, chunks);
    });
}
} // namespace

void AddGBufferPass(RenderGraph&            graph,
                    RDGGraphicsPassDesc     desc,
                    const RenderScene&      scene,
                    const HybridGIRenderer* hybrid,
                    bool                    preserveDrawOrder)
{
    desc.SetShaderProgramName(GBufferShaderName(hybrid != nullptr, false, false, scene.GetUVSetCount()));
    GBufferDrawGroups groups = GroupGBufferDraws(scene, hybrid, preserveDrawOrder);
    DeclareGBufferPipelines(desc, groups);
    graph.AddGraphicsPass(std::move(desc))
        .RecordPassCommands([groups = std::move(groups), isHybrid = hybrid != nullptr](RDGPassCmdEncoder& encoder) {
            RecordGBufferGroups(encoder, groups, isHybrid);
        });
}

SceneCullingResult AddCulledGBufferPass(RenderDevice&           device,
                                        SceneCullingDrawCache&  cache,
                                        RenderGraph&            graph,
                                        RDGGraphicsPassDesc&    desc,
                                        const RenderScene&      scene,
                                        const HybridGIRenderer* hybrid,
                                        bool                    preserveDrawOrder,
                                        RDGBuffer*              pArguments)
{
    const RHIGPUInfo&  gpu = device.GetGPUInfo();
    SceneCullingResult result;
    result.status = ValidateCullingDevice(gpu, hybrid != nullptr);
    const SceneCullingDrawKey key{scene.GetIdentity(),        scene.GetGeometryRevision(),
                                  scene.GetSurfaceRevision(), scene.GetRenderableNodes().size(),
                                  hybrid != nullptr,          preserveDrawOrder};
    const bool                reuseDraws = cache.key == key;
    // Check the device first: the direct path groups draws again for itself.
    GBufferDrawGroups                groups;
    uint64_t                         bytes = 0;
    HeapVector<ComputeDispatchChunk> chunks;
    if (result.status == SceneCullingStatus::eCulled)
    {
        uint64_t candidateCount = cache.contents.size();
        if (!reuseDraws)
        {
            groups         = GroupGBufferDraws(scene, hybrid, preserveDrawOrder, true);
            candidateCount = 0;
            for (const GBufferDrawGroup& group : groups)
            {
                candidateCount += group.items.size();
            }
        }
        result.status         = ValidateCullingResources(candidateCount, gpu, bytes);
        result.candidateCount = static_cast<uint32_t>(std::min(candidateCount, uint64_t(UINT32_MAX)));
    }
    if (result.status == SceneCullingStatus::eCulled && !BuildCullingDispatches(result.candidateCount, gpu, chunks))
    {
        result.status = SceneCullingStatus::eDispatchLimit;
    }
    if (result.status == SceneCullingStatus::eCulled)
    {
        bool uploaded = true;
        if (!reuseDraws)
        {
            HeapVector<GBufferDrawData> data = BuildGBufferDrawRecords(groups, result.candidateCount);
            uploaded = UploadCullingData(device, cache.draws, cache.contents, data, static_cast<uint32_t>(bytes),
                                         "gbuffer_draw_data", result.uploadedBytes);
            if (uploaded)
            {
                cache.batches = BuildIndirectBatches(groups);
                cache.key     = key;
            }
        }
        if (uploaded && hybrid != nullptr)
        {
            HeapVector<Mat4> previousModels = BuildPreviousModels(cache.contents, *hybrid);
            uploaded = UploadCullingData(device, cache.previousModels, cache.previousModelContents, previousModels,
                                         static_cast<uint32_t>(bytes), "gbuffer_previous_models", result.uploadedBytes);
        }
        if (!uploaded)
        {
            result.status = SceneCullingStatus::eUploadFailed;
        }
    }
    RDGBuffer arguments;
    if (result.status == SceneCullingStatus::eCulled)
    {
        RDGBufferDesc bufferDesc;
        bufferDesc.name = "gbuffer_indirect_arguments";
        bufferDesc.size = uint64_t(result.candidateCount) * sizeof(DrawIndexedIndirectCommand);
        bufferDesc.usageFlags.SetFlags(RHIBufferUsageFlagBits::eStorageBuffer, RHIBufferUsageFlagBits::eIndirectBuffer);
        arguments = graph.GetResourceManager()->CreateBuffer(bufferDesc);
        if (!arguments)
        {
            result.status = SceneCullingStatus::eAllocationFailed;
        }
    }
    if (result.status == SceneCullingStatus::eCulled)
    {
        if (pArguments != nullptr)
        {
            *pArguments = arguments;
        }
        AddGBufferCullPass(graph, scene, cache.draws.GetBuffer(), arguments, std::move(chunks));
        desc.SetShaderProgramName(GBufferShaderName(hybrid != nullptr, false, true, scene.GetUVSetCount()));
        desc.BindStorageBuffer("GBufferDrawBuffer", cache.draws.GetBuffer());
        if (hybrid != nullptr)
        {
            desc.BindStorageBuffer("GBufferPreviousModels", cache.previousModels.GetBuffer());
        }
        desc.UseIndirectBuffer(arguments);
        HeapVector<GBufferIndirectBatch> batches      = DeclareIndirectBatches(desc, cache.batches);
        const uint32_t                   maxDrawCount = gpu.supportMultiDrawIndirect ? gpu.maxDrawIndirectCount : 1;
        graph.AddGraphicsPass(std::move(desc))
            .RecordPassCommands([batches = std::move(batches), arguments, maxDrawCount](RDGPassCmdEncoder& encoder) {
                RecordCulledGBufferDraws(encoder, batches, arguments, maxDrawCount);
            });
    }
    return result;
}

void SceneCullingDrawCache::Destroy(RenderDevice& device)
{
    draws.Destroy(device);
    previousModels.Destroy(device);
    contents.clear();
    previousModelContents.clear();
    batches.clear();
    key = {};
}

const char* SceneCullingStatusName(SceneCullingStatus status)
{
    const char* name = "not_used";
    switch (status)
    {
        case SceneCullingStatus::eNotUsed: break;
        case SceneCullingStatus::eDisabled: name = "direct_disabled"; break;
        case SceneCullingStatus::eEmpty: name = "empty"; break;
        case SceneCullingStatus::eCulled: name = "gpu_indirect"; break;
        case SceneCullingStatus::eUnsupportedFirstInstance: name = "direct_unsupported_first_instance"; break;
        case SceneCullingStatus::eVaryingLimit: name = "direct_varying_limit"; break;
        case SceneCullingStatus::eBufferLimit: name = "direct_buffer_limit"; break;
        case SceneCullingStatus::eDispatchLimit: name = "direct_dispatch_limit"; break;
        case SceneCullingStatus::eUploadFailed: name = "direct_upload_failed"; break;
        case SceneCullingStatus::eAllocationFailed: name = "direct_allocation_failed"; break;
    }
    return name;
}

const char* SceneCullingFallbackReason(SceneCullingStatus status)
{
    const char* reason = "";
    switch (status)
    {
        case SceneCullingStatus::eNotUsed:
        case SceneCullingStatus::eDisabled:
        case SceneCullingStatus::eEmpty:
        case SceneCullingStatus::eCulled: break;
        case SceneCullingStatus::eUnsupportedFirstInstance:
            reason = "Indexed indirect first-instance drawing is unavailable.";
            break;
        case SceneCullingStatus::eVaryingLimit:
            reason = "Indirect hybrid GBuffer shader interfaces exceed device component limits.";
            break;
        case SceneCullingStatus::eBufferLimit: reason = "Culling data exceeds storage-buffer size or alignment limits."; break;
        case SceneCullingStatus::eDispatchLimit: reason = "Culling workgroups exceed device compute limits."; break;
        case SceneCullingStatus::eUploadFailed: reason = "Culling metadata allocation or upload failed."; break;
        case SceneCullingStatus::eAllocationFailed: reason = "Culling argument-buffer declaration failed."; break;
    }
    return reason;
}

namespace
{
struct ForwardPushConstants
{
    uint32_t nodeIndex;
    uint32_t materialIndex;
    uint32_t topology;
};

void SetForwardConstants(RDGPassCmdEncoder& encoder, const ForwardPushConstants& constants, glm::uvec2 captureExtent)
{
    if (captureExtent.x != 0)
    {
        struct CaptureConstants
        {
            ForwardPushConstants draw;
            uint32_t             padding;
            glm::uvec2           extent;
        };
        encoder.SetPushConstants(CaptureConstants{constants, 0, captureExtent});
    }
    else
    {
        encoder.SetPushConstants(constants);
    }
}

void RecordForwardDraws(RDGPassCmdEncoder&               encoder,
                        const HeapVector<SceneMeshDraw>& draws,
                        bool                             displayLinear = false,
                        glm::uvec2                       captureExtent = glm::uvec2(0))
{
    for (const SceneMeshDraw& draw : draws)
    {
        const ForwardPushConstants constants{draw.nodeIndex, draw.materialIndex,
                                             static_cast<uint32_t>(draw.topology) | (displayLinear ? 4u : 0u)};

        SetForwardConstants(encoder, constants, captureExtent);
        encoder.DrawIndexed(draw.indexCount, 1, draw.firstIndex, 0, 0);
    }
}

RHIDrawPrimitiveType ForwardTopology(sg::MeshTopology topology)
{
    RHIDrawPrimitiveType result = RHIDrawPrimitiveType::eTriangleList;

    if (topology == sg::MeshTopology::Points)
    {
        result = RHIDrawPrimitiveType::ePointList;
    }
    else if (topology == sg::MeshTopology::Lines)
    {
        result = RHIDrawPrimitiveType::eLineList;
    }

    return result;
}

void RecordLightingCaptureClear(RDGPassCmdEncoder& encoder, const HeapVector<ComputeDispatchChunk>& chunks)
{
    for (const ComputeDispatchChunk& chunk : chunks)
    {
        encoder.SetPushConstants(glm::uvec2(chunk.firstItem, chunk.itemCount));

        encoder.Dispatch(chunk.groups.x, chunk.groups.y, chunk.groups.z);
    }
}
} // namespace

DeferredLightingRenderer::DeferredLightingRenderer(RenderDevice* pRenderDevice) : m_pRenderDevice(pRenderDevice) {}

void DeferredLightingRenderer::Init()
{
    const platform::ConfigLoader& config = platform::ConfigLoader::GetInstance();

    bool markersEnabled                  = false;

    float markerSize                     = 0.02f;

    const bool valid                     = config.ReadBool("light_markers.enabled", markersEnabled)
                                        && config.ReadNumber("light_markers.size", markerSize) && std::isfinite(markerSize) && markerSize > 0.0f;

    Init(valid && markersEnabled, valid ? markerSize : 0.02f);

    if (!valid)
    {
        LOGW("Invalid light_markers configuration; markers disabled");
    }
}

void DeferredLightingRenderer::Init(bool markersEnabled, float markerSize)
{
    PrepareSamplers();
    SetLightMarkers(markersEnabled, markerSize);
}

void DeferredLightingRenderer::Destroy()
{
    for (const HybridView& view : m_hybridViews)
    {
        view.renderer->Destroy();
        ZEN_DELETE(view.renderer);
    }
    m_hybridViews.clear();
    m_hybrid = nullptr;
    if (m_pRenderDevice != nullptr)
    {
        m_sortedBlendIndices.Destroy(*m_pRenderDevice);
        m_forwardGPUDraws.Destroy(*m_pRenderDevice);
        m_gbufferDrawData.Destroy(*m_pRenderDevice);
    }
    m_forwardDraws.Clear();
}

void DeferredLightingRenderer::SetRenderScene(RenderScene* scene)
{
    for (const HybridView& view : m_hybridViews)
    {
        view.renderer->Destroy();
    }
    m_pScene = scene;
}

void DeferredLightingRenderer::OnRenderGraphExecuted(RenderFrameResult result)
{
    m_forwardGPUDraws.OnExecuted(result == RenderFrameResult::eSubmitted);
    if (m_hybrid != nullptr)
    {
        m_hybrid->OnRenderGraphExecuted(result);
    }
}

bool DeferredLightingRenderer::SetLightMarkers(bool enabled, float size)
{
    const bool valid = std::isfinite(size) && size > 0.0f;

    if (valid)
    {
        m_lightMarkersEnabled = enabled;

        m_lightMarkerSize     = size;
    }

    return valid;
}

void DeferredLightingRenderer::PrepareSamplers()
{
    {
        RHISamplerCreateInfo samplerInfo{};

        samplerInfo.borderColor = RHISamplerBorderColor::eFloatOpaqueWhite;

        m_pDepthSampler         = m_pRenderDevice->CreateSampler(samplerInfo);
    }

    const RHISamplerCreateInfo colorSamplerInfo  = RHISamplerCreateInfo::CreateLinearRepeat();

    m_pColorSampler                              = m_pRenderDevice->CreateSampler(colorSamplerInfo);

    RHISamplerCreateInfo transmissionSamplerInfo = RHISamplerCreateInfo::CreateLinearRepeat();

    transmissionSamplerInfo.repeatU              = RHISamplerRepeatMode::eClampToEdge;

    transmissionSamplerInfo.repeatV              = RHISamplerRepeatMode::eClampToEdge;

    m_pTransmissionSampler                       = m_pRenderDevice->CreateSampler(transmissionSamplerInfo);
}

bool DeferredLightingRenderer::BuildLightingCaptureClear(const RenderView& view)
{
    const uint64_t pixels = uint64_t(view.GetWidth()) * view.GetHeight();

    uint64_t bytes        = 0;

    const RHIGPUInfo& gpu = m_pRenderDevice->GetGPUInfo();

    bool valid            = pixels != 0 && gpu.supportFragmentStoresAndAtomics
                         && ValidateGIStorageBuffer(pixels, ZEN_LIGHTING_CAPTURE_BYTES_PER_PIXEL, gpu, bytes) == GIResourceStatus::eSuccess
                         && m_captureOutput != nullptr && m_captureReadback != nullptr && m_captureOutput->GetRequiredSize() == bytes
                         && m_captureReadback->GetRequiredSize() == bytes;

    HeapVector<ComputeDispatchChunk> chunks;

    uint32_t first = 0;

    while (valid && first < pixels)
    {
        ComputeDispatchChunk chunk;

        valid = BuildComputeDispatchChunk(first, static_cast<uint32_t>(pixels) - first, ZEN_LIGHTING_CAPTURE_GROUP_SIZE, gpu,
                                          chunk);

        if (valid)
        {
            chunks.push_back(chunk);

            first += chunk.itemCount;
        }
    }

    if (valid)
    {
        RDGComputePassDesc clear;

        clear.SetShaderProgramName("ClearLightingCaptureSP");

        clear.SetPassTag("ClearLightingCapture");

        clear.independentDispatches = true;

        clear.BindStorageBuffer("LightingCapture", m_captureOutput, RDGContentGuarantee::eFullWrite);

        m_pRenderDevice->GetCurrentFrameRDG()
            ->AddComputePass(std::move(clear))
            .RecordPassCommands([chunks](RDGPassCmdEncoder& encoder) { RecordLightingCaptureClear(encoder, chunks); });
    }

    return valid;
}

void DeferredLightingRenderer::BuildRenderGraph(const RenderView& view, VoxelGIRenderer* voxelGI, SceneShadowRenderer* shadows)
{
    BuildGBufferGraph(view, voxelGI != nullptr);

    BuildCompositionGraph(view, voxelGI, shadows);
}

void DeferredLightingRenderer::BuildGBufferGraph(const RenderView& view, bool hybrid)
{
    const SceneCullingStatus previousCullingStatus = m_gpuCullingStatus.status;
    m_gpuCullingStatus                             = {};
    RenderGraph* pRDG                              = m_pRenderDevice->GetCurrentFrameRDG();

    VERIFY_EXPR(pRDG != nullptr && m_pScene != nullptr);

    const uint32_t width  = view.GetWidth();

    const uint32_t height = view.GetHeight();

    // One texel per screen pixel: the lighting pass reads the G-buffer at its own fragment
    // coordinate. A suspended (zero-sized) view renders nothing and declares no G-buffer.
    m_hybrid = nullptr;
    if (hybrid)
    {
        for (const HybridView& entry : m_hybridViews)
        {
            if (entry.id == view.historyId)
            {
                m_hybrid = entry.renderer;
            }
        }
        if (m_hybrid == nullptr)
        {
            m_hybrid = ZEN_NEW() HybridGIRenderer(m_pRenderDevice);
            m_hybridViews.push_back({view.historyId, m_hybrid});
        }
        m_hybrid->SetDeterministicGuideCapture(m_deterministicGuideCapture);
        m_hybrid->Prepare(view, *m_pScene);
        m_hybrid->SetCapture(m_hybridCaptureOutput, m_hybridCaptureReadback);
    }
    else
    {
        for (const HybridView& entry : m_hybridViews)
        {
            if (entry.id == view.historyId)
            {
                entry.renderer->InvalidateHistory("inactive");
            }
        }
    }
    // Scenes whose translucent layers alone need forward shading still light opaque surfaces here.
    const bool declared = width != 0 && height != 0 && (hybrid || ShadesOpaqueDeferred());

    m_gbufferExtent     = declared ? glm::uvec2(width, height) : glm::uvec2(0, 0);

    if (declared)
    {
        RHIGfxPipelineStates pso{};

        pso.rasterizationState          = {};

        pso.rasterizationState.cullMode = RHIPolygonCullMode::eDisabled;

        pso.depthStencilState           = RHIGfxPipelineDepthStencilState::Create(true, true, RHIDepthCompareOperator::eLess);

        pso.multiSampleState            = {};

        pso.colorBlendState.AddAttachments(hybrid ? 7 : 4);

        pso.dynamicStates.Enable(RHIDynamicState::eScissor, RHIDynamicState::eViewPort);

        RDGGraphicsPassDesc offscreen{};

        offscreen.AddColorOutput(DataFormat::eR16G16UNORM, width, height, "offscreen_normal");

        offscreen.AddColorOutput(DataFormat::eR8G8B8A8UNORM, width, height, "offscreen_albedo");

        offscreen.AddColorOutput(DataFormat::eR8G8B8A8UNORM, width, height, "offscreen_roughness");

        offscreen.AddColorOutput(DataFormat::eR16G16B16A16SFloat, width, height, "offscreen_emissive_occlusion");

        if (hybrid)
        {
            offscreen.AddColorOutput(DataFormat::eR32G32UInt, width, height, "offscreen_receiver");
            offscreen.AddColorOutput(DataFormat::eR32G32B32A32SFloat, width, height, "offscreen_motion");
            offscreen.AddColorOutput(DataFormat::eR32G32B32A32SFloat, width, height, "offscreen_position");
            m_hybrid->BindReceiverInputs(offscreen);
        }

        offscreen.AddDepthStencilOutput(view.GetDepthStencilFormat(), width, height, "offscreen_depth",
                                        RHIRenderTargetLoadOp::eClear, RHIRenderTargetStoreOp::eStore);

        offscreen.SetPipelineStates(pso);

        offscreen.SetRenderArea(0, 0, width, height);

        offscreen.SetPassTag("OffScreen");

        offscreen.BindStorageBuffer("NodeBuffer", m_pScene->GetNodesDataSSBO());

        offscreen.BindStorageBuffer("MaterialBuffer", m_pScene->GetMaterialsDataSSBO());

        offscreen.BindStorageBuffer("UVBuffer", m_pScene->GetUVBuffer());

        BindSceneTextureArray(offscreen, m_pColorSampler, m_pScene->GetSceneTextureViews(), m_pScene->GetSceneSamplers());

        offscreen.BindValue("uCameraData", m_pScene->GetCameraUniformData(), sizeof(sg::CameraUniformData));

        offscreen.BindVertexBuffer(m_pScene->GetVertexBuffer());

        offscreen.BindIndexBuffer(m_pScene->GetIndexBuffer());

        m_gpuCullingStatus = m_gpuCullingEnabled ? AddCulledGBufferPass(*m_pRenderDevice, m_gbufferDrawData, *pRDG, offscreen,
                                                                        *m_pScene, m_hybrid)
                                                 : SceneCullingResult{SceneCullingStatus::eDisabled};
        if (m_gpuCullingStatus.status != SceneCullingStatus::eCulled)
        {
            AddGBufferPass(*pRDG, std::move(offscreen), *m_pScene, m_hybrid);
        }
        const char* fallbackReason = SceneCullingFallbackReason(m_gpuCullingStatus.status);
        if (m_gpuCullingStatus.status != previousCullingStatus && fallbackReason[0] != '\0')
        {
            LOGW("GPU culling fell back to direct drawing: {}", fallbackReason);
        }
    }
}

DebugOutputDescription DeferredLightingRenderer::BuildDebugView(const RenderView& view, const DebugSelection& selection)
{
    DebugOutputDescription description;

    description.output    = selection.output;

    description.width     = view.width;

    description.height    = view.height;

    const bool depth      = selection.output == DebugOutput::eDepth;

    description.available = m_pScene != nullptr && view.width > 0 && view.height > 0
                         && (depth || m_gbufferExtent == glm::uvec2(view.width, view.height));

    description.reason = description.available
                           ? ""
                           : "Opaque materials of this scene need forward shading (material extensions); it has no G-buffer.";

    description.format = depth                                    ? view.GetDepthStencilFormat()
                       : selection.output == DebugOutput::eNormal ? DataFormat::eR16G16UNORM
                                                                  : DataFormat::eR8G8B8A8UNORM;

    description.interpretation = depth ? "Depth of opaque/masked surfaces: raw device Z or linear view distance."
                               : selection.output == DebugOutput::eNormal ? "World-space normal mapped from [-1,1] to RGB."
                                                                          : "Linear base color encoded as sRGB; no lighting.";

    if (description.available)
    {
        const sg::CameraUniformData& camera = *reinterpret_cast<const sg::CameraUniformData*>(m_pScene->GetCameraUniformData());

        DebugVisualizationData data;

        data.inverseProjection   = glm::inverse(camera.proj);

        data.range               = Vec4(selection.minimum, selection.maximum, selection.linearDepth ? 1.0f : 0.0f, 0);

        data.selection.x         = depth ? 1 : selection.output == DebugOutput::eNormal ? 3 : 2;

        const bool normal        = selection.output == DebugOutput::eNormal;

        RDGGraphicsPassDesc pass = MakeDebugVisualizationPass(view, normal ? "RenderDebugNormalSP" : "RenderDebug2DSP", data);

        if (depth)
        {
            pass.BindSampledTexture("sourceImage", m_pDepthSampler, view.depth->GetDefaultView());
        }
        else
        {
            pass.BindSampledTexture("sourceImage", m_pDepthSampler,
                                    NameID(selection.output == DebugOutput::eNormal ? "offscreen_normal" : "offscreen_albedo"));
        }

        if (normal)
        {
            pass.BindSampledTexture("depthMap", m_pDepthSampler, "offscreen_depth");
        }

        AddDebugVisualizationPass(*m_pRenderDevice->GetCurrentFrameRDG(), std::move(pass));
    }

    return description;
}

void DeferredLightingRenderer::BuildCompositionGraph(const RenderView&    view,
                                                     VoxelGIRenderer*     voxelGI,
                                                     SceneShadowRenderer* shadows)
{
    if (m_hybrid != nullptr && voxelGI != nullptr && !m_hybrid->BuildRenderGraph(view, *voxelGI))
    {
        LOGE("Hybrid GI history allocation failed; rendering the minimum PBR tier");
        voxelGI = nullptr;
        shadows = nullptr;
    }
    if (UsesForwardMaterials())
    {
        BuildForwardGraph(view, voxelGI, shadows);
    }
    else
    {
        RenderGraph* pRDG = m_pRenderDevice->GetCurrentFrameRDG();

        VERIFY_EXPR(pRDG != nullptr && m_pScene != nullptr);

        m_captureRecorded       = false;

        const bool capture      = m_captureOutput != nullptr;

        const bool captureValid = !capture || BuildLightingCaptureClear(view);

        AddLightingPass(view, voxelGI, shadows, capture, captureValid, nullptr, nullptr);

        if (capture && captureValid)
        {
            pRDG->AddTransferPass("ReadLightingCapture")
                .CopyBuffer(m_captureOutput, m_captureReadback, {0, 0, m_captureOutput->GetRequiredSize()})
                .NeverCull();

            m_captureRecorded = true;
        }

        BuildLightMarkers(view);
    }
    if (voxelGI != nullptr)
    {
        voxelGI->ReadRayTotals();
    }
}

void DeferredLightingRenderer::AddLightingPass(const RenderView&    view,
                                               VoxelGIRenderer*     voxelGI,
                                               SceneShadowRenderer* shadows,
                                               bool                 capture,
                                               bool                 captureValid,
                                               const RDGTexture*    linearTarget,
                                               const RDGTexture*    lightVisibility)
{
    RenderGraph* graph = m_pRenderDevice->GetCurrentFrameRDG();

    const glm::uvec2 captureExtent(view.GetWidth(), view.GetHeight());

    const bool rayShadows = voxelGI != nullptr && voxelGI->UsesRayShadows();

    const bool hybrid     = m_hybrid != nullptr && m_hybrid->IsActive();

    // Ray-shadowed and shadow-mapped hybrid receivers export exact shadow results for resting layers.
    const bool visibility = lightVisibility != nullptr && (rayShadows || (hybrid && !capture));

    RHIGfxPipelineStates pso{};

    pso.primitiveType      = RHIDrawPrimitiveType::eTriangleList;

    pso.rasterizationState = {};

    pso.multiSampleState   = {};

    pso.dynamicStates.Enable(RHIDynamicState::eScissor, RHIDynamicState::eViewPort);

    pso.colorBlendState.AddAttachments(visibility ? 2 : 1);

    pso.depthStencilState = RHIGfxPipelineDepthStencilState::Create(true, true, RHIDepthCompareOperator::eLessOrEqual);

    RDGGraphicsPassDesc lighting{};

    // Without GI, scene shadow maps still shadow direct light when the frame prepared them.
    const bool mapShadowedPBR = voxelGI == nullptr && shadows != nullptr && !capture;
    lighting.SetShaderProgramName(voxelGI == nullptr ? (capture          ? "DeferredLightingCaptureSP"
                                                        : mapShadowedPBR ? "DeferredShadowMapsSP"
                                                                         : "DeferredLightingSP")
                                                     : (capture ? "DeferredVoxelGICaptureSP" : "DeferredVoxelGISP"));

    if (hybrid)
    {
        lighting.SetShaderProgramName(capture      ? "DeferredHybridGICaptureSP"
                                      : visibility ? "DeferredHybridGIVisibilitySP"
                                                   : "DeferredHybridGISP");
        m_hybrid->BindLightingInputs(lighting);
    }
    if (rayShadows)
    {
        // Ray-count diagnostics never coincide with a lighting capture.
        const bool counts = !capture && voxelGI->CountsRays();
        // Ray shadows are HW RT; Compute uses shadow maps. [plain, capture, counts][color only, with receiver visibility]
        static const char* const programs[3][2] = {
            {"DeferredHardwareShadowsSP", "DeferredHardwareShadowsVisibilitySP"},
            {"DeferredHardwareShadowsCaptureSP", "DeferredHardwareShadowsCaptureVisibilitySP"},
            {"DeferredHardwareShadowsCountsSP", "DeferredHardwareShadowsCountsVisibilitySP"}};
        const uint32_t variant = capture ? 1 : counts ? 2 : 0;
        lighting.SetShaderProgramName(programs[variant][visibility ? 1 : 0]);
        lighting.BindSampledTexture("shadowReceiverPosition", m_pDepthSampler, "offscreen_position");
        if (counts)
        {
            voxelGI->BindRayTotals(lighting);
        }
    }

    if (linearTarget != nullptr)
    {
        // The forward background already wrote the G-buffer depth that this pass writes again.
        lighting.AddColorOutput(*linearTarget, RHIRenderTargetLoadOp::eLoad);

        lighting.AddDepthStencilOutput(view.GetDepthTarget(), RHIRenderTargetLoadOp::eLoad, RHIRenderTargetStoreOp::eStore);
    }
    else
    {
        lighting.AddColorOutput(view.GetColorTarget(), RHIRenderTargetLoadOp::eLoad);

        lighting.AddDepthStencilOutput(view.GetDepthTarget(), RHIRenderTargetLoadOp::eClear, RHIRenderTargetStoreOp::eStore);
    }

    if (visibility)
    {
        lighting.AddColorOutput(*lightVisibility, RHIRenderTargetLoadOp::eClear);
    }

    lighting.SetPipelineStates(pso);

    lighting.SetRenderArea(0, 0, view.GetWidth(), view.GetHeight());

    lighting.SetPassTag("SceneLighting");

    const EnvTexture& env               = m_pScene->GetEnvTexture();

    const sg::CameraUniformData& camera = *reinterpret_cast<const sg::CameraUniformData*>(m_pScene->GetCameraUniformData());

    GBufferUniformData gbuffer          = BuildGBufferUniformData(camera.projViewMatrix);

    gbuffer.worldOrigin.w               = linearTarget != nullptr ? 1.0f : 0.0f;

    lighting.BindValue("uGBufferData", gbuffer);

    lighting.BindSampledTexture("normalMap", m_pColorSampler, "offscreen_normal");

    lighting.BindSampledTexture("albedoMap", m_pColorSampler, "offscreen_albedo");

    lighting.BindSampledTexture("metallicRoughnessMap", m_pColorSampler, "offscreen_roughness");

    lighting.BindSampledTexture("emissiveOcclusionMap", m_pColorSampler, "offscreen_emissive_occlusion");

    lighting.BindSampledTexture("depthMap", m_pDepthSampler, "offscreen_depth");

    lighting.BindSampledTexture("envIrradianceMap", env.pIrradianceSampler, env.pIrradiance->GetDefaultView());

    lighting.BindSampledTexture("envPrefilteredMap", env.pPrefilteredSampler, env.pPrefiltered->GetDefaultView());

    lighting.BindSampledTexture("lutBRDFMap", env.pLutBRDFSampler, env.pLutBRDF->GetDefaultView());

    lighting.BindValue("uSceneData", m_pScene->GetSceneUniformData(), sizeof(SceneUniformData));

    if (capture)
    {
        lighting.BindStorageBuffer("LightingCapture", m_captureOutput);
    }

    if (voxelGI != nullptr)
    {
        voxelGI->BindLightingInputs(lighting);

        voxelGI->BindShadowInputs(lighting, shadows);
    }
    else if (mapShadowedPBR)
    {
        shadows->BindLightingInputs(lighting);
    }

    graph->AddGraphicsPass(std::move(lighting))
        .RecordPassCommands([capture, captureValid, captureExtent](RDGPassCmdEncoder& encoder) {
            if (!captureValid)
            {
                encoder.Fail(RDGErrorCode::eRange, "Lighting capture extent, buffer size or device limits changed");
            }

            if (capture)
            {
                encoder.SetPushConstants(captureExtent);
            }

            encoder.Draw(3, 1);
        });
}

bool DeferredLightingRenderer::ShadesOpaqueDeferred() const
{
    return SupportsDeferredOpaque(m_pScene->GetMaterialsData());
}

bool DeferredLightingRenderer::UsesForwardMaterials() const
{
    bool result = false;

    for (const sg::MaterialData& material : m_pScene->GetMaterialsData())
    {
        result = result || material.materialProperties.w > 0.5f || IsForwardTranslucent(material);
    }

    for (const SceneMeshDraw& draw : SnapshotSceneDraws(*m_pScene, GI_ALL, false))
    {
        result = result || draw.topology != sg::MeshTopology::Triangles;
    }

    return result;
}

void DeferredLightingRenderer::BuildForwardGraph(const RenderView& view, VoxelGIRenderer* voxelGI, SceneShadowRenderer* shadows)
{
    RenderGraph* graph            = m_pRenderDevice->GetCurrentFrameRDG();

    RDGResourceManager* resources = graph->GetResourceManager();

    const uint32_t width          = view.GetWidth();

    const uint32_t height         = view.GetHeight();

    const EnvTexture& env         = m_pScene->GetEnvTexture();
    const bool        capture =
        m_captureOutput != nullptr && m_hybrid != nullptr && m_hybrid->IsActive() && BuildLightingCaptureClear(view);
    const glm::uvec2 captureExtent = capture ? glm::uvec2(width, height) : glm::uvec2(0);


    bool displayTransparency       = false;

    for (const sg::MaterialData& material : m_pScene->GetMaterialsData())
    {
        displayTransparency |=
            material.materialProperties.y > 0.5f && material.surfaceProperties.y == static_cast<float>(sg::AlphaMode::Blend);
    }

    RDGTextureDesc colorDesc;

    colorDesc.name             = "forward_hdr";

    colorDesc.texFormat.format = DataFormat::eR16G16B16A16SFloat;

    colorDesc.texFormat.width  = width;

    colorDesc.texFormat.height = height;

    colorDesc.texFormat.depth  = 1;

    colorDesc.usageFlags.SetFlags(RHITextureUsageFlagBits::eColorAttachment, RHITextureUsageFlagBits::eSampled,
                                  RHITextureUsageFlagBits::eTransferSrc);

    const RDGTexture color  = resources->CreateTexture(colorDesc);

    RDGTexture displayColor = color;

    if (displayTransparency)
    {
        RDGTextureDesc displayDesc = colorDesc;

        displayDesc.name           = "forward_display_linear";

        displayColor               = resources->CreateTexture(displayDesc);
    }

    RDGTextureDesc scatterDesc   = colorDesc;

    scatterDesc.name             = "forward_scatter_lighting";

    scatterDesc.texFormat.format = DataFormat::eR32G32B32A32SFloat;

    scatterDesc.usageFlags.SetFlag(RHITextureUsageFlagBits::eTransferDst);

    const RDGTexture scatterLighting = resources->CreateTexture(scatterDesc);

    scatterDesc.name                 = "forward_scatter_position";

    const RDGTexture scatterPosition = resources->CreateTexture(scatterDesc);

    graph->AddTransferPass("InitializeScattering")
        .ClearTexture(scatterLighting, Color(0.0f))
        .ClearTexture(scatterPosition, Color(0.0f));

    RDGTextureDesc opaqueDesc    = colorDesc;

    opaqueDesc.name              = "forward_opaque";

    opaqueDesc.texFormat.mipmaps = 1;

    uint32_t mipSize             = std::max(width, height);

    while (mipSize > 1)
    {
        ++opaqueDesc.texFormat.mipmaps;

        mipSize /= 2;
    }

    opaqueDesc.usageFlags.SetFlag(RHITextureUsageFlagBits::eTransferDst);

    const RDGTexture opaque = resources->CreateTexture(opaqueDesc);

    graph->AddTransferPass("InitializeRefraction").ClearTexture(opaque, Color(0.0f));

    // Ray-shadowed and hybrid forward layers read the deferred receivers' exact shadow results.
    const bool rayShadows         = voxelGI != nullptr && voxelGI->UsesRayShadows();

    const bool hybrid             = m_hybrid != nullptr && m_hybrid->IsActive();

    const bool receiverVisibility = rayShadows || hybrid;

    RDGTexture lightVisibility;

    if (receiverVisibility)
    {
        RDGTextureDesc visibilityDesc;

        visibilityDesc.name             = "deferred_light_visibility";

        visibilityDesc.texFormat.format = DataFormat::eR32G32UInt;

        visibilityDesc.texFormat.width  = width;

        visibilityDesc.texFormat.height = height;

        visibilityDesc.texFormat.depth  = 1;

        visibilityDesc.usageFlags.SetFlags(RHITextureUsageFlagBits::eColorAttachment, RHITextureUsageFlagBits::eSampled,
                                           RHITextureUsageFlagBits::eTransferDst);

        lightVisibility = resources->CreateTexture(visibilityDesc);
    }

    RHIGfxPipelineStates fullscreenState{};

    fullscreenState.colorBlendState.AddAttachment();

    fullscreenState.dynamicStates.Enable(RHIDynamicState::eScissor, RHIDynamicState::eViewPort);

    fullscreenState.depthStencilState = RHIGfxPipelineDepthStencilState::Create(false, false, RHIDepthCompareOperator::eAlways);

    const Mat4             cameraView = reinterpret_cast<const sg::CameraUniformData*>(m_pScene->GetCameraUniformData())->view;
    const ForwardGPUResult gpuForward =
        m_gpuCullingEnabled ? m_forwardGPUDraws.Build(*m_pRenderDevice, *graph, *m_pScene, m_forwardDraws, cameraView)
                            : ForwardGPUResult{};
    if (!gpuForward)
    {
        m_forwardDraws.Update(*m_pScene, cameraView);
    }
    const HeapVector<SceneMeshDraw>      noDraws;
    const HeapVector<SceneMeshDraw>*     opaqueDraws[3]    = {&m_forwardDraws.opaque[0], &m_forwardDraws.opaque[1],
                                                              &m_forwardDraws.opaque[2]};
    const HeapVector<ForwardSortedDraw>& translucentDepths = m_forwardDraws.translucent;
    const HeapVector<uint32_t>&          sortedIndices     = m_forwardDraws.indices;
    const uint32_t                       sortedBytes       = static_cast<uint32_t>(sortedIndices.size() * sizeof(uint32_t));
    if (!gpuForward && m_forwardDraws.indicesChanged)
    {
        m_sortedBlendIndices.MarkDirty(0, sortedBytes);
    }
    // Retry a declined upload on a cache hit; its ring retains dirty ranges until it succeeds.
    const bool sortedReady =
        !gpuForward && !sortedIndices.empty()
        && m_sortedBlendIndices.Update(*m_pRenderDevice, sortedIndices.data(), sortedBytes, "forward_sorted_blend_indices");
    const bool cachedRuns = sortedReady || !m_forwardDraws.hasBlend;

    // Hybrid receivers already rasterized every opaque triangle. Prefilling their depth lets
    // the forward passes reject hidden fragments early instead of shading each layer, unless a
    // layer the receivers exclude (diffuse transmission, volume back faces) needs its own depth.
    const uint32_t triangles = static_cast<uint32_t>(sg::MeshTopology::Triangles);

    const bool gbuffer       = m_gbufferExtent == glm::uvec2(width, height);

    // When the G-buffer shades every opaque material, deferred lighting replaces the forward
    // opaque triangles and only translucent layers and non-triangle draws are shaded forward.
    const bool deferredOpaque = gbuffer && ShadesOpaqueDeferred();

    bool prefilled            = deferredOpaque || (m_hybrid != nullptr && gbuffer);

    for (const SceneMeshDraw& draw : *opaqueDraws[triangles])
    {
        const sg::MaterialData& material = m_pScene->GetMaterialsData()[draw.materialIndex];

        prefilled &=
            deferredOpaque || (material.diffuseTransmissionColorFactor.w == 0.0f && material.volumeIridescence.x == 0.0f);
    }

    if (deferredOpaque)
    {
        opaqueDraws[triangles] = &noDraws;
    }

    RHIGfxPipelineStates depthState = fullscreenState;

    depthState.depthStencilState    = RHIGfxPipelineDepthStencilState::Create(true, true, RHIDepthCompareOperator::eAlways);

    RDGGraphicsPassDesc background;

    background.SetShaderProgramName(prefilled ? "ForwardBackgroundDepthSP" : "ForwardBackgroundSP");

    background.SetPassTag("ForwardBackground");

    background.SetPipelineStates(prefilled ? depthState : fullscreenState);

    background.SetRenderArea(0, 0, width, height);

    background.AddColorOutput(color, RHIRenderTargetLoadOp::eClear, RHIRenderTargetStoreOp::eStore, true);

    background.AddDepthStencilOutput(view.GetDepthTarget(), RHIRenderTargetLoadOp::eClear, RHIRenderTargetStoreOp::eStore);

    background.BindValue("uCameraData", m_pScene->GetCameraUniformData(), sizeof(sg::CameraUniformData));

    background.BindValue("uSceneData", m_pScene->GetSceneUniformData(), sizeof(SceneUniformData));

    background.BindSampledTexture("envSourceMap", env.pPrefilteredSampler, env.pSkybox->GetDefaultView());

    if (prefilled)
    {
        background.BindSampledTexture("depthMap", m_pDepthSampler, "offscreen_depth");
    }

    graph->AddGraphicsPass(std::move(background)).RecordPassCommands([](RDGPassCmdEncoder& encoder) { encoder.Draw(3, 1); });

    // Only blend triangles can rest on a deferred receiver and take its shadow-ray results
    // (forward_material.glsl). Without one, deferred lighting does not export them.
    const bool restingLayers = m_forwardDraws.hasBlend;

    // Lighting captures keep evaluating every shadow-map light in the layer itself.
    const bool shareRays = (rayShadows || (hybrid && !capture)) && deferredOpaque && restingLayers;

    if (deferredOpaque)
    {
        AddLightingPass(view, voxelGI, shadows, capture, true, &color, shareRays ? &lightVisibility : nullptr);
    }

    if (receiverVisibility && !shareRays)
    {
        // Every forward layer evaluates its own shadows; the bound receiver results stay empty.
        graph->AddTransferPass("ClearLightVisibility").ClearTexture(lightVisibility, Color(0.0f));
    }

    bool scattering = false;

    for (const sg::MaterialData& material : m_pScene->GetMaterialsData())
    {
        scattering |= material.diffuseTransmissionColorFactor.w > 0.0f
                   && glm::any(glm::greaterThan(Vec3(material.volumeScatterColorRetroreflection), Vec3(0.0f)));
    }

    if (scattering)
    {
        for (uint32_t topology = 0; topology < 3; ++topology)
        {
            if (!opaqueDraws[topology]->empty())
            {
                RHIGfxPipelineStates state{};

                state.primitiveType               = ForwardTopology(static_cast<sg::MeshTopology>(topology));

                state.rasterizationState.cullMode = RHIPolygonCullMode::eDisabled;

                state.depthStencilState = RHIGfxPipelineDepthStencilState::Create(true, !prefilled || topology != triangles,
                                                                                  RHIDepthCompareOperator::eLessOrEqual);

                state.colorBlendState.AddAttachments(2);

                state.dynamicStates.Enable(RHIDynamicState::eScissor, RHIDynamicState::eViewPort);

                RDGGraphicsPassDesc source;

                source.SetShaderProgramName(voxelGI == nullptr ? "ForwardScatterSP" : "ForwardScatterVoxelGISP");
                if (voxelGI != nullptr && voxelGI->UsesRayShadows())
                {
                    const bool counts = voxelGI->CountsRays();
                    source.SetShaderProgramName(counts ? "ForwardScatterHardwareShadowsCountsSP"
                                                       : "ForwardScatterHardwareShadowsSP");
                    if (counts)
                    {
                        voxelGI->BindRayTotals(source);
                    }
                }

                source.SetPassTag("ForwardScatterSource");

                source.SetPipelineStates(state);

                source.SetRenderArea(0, 0, width, height);

                source.AddColorOutput(scatterLighting, RHIRenderTargetLoadOp::eLoad);

                source.AddColorOutput(scatterPosition, RHIRenderTargetLoadOp::eLoad);

                source.AddDepthStencilOutput(view.GetDepthTarget(), RHIRenderTargetLoadOp::eLoad,
                                             RHIRenderTargetStoreOp::eStore);

                source.BindStorageBuffer("NodeBuffer", m_pScene->GetNodesDataSSBO());

                source.BindStorageBuffer("MaterialBuffer", m_pScene->GetMaterialsDataSSBO());

                source.BindStorageBuffer("UVBuffer", m_pScene->GetUVBuffer());

                source.BindValue("uCameraData", m_pScene->GetCameraUniformData(), sizeof(sg::CameraUniformData));

                source.BindValue("uSceneData", m_pScene->GetSceneUniformData(), sizeof(SceneUniformData));

                source.BindSampledTexture("envIrradianceMap", env.pIrradianceSampler, env.pIrradiance->GetDefaultView());

                BindSceneTextureArray(source, m_pColorSampler, m_pScene->GetSceneTextureViews(), m_pScene->GetSceneSamplers());

                source.BindVertexBuffer(m_pScene->GetVertexBuffer());

                source.BindIndexBuffer(m_pScene->GetIndexBuffer());

                if (voxelGI != nullptr)
                {
                    voxelGI->BindLightingInputs(source);

                    voxelGI->BindShadowInputs(source, shadows, true);
                }

                graph->AddGraphicsPass(std::move(source))
                    .RecordPassCommands(
                        [draws = *opaqueDraws[topology]](RDGPassCmdEncoder& encoder) { RecordForwardDraws(encoder, draws); });
            }
        }
    }

    for (uint32_t stage = 0; stage < 4; ++stage)
    {
        if (stage == 3)
        {
            RHITextureCopyRegion region{};

            region.size                       = Vec3i(width, height, 1);

            region.srcSubresources.layerCount = 1;

            region.dstSubresources.layerCount = 1;

            region.srcSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);

            region.dstSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);

            graph->AddTransferPass("ResolveRefraction").CopyTexture(color, opaque, MakeVecView(&region, 1));

            graph->AddTransferPass("FilterRefraction").GenerateMipmaps(opaque);

            if (displayTransparency)
            {
                // Display-referred unlit colors must blend after exposure/tone mapping.
                // Keep the opaque HDR snapshot for transmission and refraction.
                RDGGraphicsPassDesc displayOpaque;

                displayOpaque.SetShaderProgramName("ForwardToneMapSP");

                displayOpaque.SetPassTag("ForwardOpaqueDisplay");

                displayOpaque.SetPipelineStates(fullscreenState);

                displayOpaque.SetRenderArea(0, 0, width, height);

                displayOpaque.AddColorOutput(displayColor, RHIRenderTargetLoadOp::eClear);

                displayOpaque.BindSampledTexture("sceneColorMap", m_pTransmissionSampler, color);

                graph->AddGraphicsPass(std::move(displayOpaque)).RecordPassCommands([](RDGPassCmdEncoder& encoder) {
                    encoder.SetPushConstants(1u);

                    encoder.Draw(3, 1);
                });
            }
        }

        uint32_t   first  = 0;
        const bool gpuRun = stage == 3 && bool(gpuForward);
        if (gpuRun)
        {
            const sg::CameraUniformData& camera =
                *reinterpret_cast<const sg::CameraUniformData*>(m_pScene->GetCameraUniformData());
            m_forwardGPUDraws.Emit(*m_pRenderDevice, *graph, gpuForward, view.GetDepthTarget(), width, height,
                                   camera.projViewMatrix);
        }
        bool pending = stage < 3 ? !opaqueDraws[stage]->empty() : gpuRun || !translucentDepths.empty();
        while (pending)
        {
            HeapVector<SceneMeshDraw> draws;
            const ForwardDrawBatch*   batch    = stage == 3 && !gpuRun && cachedRuns ? &m_forwardDraws.batches[first] : nullptr;
            const sg::MeshTopology    topology = stage < 3        ? static_cast<sg::MeshTopology>(stage)
                                               : gpuRun           ? sg::MeshTopology::Triangles
                                               : batch != nullptr ? batch->topology
                                                                  : translucentDepths[first].draw.topology;
            const bool                writeDepth =
                stage < 3
                || (!gpuRun
                    && (batch != nullptr
                            ? batch->writeDepth
                            : m_pScene->GetMaterialsData()[translucentDepths[first].draw.materialIndex].surfaceProperties.y
                                  != static_cast<float>(sg::AlphaMode::Blend)));
            const bool sortedRun = batch != nullptr && batch->sorted;
            const bool splitCore =
                stage == 3 && topology == sg::MeshTopology::Triangles
                && (gpuRun ? gpuForward.splitCore
                    : batch != nullptr
                        ? batch->splitCore
                        : m_pScene->GetMaterialsData()[translucentDepths[first].draw.materialIndex].textureSampling.z > 0.0f);
            const bool basicPBR =
                stage == 3
                && (gpuRun
                    || (batch != nullptr ? batch->basicPBR
                                         : IsBasicForwardMaterial(
                                               m_pScene->GetMaterialsData()[translucentDepths[first].draw.materialIndex])));
            if (gpuRun)
            {
                pending = false;
            }
            else if (stage < 3)
            {
                draws   = *opaqueDraws[stage];
                pending = false;
            }
            else if (batch != nullptr)
            {
                draws = HeapVector<SceneMeshDraw>(m_forwardDraws.draws.data() + batch->firstDraw, batch->drawCount);
                ++first;
                pending = first < m_forwardDraws.batches.size();
            }
            else
            {
                // An unavailable sorted index buffer keeps the original per-triangle draw order.
                while (
                    first < translucentDepths.size() && translucentDepths[first].draw.topology == topology
                    && (m_pScene->GetMaterialsData()[translucentDepths[first].draw.materialIndex].surfaceProperties.y
                        != static_cast<float>(sg::AlphaMode::Blend))
                           == writeDepth
                    && IsBasicForwardMaterial(m_pScene->GetMaterialsData()[translucentDepths[first].draw.materialIndex])
                           == basicPBR
                    && (topology == sg::MeshTopology::Triangles
                        && m_pScene->GetMaterialsData()[translucentDepths[first].draw.materialIndex].textureSampling.z > 0.0f)
                           == splitCore)
                {
                    draws.push_back(translucentDepths[first].draw);
                    ++first;
                }
                pending = first < translucentDepths.size();
            }

            RHIGfxPipelineStates state{};

            state.primitiveType               = ForwardTopology(topology);

            state.rasterizationState.cullMode = RHIPolygonCullMode::eDisabled;

            state.depthStencilState           = RHIGfxPipelineDepthStencilState::Create(
                true, writeDepth && (!prefilled || stage != triangles),
                splitCore ? RHIDepthCompareOperator::eLess : RHIDepthCompareOperator::eLessOrEqual);

            state.colorBlendState.AddAttachment();

            state.dynamicStates.Enable(RHIDynamicState::eScissor, RHIDynamicState::eViewPort);

            if (stage == 3)
            {
                RHIGfxPipelineColorBlendState::Attachment& blend = state.colorBlendState.attachments[0];

                blend.enableBlend                                = true;

                blend.srcColorBlendFactor                        = RHIBlendFactor::eSrcAlpha;

                blend.dstColorBlendFactor                        = RHIBlendFactor::eOneMinusSrcAlpha;

                blend.srcAlphaBlendFactor                        = RHIBlendFactor::eOne;

                blend.dstAlphaBlendFactor                        = RHIBlendFactor::eOneMinusSrcAlpha;
            }

            RDGGraphicsPassDesc forward;

            forward.SetShaderProgramName(voxelGI == nullptr
                                             ? (basicPBR ? "ForwardMaterialBasicSP" : "ForwardMaterialSP")
                                             : (basicPBR ? "ForwardMaterialVoxelGIBasicSP" : "ForwardMaterialVoxelGISP"));

            if (hybrid)
            {
                forward.SetShaderProgramName(capture ? (basicPBR ? "ForwardHybridGICaptureBasicSP" : "ForwardHybridGICaptureSP")
                                                     : (basicPBR ? "ForwardHybridGIBasicSP" : "ForwardHybridGISP"));
                if (capture)
                {
                    forward.BindStorageBuffer("LightingCapture", m_captureOutput);
                }
                m_hybrid->BindLightingInputs(forward);
            }
            if (receiverVisibility)
            {
                forward.BindSampledTexture("receiverLightVisibility", m_pDepthSampler, lightVisibility);
            }
            if (rayShadows)
            {
                const bool counts = !capture && voxelGI->CountsRays();
                forward.SetShaderProgramName(
                    capture  ? (basicPBR ? "ForwardHardwareShadowsCaptureBasicSP" : "ForwardHardwareShadowsCaptureSP")
                    : counts ? (basicPBR ? "ForwardHardwareShadowsCountsBasicSP" : "ForwardHardwareShadowsCountsSP")
                             : (basicPBR ? "ForwardHardwareShadowsBasicSP" : "ForwardHardwareShadowsSP"));
                if (counts)
                {
                    voxelGI->BindRayTotals(forward);
                }
            }

            if (gpuRun)
            {
                std::string name(forward.shaderProgramName.CStr());
                name.insert(name.size() - 2u, "GPU");
                forward.SetShaderProgramName(NameID(name));
            }
            forward.SetShaderProgramName(MaterialUVShaderName(forward.shaderProgramName, m_pScene->GetUVSetCount()));

            forward.SetPassTag(stage < 3 ? "ForwardOpaque" : "ForwardTranslucent");

            forward.SetPipelineStates(state);

            forward.SetRenderArea(0, 0, width, height);

            forward.AddColorOutput(stage == 3 ? displayColor : color, RHIRenderTargetLoadOp::eLoad);

            forward.AddDepthStencilOutput(view.GetDepthTarget(), RHIRenderTargetLoadOp::eLoad, RHIRenderTargetStoreOp::eStore);

            forward.BindStorageBuffer("NodeBuffer", m_pScene->GetNodesDataSSBO());

            forward.BindStorageBuffer("MaterialBuffer", m_pScene->GetMaterialsDataSSBO());

            forward.BindStorageBuffer("UVBuffer", m_pScene->GetUVBuffer());

            forward.BindValue("uCameraData", m_pScene->GetCameraUniformData(), sizeof(sg::CameraUniformData));

            forward.BindValue("uSceneData", m_pScene->GetSceneUniformData(), sizeof(SceneUniformData));

            forward.BindSampledTexture("envIrradianceMap", env.pIrradianceSampler, env.pIrradiance->GetDefaultView());

            forward.BindSampledTexture("envPrefilteredMap", env.pPrefilteredSampler, env.pPrefiltered->GetDefaultView());

            forward.BindSampledTexture("lutBRDFMap", env.pLutBRDFSampler, env.pLutBRDF->GetDefaultView());

            forward.BindSampledTexture("envSourceMap", env.pPrefilteredSampler, env.pSkybox->GetDefaultView());

            forward.BindSampledTexture("opaqueColorMap", m_pTransmissionSampler, opaque);

            forward.BindSampledTexture("scatterLightingMap", m_pDepthSampler, scatterLighting);

            forward.BindSampledTexture("scatterPositionMap", m_pDepthSampler, scatterPosition);

            BindSceneTextureArray(forward, m_pColorSampler, m_pScene->GetSceneTextureViews(), m_pScene->GetSceneSamplers());

            if (gpuRun)
            {
                forward.BindStorageBuffer("ForwardTriangles", m_forwardGPUDraws.GetTriangles());
                forward.BindStorageBuffer("ForwardVertices", m_pScene->GetVertexBuffer());
                forward.BindStorageBuffer("ForwardSourceIndices", m_pScene->GetIndexBuffer());
                forward.BindIndexBuffer(gpuForward.indices);
                forward.UseIndirectBuffer(gpuForward.arguments);
            }
            else
            {
                forward.BindVertexBuffer(m_pScene->GetVertexBuffer());
                forward.BindIndexBuffer(sortedRun ? m_sortedBlendIndices.GetBuffer() : m_pScene->GetIndexBuffer());
            }

            if (voxelGI != nullptr)
            {
                voxelGI->BindLightingInputs(forward);

                voxelGI->BindShadowInputs(forward, shadows, true);
            }

            if (gpuRun)
            {
                graph->AddGraphicsPass(std::move(forward))
                    .RecordPassCommands(
                        [arguments = gpuForward.arguments, captureExtent, displayTransparency](RDGPassCmdEncoder& encoder) {
                            const ForwardPushConstants constants{0, 0, 2u | (displayTransparency ? 4u : 0u)};
                            SetForwardConstants(encoder, constants, captureExtent);
                            encoder.DrawIndexedIndirect(arguments, 0, 1, 20);
                        });
            }
            else
            {
                graph->AddGraphicsPass(std::move(forward))
                    .RecordPassCommands(
                        [draws, captureExtent, displayLinear = stage == 3 && displayTransparency](RDGPassCmdEncoder& encoder) {
                            RecordForwardDraws(encoder, draws, displayLinear, captureExtent);
                        });
            }
        }
    }

    RDGGraphicsPassDesc toneMap;

    toneMap.SetShaderProgramName("ForwardToneMapSP");

    toneMap.SetPassTag("ForwardToneMap");

    toneMap.SetPipelineStates(fullscreenState);

    toneMap.SetRenderArea(0, 0, width, height);

    toneMap.AddColorOutput(view.GetColorTarget(), RHIRenderTargetLoadOp::eLoad);

    toneMap.BindSampledTexture("sceneColorMap", m_pTransmissionSampler, displayColor);

    graph->AddGraphicsPass(std::move(toneMap)).RecordPassCommands([displayTransparency](RDGPassCmdEncoder& encoder) {
        encoder.SetPushConstants(displayTransparency ? 2u : 0u);

        encoder.Draw(3, 1);
    });

    if (capture)
    {
        graph->AddTransferPass("ReadForwardLightingCapture")
            .CopyBuffer(m_captureOutput, m_captureReadback, {0, 0, m_captureOutput->GetRequiredSize()})
            .NeverCull();
    }
    m_captureRecorded = capture;

    BuildLightMarkers(view);
}

void DeferredLightingRenderer::BuildLightMarkers(const RenderView& view)
{
    const SceneUniformData& sceneData = *reinterpret_cast<const SceneUniformData*>(m_pScene->GetSceneUniformData());

    const uint32_t lightCount         = static_cast<uint32_t>(sceneData.lightInfo.x);

    if (sceneData.cameraLight.colorIntensity.w > 0.0f)
    {
        RHIGfxPipelineStates pso{};

        pso.rasterizationState.cullMode = RHIPolygonCullMode::eDisabled;

        pso.depthStencilState           = RHIGfxPipelineDepthStencilState::Create(true, true, RHIDepthCompareOperator::eLess);

        pso.colorBlendState.AddAttachment();

        pso.dynamicStates.Enable(RHIDynamicState::eScissor, RHIDynamicState::eViewPort);

        RDGGraphicsPassDesc ball;

        ball.SetShaderProgramName("LightBallSP");

        ball.SetPassTag("CameraLightBall");

        ball.SetPipelineStates(pso);

        ball.SetRenderArea(0, 0, view.GetWidth(), view.GetHeight());

        ball.AddColorOutput(view.GetColorTarget(), RHIRenderTargetLoadOp::eLoad);

        ball.AddDepthStencilOutput(view.GetDepthTarget(), RHIRenderTargetLoadOp::eLoad, RHIRenderTargetStoreOp::eStore);

        ball.BindValue("uCameraData", m_pScene->GetCameraUniformData(), sizeof(sg::CameraUniformData));

        ball.BindValue("uSceneData", sceneData);

        m_pRenderDevice->GetCurrentFrameRDG()
            ->AddGraphicsPass(std::move(ball))
            .RecordPassCommands([](RDGPassCmdEncoder& encoder) { encoder.Draw(24 * 12 * 6, 1); });
    }

    if (m_lightMarkersEnabled && lightCount > 0)
    {
        RHIGfxPipelineStates pso{};

        pso.rasterizationState.cullMode = RHIPolygonCullMode::eDisabled;

        pso.depthStencilState           = RHIGfxPipelineDepthStencilState::Create(true, true, RHIDepthCompareOperator::eLess);

        pso.colorBlendState.AddAttachment();

        pso.dynamicStates.Enable(RHIDynamicState::eScissor, RHIDynamicState::eViewPort);

        RDGGraphicsPassDesc markers;

        markers.SetShaderProgramName("LightMarkerSP");

        markers.SetPassTag("LightMarkers");

        markers.SetPipelineStates(pso);

        markers.SetRenderArea(0, 0, view.GetWidth(), view.GetHeight());

        markers.AddColorOutput(view.GetColorTarget(), RHIRenderTargetLoadOp::eLoad);

        markers.AddDepthStencilOutput(view.GetDepthTarget(), RHIRenderTargetLoadOp::eLoad, RHIRenderTargetStoreOp::eStore);

        markers.BindValue("uCameraData", m_pScene->GetCameraUniformData(), sizeof(sg::CameraUniformData));

        markers.BindValue("uSceneData", sceneData);

        // Visual-only boxes share the lighting snapshot and never enter the voxel volume.
        const float markerSize = m_lightMarkerSize;

        m_pRenderDevice->GetCurrentFrameRDG()
            ->AddGraphicsPass(std::move(markers))
            .RecordPassCommands([markerSize, lightCount](RDGPassCmdEncoder& encoder) {
                encoder.SetPushConstants(markerSize);

                encoder.Draw(36, lightCount);
            });
    }
}

} // namespace zen::rc
