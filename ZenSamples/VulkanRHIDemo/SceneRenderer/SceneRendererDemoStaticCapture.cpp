#include "SceneRendererDemo.h"
#include "Graphics/RenderCore/V2/Renderer/RendererServer.h"
#include "Graphics/RenderCore/V2/Renderer/DynamicVoxelGIRenderer.h"
#include "Graphics/RenderCore/V2/Renderer/VoxelizerBase.h"
#include "Graphics/RenderCore/V2/ComputeDispatch.h"
#include "Graphics/Shared/LightingCapture.h"
#include <fstream>
#include <iomanip>

namespace zen
{
namespace
{
void RecordStaticCapture(rc::RDGPassCmdEncoder& encoder,
                         const HeapVector<rc::ComputeDispatchChunk>& chunks)
{
    for (const rc::ComputeDispatchChunk& chunk : chunks)
    {
        encoder.SetPushConstants(glm::uvec2(chunk.firstItem, chunk.itemCount));
        encoder.Dispatch(chunk.groups.x, chunk.groups.y, chunk.groups.z);
    }
}
} // namespace

bool SceneRendererDemo::CaptureStaticGI(const std::string& path, uint32_t& fallbackFlags)
{
    rc::RendererServer& server           = *m_renderDevice->GetRendererServer();
    rc::DynamicVoxelGIRenderer* renderer = server.RequestDynamicVoxelGI();
    const rc::GIGridUniform& grid        = server.GetClassVisibility().GetInfo().grid;
    const uint32_t n                     = grid.dimensions.x;
    // The combined diagnostic readback exceeds RHI's uint32 byte size at 256 cubed.
    const uint32_t cells                 = n == 64 || n == 128 ? n * n * n : 0;
    const uint32_t faceBytes             = cells * GI_FACE_COUNT * 2 * sizeof(Vec4);
    const uint32_t legacyBytes       = faceBytes + cells * sizeof(uint32_t) + sizeof(glm::uvec4);
    const uint32_t workOffset        = legacyBytes;
    const uint32_t mapOffset         = workOffset + 16;
    const uint32_t staticListOffset  = mapOffset + cells * 4;
    const uint32_t dynamicListOffset = staticListOffset + cells * 4;
    const uint32_t dynamicMaskOffset = dynamicListOffset + cells * 4;
    const uint32_t dynamicFaceOffset = dynamicMaskOffset + cells * 4;
    const uint32_t bytes             = dynamicFaceOffset + faceBytes;
    uint64_t checked     = 0;
    bool valid                           = renderer != nullptr && cells != 0 &&
        rc::ValidateGIStorageBuffer(bytes, 1, m_renderDevice->GetGPUInfo(), checked) ==
            rc::GIResourceStatus::eSuccess;
    HeapVector<rc::ComputeDispatchChunk> chunks;
    uint32_t first = 0;
    while (valid && first < cells)
    {
        rc::ComputeDispatchChunk chunk;
        valid = rc::BuildComputeDispatchChunk(first, cells - first, GI_QUERY_GROUP_SIZE,
                                              m_renderDevice->GetGPUInfo(), chunk);
        if (valid)
        {
            chunks.push_back(chunk);
            first += chunk.itemCount;
        }
    }
    RHIBuffer* output      = nullptr;
    RHIBuffer* readback    = nullptr;
    rc::RenderGraph& graph = *m_renderDevice->GetCurrentFrameRDG();
    if (valid)
    {
        RHIBufferCreateInfo info;
        info.size         = faceBytes / GI_FACE_COUNT;
        info.allocateType = RHIBufferAllocateType::eGPU;
        info.usageFlags.SetFlags(RHIBufferUsageFlagBits::eStorageBuffer,
                                 RHIBufferUsageFlagBits::eTransferSrcBuffer);
        output            = m_renderDevice->CreateBuffer(info);
        info.size         = bytes;
        info.allocateType = RHIBufferAllocateType::eCPURead;
        info.usageFlags   = 0;
        info.usageFlags.SetFlag(RHIBufferUsageFlagBits::eTransferDstBuffer);
        readback = m_renderDevice->CreateBuffer(info);
        valid    = output != nullptr && readback != nullptr && graph.Begin();
    }
    if (valid)
    {
        for (uint32_t face = 0; face < GI_FACE_COUNT * 2; ++face)
        {
            rc::RDGComputePassDesc capture;
            capture.SetShaderProgramName("GIStaticCaptureSP");
            capture.SetPassTag("CaptureStaticGI");
            capture.independentDispatches = true;
            capture.BindStorageImage("rawIrradiance",
                                     (face < GI_FACE_COUNT ?
                                          renderer->GetRawIrradiance(face) :
                                          renderer->GetDynamicIrradiance(face % GI_FACE_COUNT))
                                         ->GetDefaultView());
            capture.BindStorageImage("paddedIrradiance",
                                     (face < GI_FACE_COUNT ? renderer->GetPaddedIrradiance(face) :
                                                             renderer->GetDynamicFilteredIrradiance(
                                                                 face % GI_FACE_COUNT))
                                         ->GetDefaultView());
            capture.BindStorageBuffer("StaticCapture", output, rc::RDGContentGuarantee::eFullWrite);
            graph.AddComputePass(std::move(capture))
                .RecordPassCommands([chunks](rc::RDGPassCmdEncoder& encoder) {
                    RecordStaticCapture(encoder, chunks);
                });
            graph.AddTransferPass("ReadStaticFace")
                .CopyBuffer(output, readback,
                            {0,
                             (face < GI_FACE_COUNT ? 0 : dynamicFaceOffset) +
                                 faceBytes / GI_FACE_COUNT * (face % GI_FACE_COUNT),
                             faceBytes / GI_FACE_COUNT})
                .NeverCull();
        }
        graph.AddTransferPass("ReadStaticGI")
            .CopyBuffer(renderer->GetLightMask(), readback,
                        {0, faceBytes, cells * sizeof(uint32_t)})
            .CopyBuffer(renderer->GetStatus(), readback,
                        {0, legacyBytes - sizeof(glm::uvec4), sizeof(glm::uvec4)})
            .CopyBuffer(renderer->GetWorkCounts(), readback, {0, workOffset, 16})
            .CopyBuffer(server.RequestVoxelizer(GI_STATIC)->GetGridToList(), readback,
                        {0, mapOffset, cells * 4})
            .CopyBuffer(renderer->GetReceiverList(GI_STATIC), readback,
                        {0, staticListOffset, cells * 4})
            .CopyBuffer(renderer->GetReceiverList(GI_DYNAMIC), readback,
                        {0, dynamicListOffset, cells * 4})
            .CopyBuffer(renderer->GetDynamicLightMask(), readback,
                        {0, dynamicMaskOffset, cells * 4})
            .NeverCull();
        valid = graph.End() && m_renderDevice->ExecuteRenderGraph(graph);
        m_renderDevice->FlushRHIThread();
        m_renderDevice->WaitForIdle();
    }
    if (valid)
    {
        const uint8_t* data = readback->Map();
        valid               = data != nullptr;
        if (valid)
        {
            glm::uvec4 status;
            std::memcpy(&status, data + legacyBytes - sizeof(status), sizeof(status));
            glm::uvec4 counts;
            std::memcpy(&counts, data + workOffset, sizeof(counts));
            fallbackFlags = status.z;
            if (fallbackFlags != 0)
            {
                LOGW("Static GI used cone fallback: flags {}, occupied {}, capacity {}", status.z,
                     status.x, status.y);
            }
            std::ofstream binary(path + ".static.bin", std::ios::binary);
            binary.write(reinterpret_cast<const char*>(data), bytes);
            binary.flush();
            std::ofstream metadata(path + ".static.json");
            metadata
                << std::setprecision(9) << "{\"resolution\":" << n << ",\"minimum_cell_size\":["
                << grid.minimumCellSize.x << ',' << grid.minimumCellSize.y << ','
                << grid.minimumCellSize.z << ',' << grid.minimumCellSize.w << ']'
                << ",\"occupied\":" << status.x << ",\"capacity\":" << status.y
                << ",\"rays_per_face\":" << renderer->GetRaysPerFace()
                << ",\"cache_stride\":" << renderer->GetCacheStride()
                << ",\"fallback_flags\":" << status.z
                << ",\"format_version\":3,\"dynamic_occupied\":" << status.w
                << ",\"selected_static\":" << counts.x << ",\"selected_dynamic\":" << counts.y
                << ",\"cache_updated\":" << counts.z << ",\"cache_ready_receivers\":" << counts.w
                << ",\"temporal_mode\":" << renderer->GetFilterUniform().control.x
                << ",\"spatial_filter\":" << renderer->GetFilterUniform().control.y
                << ",\"history_time\":" << renderer->GetFilterUniform().timing.x
                << ",\"history_reset\":" << renderer->GetFilterUniform().control.z
                << ",\"cache_batches\":" << renderer->GetCacheBuildBatches()
                << ",\"light_mask_update_bits\":" << renderer->GetLightMaskUpdateBits()
                << ",\"static_generation\":"
                << server.RequestVoxelizer(GI_STATIC)->GetGeometryRevision()
                << ",\"dynamic_generation\":"
                << server.RequestVoxelizer(GI_DYNAMIC)->GetGeometryRevision()
                << ",\"static_visibility_generation\":"
                << server.RequestVoxelizer(GI_STATIC)->GetRecordedVisibilityRevision()
                << ",\"dynamic_visibility_generation\":"
                << server.RequestVoxelizer(GI_DYNAMIC)->GetRecordedVisibilityRevision()
                << ",\"offsets\":{\"work\":" << workOffset << ",\"static_map\":" << mapOffset
                << ",\"static_list\":" << staticListOffset
                << ",\"dynamic_list\":" << dynamicListOffset
                << ",\"dynamic_mask\":" << dynamicMaskOffset
                << ",\"dynamic_faces\":" << dynamicFaceOffset << '}'
                << ",\"flags\":{\"overflow\":1,\"unknown\":2,\"unsupported\":4,\"cache_pending\":8}"
                << ",\"layout\":\"float4 raw/final pairs (static final includes padding), Z-fast cells, faces +X -X +Y -Y +Z -Z; uint light masks; uint4 status\""
                << ",\"raw_units\":\"irradiance, receiver BRDF excluded; hit terms scaled by indirect gain, escaped environment unscaled\""
                << ",\"surface_bytes_per_pixel\":" << ZEN_SURFACE_CAPTURE_BYTES_PER_PIXEL
                << ",\"surface_layout\":\"float4 position, shading normal, geometric normal, uint-bitcast instance/class/texel XY, albedo/metallic, AO/roughness/directional-ready/0\"}\n";
            metadata.flush();
            valid = binary.good() && metadata.good();
            readback->Unmap();
        }
    }
    m_renderDevice->DestroyBuffer(output);
    m_renderDevice->DestroyBuffer(readback);
    return valid;
}
} // namespace zen
