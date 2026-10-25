#include "Graphics/RenderCore/V2/SceneRayQuery.h"
#include "Graphics/RenderCore/V2/RenderScene.h"
#include "Graphics/RenderCore/V2/RenderDevice.h"
#include "Graphics/RenderCore/V2/Renderer/RendererUtils.h"
#include "Graphics/RenderCore/V2/VoxelResourcePlanning.h"
#include "Templates/SmallVector.h"
#include <bit>
#include <cstring>

namespace zen::rc
{
namespace
{
bool SameGeometry(VectorView<const RHIAccelerationStructureGeometry> first,
                  VectorView<const RHIAccelerationStructureGeometry> second)
{
    bool same = first.size() == second.size();
    for (size_t i = 0; same && i < first.size(); ++i)
    {
        same = first[i].vertexCount == second[i].vertexCount && first[i].vertexStride == second[i].vertexStride
            && first[i].indexCount == second[i].indexCount && first[i].indexOffset == second[i].indexOffset
            && first[i].opaque == second[i].opaque;
    }
    return same;
}

// Whether a primitive can ever block a ray (MaterialMayOcclude over its largest vertex alpha). New
// Sponza's dirt decals (alpha factor 0.35) cover the walls just above every shadow-ray origin.
bool PrimitiveMayOcclude(const RenderScene& scene, const sg::SubMesh& primitive, uint32_t material)
{
    bool occludes = material >= scene.GetMaterialsData().size();
    if (!occludes)
    {
        const HeapVector<uint32_t>&      indices  = scene.GetIndices();
        const HeapVector<asset::Vertex>& vertices = scene.GetVertices();
        // Out-of-range indices count as fully opaque; the caller rejects the primitive anyway.
        const uint64_t end   = uint64_t(primitive.GetFirstIndex()) + primitive.GetIndexCount();
        float          alpha = end > indices.size() ? 1.0f : 0.0f;
        for (uint64_t index = primitive.GetFirstIndex(); index < std::min<uint64_t>(end, indices.size()); ++index)
        {
            alpha = std::max(alpha, indices[index] < vertices.size() ? vertices[indices[index]].color.a : 1.0f);
        }
        occludes = MaterialMayOcclude(scene.GetMaterialsData()[material], alpha);
    }
    return occludes;
}

// FNV-1a over the position bits; only the BLAS inputs (positions) matter.
uint64_t HashPositions(const HeapVector<asset::Vertex>& vertices, uint32_t begin, uint32_t end)
{
    uint64_t hash = 0xcbf29ce484222325ull;
    for (uint32_t vertex = begin; vertex < end; ++vertex)
    {
        for (uint32_t axis = 0; axis < 3; ++axis)
        {
            hash = (hash ^ std::bit_cast<uint32_t>(vertices[vertex].pos[axis])) * 0x100000001b3ull;
        }
    }
    return hash;
}
} // namespace

bool MaterialMayOcclude(const sg::MaterialData& material, float vertexAlpha)
{
    bool occludes = material.surfaceProperties.y == static_cast<float>(sg::AlphaMode::Opaque);
    if (!occludes)
    {
        const float alpha =
            vertexAlpha * (material.surfaceProperties.w > 0.5f ? material.diffuseFactor.a : material.baseColorFactor.a);
        occludes =
            alpha
            >= (material.surfaceProperties.y == static_cast<float>(sg::AlphaMode::Mask) ? material.surfaceProperties.x : 0.5f);
    }
    return occludes;
}

const SceneRayQuery::Generation& SceneRayQuery::Current() const
{
    return m_recorded ? m_pending : m_active;
}

bool SceneRayQuery::IsReady() const
{
    return !m_device->AreSubmissionsBlocked() && Current().tlas != nullptr;
}

uint64_t SceneRayQuery::GetGeneration() const
{
    return IsReady() ? Current().generation : 0;
}

uint64_t SceneRayQuery::GetMemoryBytes() const
{
    // Cached TLASes retain their immutable BLAS dependencies even when the current
    // scene no longer contains those meshes. Count those allocations too, once.
    SmallVector<RHIAccelerationStructure*, 64> structures;
    uint64_t                                   bytes = m_scratchBytes;
    for (uint32_t i = 0; i < RenderFrameState::kMaxFramesInFlight + 2; ++i)
    {
        const Generation& generation = i == 0 ? m_active : i == 1 ? m_pending : m_cached[i - 2];
        for (RHIBuffer* buffer : {generation.instances, generation.geometryMetadata, generation.instanceMetadata})
        {
            bytes += buffer != nullptr ? buffer->GetRequiredSize() : 0;
        }
        if (generation.tlas != nullptr)
        {
            structures.push_back(generation.tlas);
            for (RHIAccelerationStructure* dependency : generation.tlas->GetReferencedStructures())
            {
                structures.push_back(dependency);
            }
        }
        for (const MeshBuild& mesh : generation.meshes)
        {
            if (mesh.structure != nullptr)
            {
                structures.push_back(mesh.structure);
            }
        }
    }
    std::sort(structures.begin(), structures.end(), std::less<RHIAccelerationStructure*>());
    for (size_t i = 0; i < structures.size(); ++i)
    {
        if (i == 0 || structures[i] != structures[i - 1])
        {
            bytes += structures[i]->GetStorageBuffer()->GetRequiredSize();
        }
    }
    for (const RetiredMemory& retired : m_retired)
    {
        bytes += retired.bytes;
    }
    return bytes;
}

void SceneRayQuery::RetireReleasedMemory(uint64_t previousBytes)
{
    const uint64_t currentBytes = GetMemoryBytes();
    VERIFY_EXPR(currentBytes <= previousBytes);
    if (currentBytes < previousBytes)
    {
        m_retired.push_back({previousBytes - currentBytes, m_device->CaptureResourceRetirement()});
    }
}

void SceneRayQuery::Release(Generation& generation)
{
    m_device->DeferReleaseResource(generation.tlas);
    for (MeshBuild& mesh : generation.meshes)
    {
        m_device->DeferReleaseResource(mesh.structure);
    }
    m_device->DestroyBuffer(generation.instances);
    m_device->DestroyBuffer(generation.geometryMetadata);
    m_device->DestroyBuffer(generation.instanceMetadata);
    generation = Generation{};
}

void SceneRayQuery::ReleaseGenerations()
{
    // Generations share BLASes, so account for the whole release at once.
    const uint64_t previousBytes = GetMemoryBytes();
    Release(m_pending);
    Release(m_active);
    for (Generation& generation : m_cached)
    {
        Release(generation);
    }
    RetireReleasedMemory(previousBytes);
    m_recorded = false;
}

void SceneRayQuery::Cache(Generation& generation)
{
    if (generation.tlas != nullptr)
    {
        VERIFY_EXPR(m_cached[generation.slot].tlas == nullptr);
        m_cached[generation.slot] = std::move(generation);
        generation                = Generation{};
    }
}

void SceneRayQuery::Destroy()
{
    ReleaseGenerations();
    if (m_scratchBytes != 0)
    {
        m_retired.push_back({m_scratchBytes, m_device->CaptureResourceRetirement()});
    }
    m_device->DestroyBuffer(m_scratch);
    m_scratch      = nullptr;
    m_scratchBytes = 0;
    m_recorded     = false;
    m_scene        = nullptr;
    m_reason       = "scene_not_ready";
}

RHIBuffer* SceneRayQuery::CreateInputBuffer(NameID name, uint64_t size, const void* data)
{
    RHIBufferCreateInfo info;
    info.size         = size;
    info.tag          = name;
    info.allocateType = RHIBufferAllocateType::eGPU;
    info.usageFlags.SetFlags(RHIBufferUsageFlagBits::eAccelerationStructureInput, RHIBufferUsageFlagBits::eDeviceAddress,
                             RHIBufferUsageFlagBits::eTransferDstBuffer);
    RHIBuffer* buffer = m_device->CreateBuffer(info);
    if (buffer != nullptr && !m_device->UpdateBuffer(buffer, static_cast<uint32_t>(size), static_cast<const uint8_t*>(data)))
    {
        m_device->DestroyBuffer(buffer);
        buffer = nullptr;
    }
    return buffer;
}

bool SceneRayQuery::UpdateInput(RHIBuffer*&          buffer,
                                HeapVector<uint8_t>& payload,
                                NameID               name,
                                uint32_t             size,
                                const void*          data,
                                bool                 instances)
{
    bool valid = true;
    if (buffer == nullptr || buffer->GetRequiredSize() < size)
    {
        RHIBuffer* replacement = instances ? CreateInputBuffer(name, size, data)
                                           : m_device->CreateStorageBuffer(size, static_cast<const uint8_t*>(data), name);
        valid                  = replacement != nullptr;
        if (valid)
        {
            if (buffer != nullptr)
            {
                m_retired.push_back({buffer->GetRequiredSize(), m_device->CaptureResourceRetirement()});
            }
            m_device->DestroyBuffer(buffer);
            buffer = replacement;
        }
    }
    else if (payload.size() != size || std::memcmp(payload.data(), data, size) != 0)
    {
        valid = m_device->UpdateBuffer(buffer, size, static_cast<const uint8_t*>(data));
    }
    if (valid)
    {
        payload.resize(size);
        std::memcpy(payload.data(), data, size);
    }
    return valid;
}

bool SceneRayQuery::PrepareMeshes(RenderScene& scene, HeapVector<glm::uvec4>& metadata)
{
    bool valid = true;
    for (const sg::Node* node : scene.GetRenderableNodes())
    {
        const sg::Mesh* mesh    = node->GetComponent<sg::Mesh>();
        bool            present = false;
        for (const MeshBuild& existing : m_pending.meshes)
        {
            present |= existing.mesh == mesh;
        }
        if (!present && scene.GetInstanceMask(node->GetRenderableIndex()) != 0)
        {
            MeshBuild build;
            build.mesh           = mesh;
            build.metadataOffset = static_cast<uint32_t>(metadata.size());
            bool primitivesValid = true;
            for (const sg::SubMesh* primitive : mesh->GetSubMeshes())
            {
                if (primitive->topology == sg::MeshTopology::Triangles && primitive->GetIndexCount() != 0
                    && PrimitiveMayOcclude(scene, *primitive, primitive->GetMaterial()->index))
                {
                    const uint32_t                   material = primitive->GetMaterial()->index;
                    RHIAccelerationStructureGeometry geometry;
                    geometry.pVertexBuffer = scene.GetVertexBuffer();
                    geometry.pIndexBuffer  = scene.GetIndexBuffer();
                    geometry.vertexStride  = sizeof(asset::Vertex);
                    geometry.vertexCount   = static_cast<uint32_t>(scene.GetVertices().size());
                    geometry.indexOffset   = uint64_t(primitive->GetFirstIndex()) * sizeof(uint32_t);
                    geometry.indexCount    = primitive->GetIndexCount();
                    // Only mask and blend candidates need the coverage test; every other surface,
                    // single-sided or not, blocks in both directions. Mode edits rebuild.
                    geometry.opaque =
                        material >= scene.GetMaterialsData().size()
                        || scene.GetMaterialsData()[material].surfaceProperties.y == static_cast<float>(sg::AlphaMode::Opaque);
                    build.geometries.push_back(geometry);
                    metadata.push_back({primitive->GetFirstIndex(), material, geometry.indexCount / 3, 0});
                    primitivesValid &= geometry.indexCount % 3 == 0 && primitive->GetFirstIndex() <= scene.GetIndices().size()
                                    && geometry.indexCount <= scene.GetIndices().size() - primitive->GetFirstIndex();
                }
            }
            valid &= primitivesValid;
            if (!build.geometries.empty() && primitivesValid)
            {
                RHIAccelerationStructureBuildDesc desc;
                desc.geometries            = MakeVecView(build.geometries);
                desc.allowUpdate           = true;
                build.sizes                = GDynamicRHI->GetAccelerationStructureBuildSizes(desc);
                valid                     &= build.sizes.IsValid();
                const MeshBuild* previous  = nullptr;
                for (const MeshBuild& candidate : m_active.meshes)
                {
                    if (candidate.mesh == mesh && m_active.indexIdentity == scene.GetIndexBuffer()->GetStableId()
                        && SameGeometry(MakeVecView(candidate.geometries), MakeVecView(build.geometries)))
                    {
                        previous = &candidate;
                        break;
                    }
                }
                const bool sameVertices = m_active.vertexIdentity == scene.GetVertexBuffer()->GetStableId()
                                       && m_active.vertexGeneration == scene.GetVertexGeneration();
                if (previous != nullptr && (sameVertices || !scene.HasUncommittedVertices()))
                {
                    // The BLAS holds its own copy of the positions, so an unchanged mesh
                    // keeps it even when another mesh's deformation replaced the buffer.
                    build.vertexBegin  = previous->vertexBegin;
                    build.vertexEnd    = previous->vertexEnd;
                    build.positionHash = sameVertices ? previous->positionHash
                                                      : HashPositions(scene.GetVertices(), build.vertexBegin, build.vertexEnd);
                    build.structure    = previous->structure;
                    build.structure->AddReference();
                    build.refits     = previous->refits;
                    build.needsBuild = build.positionHash != previous->positionHash;
                    // Refit in place; rebuild after kMaxRefits to bound BVH degradation.
                    build.update = build.needsBuild && previous->refits < kMaxRefits;
                    build.refits = build.needsBuild ? (build.update ? previous->refits + 1 : 0) : previous->refits;
                }
                else if (previous != nullptr)
                {
                    // CPU vertices are ahead of the committed buffer, so they cannot prove
                    // this mesh unchanged: refit unconditionally and record no hash.
                    build.vertexBegin = previous->vertexBegin;
                    build.vertexEnd   = previous->vertexEnd;
                    build.structure   = previous->structure;
                    build.structure->AddReference();
                    build.update = previous->refits < kMaxRefits;
                    build.refits = build.update ? previous->refits + 1 : 0;
                }
                else
                {
                    build.vertexBegin = UINT32_MAX;
                    for (const sg::SubMesh* primitive : mesh->GetSubMeshes())
                    {
                        if (primitive->topology == sg::MeshTopology::Triangles)
                        {
                            for (uint32_t i = 0; i < primitive->GetIndexCount(); ++i)
                            {
                                const uint32_t vertex = scene.GetIndices()[primitive->GetFirstIndex() + i];
                                build.vertexBegin     = std::min(build.vertexBegin, vertex);
                                build.vertexEnd       = std::max(build.vertexEnd, vertex + 1);
                            }
                        }
                    }
                    valid &= build.vertexEnd <= scene.GetVertices().size();
                    // A zero hash forces the next comparison to refit; it never proves equality.
                    build.positionHash = valid && !scene.HasUncommittedVertices()
                                           ? HashPositions(scene.GetVertices(), build.vertexBegin, build.vertexEnd)
                                           : 0;
                }
                m_pending.meshes.push_back(std::move(build));
            }
        }
    }
    return valid;
}

bool SceneRayQuery::Prepare(RenderScene& scene, uint64_t budgetBytes)
{
    uint32_t slot = ToIndex(GRenderFrameState.GetFrameSlot());
    if (m_active.tlas != nullptr && m_active.slot == slot)
    {
        slot = (slot + 1) % std::max(2u, GRenderFrameState.GetNumFramesInFlight());
    }
    m_pending                    = std::move(m_cached[slot]);
    m_cached[slot]               = Generation{};
    m_pending.slot               = slot;
    const uint64_t previousBytes = GetMemoryBytes();
    for (MeshBuild& mesh : m_pending.meshes)
    {
        m_device->DeferReleaseResource(mesh.structure);
    }
    m_pending.meshes.clear();
    RetireReleasedMemory(previousBytes);
    HeapVector<glm::uvec4>                       geometryMetadata;
    HeapVector<glm::uvec4>                       instanceMetadata;
    HeapVector<RHIAccelerationStructureInstance> instances;
    bool                                         valid = PrepareMeshes(scene, geometryMetadata);
    for (const sg::Node* node : scene.GetRenderableNodes())
    {
        if (scene.GetInstanceMask(node->GetRenderableIndex()) != 0)
        {
            for (size_t meshIndex = 0; meshIndex < m_pending.meshes.size(); ++meshIndex)
            {
                const MeshBuild& mesh = m_pending.meshes[meshIndex];
                if (mesh.mesh == node->GetComponent<sg::Mesh>())
                {
                    const Mat4&                      transform = scene.GetInstanceTransform(node->GetRenderableIndex());
                    RHIAccelerationStructureInstance instance;
                    for (uint32_t row = 0; row < 3; ++row)
                    {
                        for (uint32_t column = 0; column < 4; ++column)
                        {
                            instance.transform[row][column] = transform[column][row];
                        }
                    }
                    instance.customIndexAndMask = static_cast<uint32_t>(instances.size()) | (0xffu << 24);
                    // Culling is evaluated with the raster material in the candidate shader.
                    instance.offsetAndFlags = ToUnderlying(RHIAccelerationStructureInstanceFlagBits::eDisableTriangleCulling)
                                           << 24;
                    instance.accelerationStructureAddress = meshIndex; // Filled after the allocation preflight.
                    instances.push_back(instance);
                    instanceMetadata.push_back({node->GetRenderableIndex(), mesh.metadataOffset, 0, 0});
                    break;
                }
            }
        }
    }
    m_pending.instanceCount = static_cast<uint32_t>(instances.size());
    valid &= instances.size() <= 0xffffffu && instances.size() <= m_device->GetGPUInfo().rayQuery.maxInstances;
    // The TLAS is rebuilt for every generation: it is small, and refitting it would
    // let BVH quality degrade without bound while instances move.
    RHIAccelerationStructureBuildDesc tlasDesc;
    tlasDesc.type                                       = RHIAccelerationStructureType::eTopLevel;
    tlasDesc.instanceCount                              = m_pending.instanceCount;
    const RHIAccelerationStructureBuildSizes tlasSizes  = GDynamicRHI->GetAccelerationStructureBuildSizes(tlasDesc);
    valid                                              &= tlasSizes.IsValid();
    uint64_t scratchSize                                = tlasSizes.buildScratchSize;
    uint64_t newStructureBytes                          = 0;
    for (const MeshBuild& mesh : m_pending.meshes)
    {
        newStructureBytes += mesh.structure == nullptr ? mesh.sizes.storageSize : 0;
        if (mesh.needsBuild)
        {
            scratchSize = std::max(scratchSize, mesh.update ? mesh.sizes.updateScratchSize : mesh.sizes.buildScratchSize);
        }
    }
    const uint32_t alignment  = m_device->GetGPUInfo().rayQuery.scratchAlignment;
    scratchSize              += alignment;
    // Scratch only grows; a replacement retires the previous buffer.
    const uint64_t scratchGrowth = scratchSize > m_scratchBytes ? scratchSize : 0;
    // Dummy records permit zero-instance scenes with valid descriptors.
    if (instances.empty())
    {
        instances.push_back(RHIAccelerationStructureInstance{});
    }
    if (instanceMetadata.empty())
    {
        instanceMetadata.push_back(glm::uvec4(0));
    }
    if (geometryMetadata.empty())
    {
        geometryMetadata.push_back(glm::uvec4(0));
    }
    bool reuseTLAS =
        m_pending.tlas != nullptr && m_pending.tlas->GetStorageBuffer()->GetRequiredSize() >= tlasSizes.storageSize;
    if (reuseTLAS)
    {
        const HeapVector<RHIAccelerationStructure*>& references = m_pending.tlas->GetReferencedStructures();
        reuseTLAS                                               = references.size() == m_pending.meshes.size();
        for (size_t i = 0; reuseTLAS && i < references.size(); ++i)
        {
            reuseTLAS = references[i] == m_pending.meshes[i].structure;
        }
    }
    uint64_t       bytes        = reuseTLAS ? 0 : tlasSizes.storageSize;
    const uint64_t inputSizes[] = {instances.size() * sizeof(RHIAccelerationStructureInstance),
                                   geometryMetadata.size() * sizeof(glm::uvec4), instanceMetadata.size() * sizeof(glm::uvec4)};
    RHIBuffer*     inputs[]     = {m_pending.instances, m_pending.geometryMetadata, m_pending.instanceMetadata};
    for (uint32_t i = 0; i < 3; ++i)
    {
        bytes += inputs[i] == nullptr || inputs[i]->GetRequiredSize() < inputSizes[i] ? inputSizes[i] : 0;
    }
    uint64_t metadataBytes = 0;

    valid &= scratchSize <= UINT32_MAX && instances.size() * sizeof(RHIAccelerationStructureInstance) <= UINT32_MAX
          && ValidateGIStorageBuffer(geometryMetadata.size(), sizeof(glm::uvec4), m_device->GetGPUInfo(), metadataBytes)
                 == GIResourceStatus::eSuccess
          && ValidateGIStorageBuffer(instanceMetadata.size(), sizeof(glm::uvec4), m_device->GetGPUInfo(), metadataBytes)
                 == GIResourceStatus::eSuccess;
    const uint64_t added = bytes + newStructureBytes + scratchGrowth;
    if (valid && budgetBytes != 0 && (added > budgetBytes || GetMemoryBytes() > budgetBytes - added))
    {
        valid    = false;
        m_reason = "acceleration_structure_budget";
    }
    if (valid)
    {
        HeapVector<RHIAccelerationStructure*> references;
        for (MeshBuild& mesh : m_pending.meshes)
        {
            if (mesh.structure == nullptr)
            {
                RHIAccelerationStructureCreateInfo info;
                info.size      = mesh.sizes.storageSize;
                info.tag       = "scene_blas";
                mesh.structure = GDynamicRHI->CreateAccelerationStructure(info);
            }
            valid &= mesh.structure != nullptr;
            references.push_back(mesh.structure);
        }
        if (valid)
        {
            for (uint32_t i = 0; i < m_pending.instanceCount; ++i)
            {
                instances[i].accelerationStructureAddress =
                    references[instances[i].accelerationStructureAddress]->GetDeviceAddress();
            }
            if (!reuseTLAS)
            {
                RHIAccelerationStructureCreateInfo info;
                info.type                             = RHIAccelerationStructureType::eTopLevel;
                info.size                             = tlasSizes.storageSize;
                info.tag                              = "scene_tlas";
                info.referencedStructures             = MakeVecView(references);
                RHIAccelerationStructure* replacement = GDynamicRHI->CreateAccelerationStructure(info);
                valid                                 = replacement != nullptr;
                if (valid)
                {
                    const uint64_t beforeRelease = GetMemoryBytes();
                    m_device->DeferReleaseResource(m_pending.tlas);
                    m_pending.tlas = nullptr;
                    RetireReleasedMemory(beforeRelease);
                    m_pending.tlas = replacement;
                }
            }
            valid = valid
                 && UpdateInput(m_pending.instances, m_pending.instancePayload, "ray_instances",
                                static_cast<uint32_t>(inputSizes[0]), instances.data(), true)
                 && UpdateInput(m_pending.geometryMetadata, m_pending.geometryMetadataPayload, "ray_geometry_metadata",
                                static_cast<uint32_t>(inputSizes[1]), geometryMetadata.data())
                 && UpdateInput(m_pending.instanceMetadata, m_pending.instanceMetadataPayload, "ray_instance_metadata",
                                static_cast<uint32_t>(inputSizes[2]), instanceMetadata.data());
            if (valid && scratchGrowth != 0)
            {
                if (m_scratchBytes != 0)
                {
                    m_retired.push_back({m_scratchBytes, m_device->CaptureResourceRetirement()});
                }
                m_device->DestroyBuffer(m_scratch);
                RHIBufferCreateInfo scratch;
                scratch.size         = scratchSize;
                scratch.allocateType = RHIBufferAllocateType::eGPU;
                scratch.usageFlags.SetFlags(RHIBufferUsageFlagBits::eDeviceAddress, RHIBufferUsageFlagBits::eStorageBuffer);
                scratch.tag    = "ray_build_scratch";
                m_scratch      = m_device->CreateBuffer(scratch);
                m_scratchBytes = m_scratch != nullptr ? scratchSize : 0;
            }
            valid = valid && m_pending.tlas != nullptr && m_pending.instances != nullptr
                 && m_pending.geometryMetadata != nullptr && m_pending.instanceMetadata != nullptr && m_scratch != nullptr;
        }
    }
    return valid;
}

void SceneRayQuery::RecordBuilds(RenderScene& scene)
{
    RenderGraph*                      graph = m_device->GetCurrentFrameRDG();
    RHIAccelerationStructureBuildInfo info;
    info.pScratchBuffer          = m_scratch;
    const uint64_t alignment     = m_device->GetGPUInfo().rayQuery.scratchAlignment;
    info.scratchOffset           = (alignment - m_scratch->GetDeviceAddress() % alignment) % alignment;
    info.description.allowUpdate = true;
    for (const MeshBuild& mesh : m_pending.meshes)
    {
        m_statistics.built   += mesh.needsBuild && !mesh.update ? 1 : 0;
        m_statistics.updated += mesh.update ? 1 : 0;
        m_statistics.reused  += mesh.needsBuild ? 0 : 1;
        if (mesh.needsBuild)
        {
            // Updates and periodic rebuilds write the existing BLAS in place; the graph
            // orders them after the previous generation's readers.
            info.pDestination           = mesh.structure;
            info.pSource                = mesh.update ? mesh.structure : nullptr;
            info.description.geometries = MakeVecView(mesh.geometries);
            graph->AddTransferPass(mesh.update ? "UpdateSceneBLAS" : "BuildSceneBLAS").BuildAccelerationStructure(info);
        }
    }
    info.description.geometries      = {};
    info.description.type            = RHIAccelerationStructureType::eTopLevel;
    info.description.pInstanceBuffer = m_pending.instances;
    info.description.instanceCount   = m_pending.instanceCount;
    info.description.allowUpdate     = false;
    info.pDestination                = m_pending.tlas;
    info.pSource                     = nullptr;
    graph->AddTransferPass("BuildSceneTLAS").BuildAccelerationStructure(info);
    m_pending.geometryRevision = scene.GetGeometryRevision();
    m_pending.surfaceRevision  = scene.GetSurfaceRevision();
    m_pending.vertexIdentity   = scene.GetVertexBuffer()->GetStableId();
    m_pending.vertexGeneration = scene.GetVertexGeneration();
    m_pending.indexIdentity    = scene.GetIndexBuffer()->GetStableId();
    m_pending.generation       = m_active.generation + 1;
    m_recorded                 = true;
}

bool SceneRayQuery::BuildRenderGraph(RenderScene& scene, uint64_t budgetBytes)
{
    if (m_scene != &scene)
    {
        Destroy();
        m_scene = &scene;
    }
    m_statistics = {};
    for (size_t i = m_retired.size(); i > 0; --i)
    {
        if (m_device->IsResourceRetired(m_retired[i - 1].retirement))
        {
            m_retired.erase(m_retired.begin() + i - 1);
        }
    }
    bool ready = m_device->GetGPUInfo().rayQuery.IsUsable();
    m_reason   = ready ? "hardware_ready" : "ray_query_features_disabled_or_unavailable";
    if (ready && budgetBytes != 0 && GetMemoryBytes() > budgetBytes)
    {
        ReleaseGenerations();
        ready    = false;
        m_reason = "acceleration_structure_budget";
    }
    if (ready
        && (m_active.tlas == nullptr || m_active.geometryRevision != scene.GetGeometryRevision()
            || m_active.surfaceRevision != scene.GetSurfaceRevision()))
    {
        ready = Prepare(scene, budgetBytes);
        if (ready)
        {
            RecordBuilds(scene);
        }
        else
        {
            // A stale TLAS is never a fallback for the current raster generation.
            ReleaseGenerations();
            if (std::strcmp(m_reason, "hardware_ready") == 0)
            {
                m_reason = "scene_build_preparation_failed";
            }
        }
    }
    return ready && IsReady();
}

void BindSceneRayGeometry(RDGPassDescBase& pass, RenderDevice& device, RenderScene& scene, bool forwardMaterialBound)
{
    pass.BindStorageBuffer("VertexBuffer", scene.GetVertexBuffer());
    pass.BindStorageBuffer("IndexBuffer", scene.GetIndexBuffer());
    if (!forwardMaterialBound)
    {
        pass.BindStorageBuffer("NodeBuffer", scene.GetNodesDataSSBO());
        pass.BindStorageBuffer("MaterialBuffer", scene.GetMaterialsDataSSBO());
        pass.BindStorageBuffer("UVBuffer", scene.GetUVBuffer());
        const RHISamplerCreateInfo samplerInfo = RHISamplerCreateInfo::CreateLinearRepeat();
        BindSceneTextureArray(pass, device.CreateSampler(samplerInfo), scene.GetSceneTextureViews(), scene.GetSceneSamplers());
    }
}

void SceneRayQuery::BindInputs(RDGPassDescBase& pass, RenderScene& scene, bool forwardMaterialBound) const
{
    const Generation& generation = Current();
    pass.BindAccelerationStructure("sceneAccelerationStructure", generation.tlas);
    pass.BindStorageBuffer("RayGeometryMetadata", generation.geometryMetadata);
    pass.BindStorageBuffer("RayInstanceMetadata", generation.instanceMetadata);
    BindSceneRayGeometry(pass, *m_device, scene, forwardMaterialBound);
    struct QueryUniforms
    {
        Vec4       minimum;
        Vec4       maximum;
        glm::uvec4 generation;
    };
    const QueryUniforms uniforms{
        Vec4(scene.GetAABB().GetMin(), 0), Vec4(scene.GetAABB().GetMax(), 0),
        glm::uvec4(static_cast<uint32_t>(generation.generation), static_cast<uint32_t>(generation.generation >> 32), 0, 0)};
    pass.BindValue("uRayQueryData", uniforms);
}

void SceneRayQuery::OnRenderGraphExecuted(RenderFrameResult result)
{
    const bool succeeded = result == RenderFrameResult::eSubmitted;

    if (m_recorded)
    {
        if (succeeded)
        {
            Cache(m_active);
            m_active  = std::move(m_pending);
            m_pending = Generation{};
        }
        else
        {
            if (result == RenderFrameResult::eDeclined)
            {
                Cache(m_pending);
            }
            else
            {
                ReleaseGenerations();

                m_reason = "failed_frame";
            }
        }
    }
    m_recorded = false;
}
} // namespace zen::rc
