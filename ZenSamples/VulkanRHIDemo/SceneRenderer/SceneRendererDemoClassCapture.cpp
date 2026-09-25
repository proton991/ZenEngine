#include "SceneRendererDemo.h"
#include "Graphics/RenderCore/V2/Renderer/RendererServer.h"
#include "Graphics/RenderCore/V2/Renderer/DynamicVoxelGIRenderer.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>

namespace zen
{
namespace
{
bool CaptureGIQueries(rc::RenderDevice& device,
                      const rc::GIVisibilityProvider& provider,
                      const HeapVector<rc::GIQuery>& queries,
                      const std::string& path)
{
    const uint32_t count = static_cast<uint32_t>(queries.size());
    RHIBuffer* requests  = device.CreateStorageBuffer(
        count * sizeof(rc::GIQuery), reinterpret_cast<const uint8_t*>(queries.data()),
        "class_query_requests");
    RHIBufferCreateInfo info;
    info.size         = count * sizeof(rc::GIQueryResult);
    info.allocateType = RHIBufferAllocateType::eGPU;
    info.usageFlags.SetFlags(RHIBufferUsageFlagBits::eStorageBuffer,
                             RHIBufferUsageFlagBits::eTransferSrcBuffer);
    RHIBuffer* results = device.CreateBuffer(info);
    info.allocateType  = RHIBufferAllocateType::eCPURead;
    info.usageFlags    = 0;
    info.usageFlags.SetFlag(RHIBufferUsageFlagBits::eTransferDstBuffer);
    RHIBuffer* readback = device.CreateBuffer(info);
    bool valid          = provider.GetInfo().ready && requests != nullptr && results != nullptr &&
        readback != nullptr;
    if (valid)
    {
        rc::RenderGraph graph("ClassVisibilityCapture");
        valid = graph.Begin() &&
            rc::BuildGIQueryPass(graph, provider, requests, results, count, device.GetGPUInfo());
        if (valid)
        {
            graph.AddTransferPass("ReadClassQueries")
                .CopyBuffer(results, readback, {0, 0, info.size})
                .NeverCull();
            valid = graph.End() && device.ExecuteRenderGraph(graph);
        }
        device.FlushRHIThread();
        device.WaitForIdle();
        valid = valid && !device.AreSubmissionsBlocked();
        if (valid)
        {
            const uint8_t* mapped = readback->Map();
            valid                 = mapped != nullptr;
            if (valid)
            {
                std::ofstream input(path + ".queries.bin", std::ios::binary);
                std::ofstream output(path + ".query-results.bin", std::ios::binary);
                input.write(reinterpret_cast<const char*>(queries.data()),
                            count * sizeof(rc::GIQuery));
                output.write(reinterpret_cast<const char*>(mapped), info.size);
                input.flush();
                output.flush();
                valid = input.good() && output.good();
                readback->Unmap();
            }
        }
    }
    device.DestroyBuffer(requests);
    device.DestroyBuffer(results);
    device.DestroyBuffer(readback);
    return valid;
}
} // namespace

bool SceneRendererDemo::CaptureClassQueries(const std::string& path)
{
    const rc::VoxelDDAProvider& provider =
        m_renderDevice->GetRendererServer()->GetClassVisibility();
    const rc::GIGridUniform grid = provider.GetInfo().grid;
    HeapVector<rc::GIQuery> queries;
    // Cell-center lines avoid edge ambiguity. Each line exercises all three masks.
    for (uint32_t axis = 0; axis < 3; ++axis)
    {
        for (uint32_t i = 0; i < 64; ++i)
        {
            Vec3 origin(grid.minimumCellSize);
            origin[(axis + 1) % 3] +=
                (float(i % grid.dimensions.x) + 0.5f) * grid.minimumCellSize.w;
            origin[(axis + 2) % 3] +=
                (float((i * 13 + 8) % grid.dimensions.x) + 0.5f) * grid.minimumCellSize.w;
            origin[axis] -= grid.minimumCellSize.w;
            Vec3 direction(0);
            direction[axis] = 1;
            for (uint32_t mask = GI_STATIC; mask <= GI_ALL; ++mask)
            {
                queries.push_back(
                    {Vec4(origin, 0),
                     Vec4(direction, (grid.dimensions.x + 2) * grid.minimumCellSize.w),
                     glm::uvec4(mask, 0, GI_INVALID_CELL, 0)});
            }
        }
    }
    return CaptureGIQueries(*m_renderDevice, provider, queries, path);
}

bool SceneRendererDemo::CaptureGITraversal(const std::string& path)
{
    rc::RendererServer& server                 = *m_renderDevice->GetRendererServer();
    const rc::DynamicVoxelGIRenderer* renderer = server.RequestDynamicVoxelGI();
    rc::GIGridUniform grid                     = server.GetClassVisibility().GetInfo().grid;
    bool valid           = renderer != nullptr && server.GetClassVisibility().GetInfo().ready;
    const uint32_t cells = grid.dimensions.x * grid.dimensions.x * grid.dimensions.x;
    RHIBuffer* readback  = nullptr;
    HeapVector<rc::GIQuery> queries;
    glm::uvec4 counts(0);
    glm::uvec4 status(0);
    if (valid)
    {
        RHIBufferCreateInfo info;
        info.size         = 32 + cells * 8;
        info.allocateType = RHIBufferAllocateType::eCPURead;
        info.usageFlags.SetFlag(RHIBufferUsageFlagBits::eTransferDstBuffer);
        readback = m_renderDevice->CreateBuffer(info);
        rc::RenderGraph graph("GITraversalReceivers");
        valid = readback != nullptr && graph.Begin();
        if (valid)
        {
            graph.AddTransferPass("ReadGITraversalReceivers")
                .CopyBuffer(renderer->GetWorkCounts(), readback, {0, 0, 16})
                .CopyBuffer(renderer->GetStatus(), readback, {0, 16, 16})
                .CopyBuffer(renderer->GetReceiverList(GI_STATIC), readback, {0, 32, cells * 4})
                .CopyBuffer(renderer->GetReceiverList(GI_DYNAMIC), readback,
                            {0, 32 + cells * 4, cells * 4})
                .NeverCull();
            valid = graph.End() && m_renderDevice->ExecuteRenderGraph(graph);
        }
        m_renderDevice->FlushRHIThread();
        m_renderDevice->WaitForIdle();
        valid = valid && !m_renderDevice->AreSubmissionsBlocked();
        if (valid)
        {
            const uint8_t* mapped = readback->Map();
            valid                 = mapped != nullptr;
            if (valid)
            {
                std::memcpy(&counts, mapped, sizeof(counts));
                std::memcpy(&status, mapped + 16, sizeof(status));
                valid = counts.x <= cells && counts.y <= cells;
                for (uint32_t kind = 0; valid && kind < 2; ++kind)
                {
                    HeapVector<uint32_t> selected(counts[kind]);
                    if (!selected.empty())
                    {
                        std::memcpy(selected.data(), mapped + 32 + kind * cells * 4,
                                    selected.size() * sizeof(uint32_t));
                        std::sort(selected.begin(), selected.end());
                    }
                    const uint32_t samples = std::min(counts[kind], 128u);
                    const uint32_t rays    = renderer->GetRaysPerFace();
                    for (uint32_t sample = 0; sample < samples; ++sample)
                    {
                        const uint32_t cell = selected[uint64_t(sample) * counts[kind] / samples];
                        const uint32_t n  = grid.dimensions.x;
                        const Vec3 center = Vec3(grid.minimumCellSize) +
                            (Vec3(cell / (n * n), (cell / n) % n, cell % n) + Vec3(0.5f)) *
                                grid.minimumCellSize.w;
                        for (uint32_t face = 0; face < GI_FACE_COUNT; ++face)
                        {
                            Vec3 normal(0), tangent(0);
                            normal[face / 2]            = face % 2 == 0 ? 1.0f : -1.0f;
                            tangent[(face / 2 + 1) % 3] = 1;
                            const Vec3 origin = center + normal * (0.5f * grid.minimumCellSize.w);
                            for (uint32_t ray = 0; ray < rays; ++ray)
                            {
                                const float u   = (float(ray) + 0.5f) / float(rays);
                                const float phi = 2 * glm::pi<float>() *
                                    glm::fract(float(ray) * 0.6180339887498949f + 0.5f);
                                const Vec3 direction = glm::normalize(
                                    std::sqrt(u) * std::cos(phi) * tangent +
                                    std::sqrt(u) * std::sin(phi) * glm::cross(normal, tangent) +
                                    std::sqrt(1 - u) * normal);
                                for (uint32_t mask = kind == 0 ? GI_STATIC : GI_ALL; mask <=
                                     (kind == 0 ? (counts.y > 0 ? GI_DYNAMIC : GI_STATIC) : GI_ALL);
                                     ++mask)
                                {
                                    queries.push_back(
                                        {Vec4(origin, grid.minimumCellSize.w * 1e-4f),
                                         Vec4(direction,
                                              2 * std::sqrt(3.0f) * n * grid.minimumCellSize.w),
                                         glm::uvec4(mask, kind == 0 ? GI_STATIC : GI_DYNAMIC, cell,
                                                    0)});
                                }
                            }
                        }
                    }
                }
                readback->Unmap();
            }
        }
    }
    m_renderDevice->DestroyBuffer(readback);
    if (valid && !queries.empty())
    {
        grid.dimensions.w = 1;
        rc::VoxelDDAProvider diagnostic;
        valid =
            diagnostic.Prepare(server.RequestVoxelizer(GI_STATIC)->GetVoxelTextures(),
                               server.RequestVoxelizer(GI_DYNAMIC)->GetVoxelTextures(), grid, 1) &&
            CaptureGIQueries(*m_renderDevice, diagnostic, queries, path);
        std::ofstream metadata(path + ".traversal.json");
        metadata << "{\"selected_static\":" << counts.x << ",\"selected_dynamic\":" << counts.y
                 << ",\"fallback_flags\":" << status.z << ",\"dynamic_occupied\":" << status.w
                 << ",\"static_occupied\":" << status.x << ",\"cache_capacity\":" << status.y
                 << ",\"sampled_static\":" << std::min(counts.x, 128u)
                 << ",\"sampled_dynamic\":" << std::min(counts.y, 128u)
                 << ",\"rays_per_face\":" << renderer->GetRaysPerFace()
                 << ",\"queries\":" << queries.size() << '}';
        metadata.flush();
        valid = valid && metadata.good();
    }
    return valid;
}

bool SceneRendererDemo::CaptureClassState(const std::string& path)
{
    bool valid = true;
    for (uint32_t mask = GI_STATIC; mask <= GI_ALL; ++mask)
    {
        valid = valid && CaptureVoxelVolume(path + ".class" + std::to_string(mask), mask);
    }
    valid = valid && CaptureClassQueries(path) && CaptureFrame(path + ".ppm");
    return valid;
}

bool SceneRendererDemo::CaptureVoxelClasses(const std::string& path)
{
    rc::RendererServer* server = m_renderDevice->GetRendererServer();
    rc::DynamicVoxelGISettings settings;
    bool valid = !server->EnableClassVoxelization(1) &&
        server->RequestVoxelizer(GI_STATIC) == nullptr &&
        server->RequestVoxelizer(GI_DYNAMIC) == nullptr &&
        rc::LoadDynamicVoxelGISettings(platform::ConfigLoader::GetInstance(), settings) &&
        server->EnableClassVoxelization(settings.memoryBudgetBytes);
    HeapVector<sg::NodeData> originalNodes;
    const HeapVector<asset::Vertex> originalVertices = m_renderScene->GetVertices();
    for (const sg::Node* node : m_scene->GetRenderableNodes())
    {
        originalNodes.push_back(node->GetData());
    }
    valid                           = valid && originalNodes.size() >= 2;
    const uint64_t originalRevision = m_renderScene->GetGeometryRevision();
    Mat4 invalid(1);
    invalid[0][0] = std::numeric_limits<float>::quiet_NaN();
    valid         = valid && !m_renderScene->SetInstanceClass(0, GI_ALL) &&
        !m_renderScene->SetInstanceEnabled(UINT32_MAX, false) &&
        !m_renderScene->SetInstanceTransform(0, invalid) &&
        !m_renderScene->UpdateVertices(UINT32_MAX, {originalVertices.data(), 1}) &&
        m_renderScene->GetGeometryRevision() == originalRevision;
    if (valid)
    {
        valid = m_renderScene->SetInstanceTransform(0, glm::scale(Mat4(1), Vec3(1e-5f))) &&
            m_renderScene->SetInstanceTransform(0, originalNodes[0].modelMatrix);
    }
    for (uint32_t i = 0; valid && i < originalNodes.size(); ++i)
    {
        valid = m_renderScene->SetInstanceClass(i, i % 2 == 0 ? GI_STATIC : GI_DYNAMIC);
    }
    const char* stages[] = {"initial",  "moved", "deformed", "removed",
                            "promoted", "empty", "restored", "inflight"};
    for (uint32_t step = 0; valid && step < 8; ++step)
    {
        if (step == 1 || step == 7)
        {
            for (uint32_t i = 1; valid && i < originalNodes.size(); i += 2)
            {
                valid = m_renderScene->SetInstanceTransform(
                    i,
                    glm::translate(Mat4(1), Vec3(.023f, -.011f, .017f)) *
                        originalNodes[i].modelMatrix);
            }
        }
        else if (step == 2)
        {
            HeapVector<asset::Vertex> deformed = originalVertices;
            for (asset::Vertex& vertex : deformed)
            {
                vertex.pos.z += 7.0f * vertex.pos.x * vertex.pos.y *
                    (0.25f - vertex.pos.x * vertex.pos.x) * (0.25f - vertex.pos.y * vertex.pos.y);
            }
            valid = m_renderScene->UpdateVertices(0, {deformed.data(), deformed.size()});
        }
        else if (step == 3 || step == 5)
        {
            for (uint32_t i = 0; valid && i < originalNodes.size(); ++i)
            {
                if (step == 5 || (i % 2) != 0)
                {
                    valid = m_renderScene->SetInstanceEnabled(i, false);
                }
            }
        }
        else if (step == 4)
        {
            const uint32_t promoted = originalNodes.size() > 2 ? 2 : 0;
            valid                   = m_renderScene->SetInstanceClass(promoted, GI_DYNAMIC);
        }
        else if (step == 6)
        {
            for (uint32_t i = 0; valid && i < originalNodes.size(); ++i)
            {
                valid = m_renderScene->SetInstanceEnabled(i, true) &&
                    m_renderScene->SetInstanceClass(i, i % 2 == 0 ? GI_STATIC : GI_DYNAMIC) &&
                    m_renderScene->SetInstanceTransform(i, originalNodes[i].modelMatrix);
            }
            valid = valid &&
                m_renderScene->UpdateVertices(0,
                                              {originalVertices.data(), originalVertices.size()});
        }
        // PBR leaves combined voxels dirty. Entering cone must rebuild the current
        // generation. Run() flushes recording, but does not wait for GPU completion.
        if (valid && step == 7)
        {
            HeapVector<asset::Vertex> deformed = originalVertices;
            for (asset::Vertex& vertex : deformed)
            {
                vertex.pos.z += .002f;
            }
            valid = m_renderScene->UpdateVertices(0, {deformed.data(), deformed.size()});
            const uint64_t combinedRevision = server->RequestVoxelizer()->GetGeometryRevision();
            valid                           = valid && Run(1, false, 2) &&
                server->RequestVoxelizer()->GetGeometryRevision() == combinedRevision;
            for (uint32_t i = 1; valid && i < originalNodes.size(); i += 2)
            {
                valid = m_renderScene->SetInstanceTransform(i, originalNodes[i].modelMatrix);
            }
            valid = valid &&
                m_renderScene->UpdateVertices(0,
                                              {originalVertices.data(), originalVertices.size()});
        }
        // This oracle fixture deliberately moves boundary geometry. Request its
        // enlarged grid explicitly; ordinary scene updates keep the grid fixed.
        valid = valid && m_renderScene->CommitGeometryUpdates() &&
            m_renderScene->SetVoxelBounds(m_renderScene->GetAABB()) && Run(1, false, 3) &&
            CaptureClassState(path + "." + stages[step]);
    }
    if (!valid)
    {
        LOGE("Class voxel lifecycle capture failed: {}", path);
    }
    return valid;
}
} // namespace zen
