#include "Math/Math.h"
#include "Utils/Errors.h"
#include "Graphics/RenderCore/V2/Renderer/WorldRadianceCache.h"
#include "Graphics/RenderCore/V2/Renderer/HybridGIRenderer.h"
#include "Graphics/RenderCore/V2/RenderDevice.h"
#include "Graphics/RenderCore/V2/RenderScene.h"
#include "Graphics/RenderCore/V2/VoxelResourcePlanning.h"
#include <array>
#include <bit>
#include <cmath>

namespace zen::rc
{
namespace
{
constexpr uint32_t kWorldPathBytes = 64;
constexpr uint32_t kCellStateBytes = 12;
const char* const kNames[] = {"GICacheKeys", "GICacheAccumulation", "GICacheResolved", "WorldCacheCells", "WorldCacheLighting"};
} // namespace

WorldRadianceCache::WorldRadianceCache(RenderDevice* device) : m_device(device) {}
WorldRadianceCache::~WorldRadianceCache()
{
    Destroy();
}

uint64_t WorldRadianceCache::GetRequiredBytes() const
{
    return m_capacity != 0 ? uint64_t(m_capacity) * (8u + kCellStateBytes + 32u * m_records) + WorldCacheLightingBytes : 0;
}

void WorldRadianceCache::Destroy()
{
    for (RHIBuffer*& buffer : m_buffers)
    {
        m_device->DestroyBuffer(buffer);
        buffer = nullptr;
    }
    m_capacity       = 0;
    m_needsClear     = true;
    m_recorded       = false;
    m_geometry       = 0;
    m_surface        = 0;
    m_lightingPolicy = UINT32_MAX;
    m_lightingEpoch  = 0;
}

bool WorldRadianceCache::Allocate(RenderScene& scene, bool reset, uint32_t records, uint64_t pathBytes, uint32_t unusedFrames)
{
    // A changed record count changes every cell's layout: start an empty cache.
    if (records != m_records)
    {
        Destroy();
        m_records = records;
    }
    const bool budgetChanged =
        pathBytes != uint64_t(m_frameData.schedule.z) * kWorldPathBytes || unusedFrames != m_cache.limits.z;
    if (reset || budgetChanged || m_capacity == 0 || m_frame % 32u == 0u)
    {
        const uint32_t strides[] = {8, 16u * m_records, 16u * m_records, kCellStateBytes};
        // The anchor-free identity has six geometric faces per spatial patch.
        const uint32_t planned = PlanGIRadianceCache(m_device->GetGPUMemoryStats(), m_device->GetGPUInfo(),
                                                     scene.GetVoxelSceneBounds().GetExtent3D(), GetRequiredBytes(), strides,
                                                     WorldCacheLightingBytes + pathBytes, 600, 6);
        uint32_t       desired = m_capacity != 0 && !reset && !budgetChanged ? std::min(m_capacity, planned) : planned;
        // Keep a live cache across small budget fluctuations. The planner grants
        // one quarter of free headroom, so retaining at most twice that grant
        // still leaves at least the cache's size available below its heap limit.
        if (m_capacity != 0 && planned >= (m_capacity + 1u) / 2u)
        {
            desired = std::max(desired, m_capacity);
        }
        const uint32_t dispatchCapacity = std::bit_floor(uint32_t(
            std::min(uint64_t(UINT32_MAX / m_records), uint64_t(m_device->GetGPUInfo().maxComputeWorkGroupCount[0]) * 256u)));
        // Three vertices per path can occupy at most this many slots before they
        // expire. Leave half the table free for the four-probe insertion policy.
        const uint64_t liveLimit    = 6u * (pathBytes / kWorldPathBytes) * unusedFrames;
        const uint32_t pathCapacity = std::bit_ceil(uint32_t(std::min(liveLimit, uint64_t(dispatchCapacity))));
        desired                     = std::min({desired, dispatchCapacity, pathCapacity});
        if (desired != m_capacity)
        {
            Destroy();
            bool allocated = false;
            while (desired != 0 && !allocated)
            {
                allocated = true;
                for (uint32_t index = 0; index < std::size(m_buffers) && allocated; ++index)
                {
                    const uint32_t bytes = index < std::size(strides) ? desired * strides[index] : WorldCacheLightingBytes;
                    m_buffers[index]     = m_device->CreateStorageBuffer(bytes, nullptr, kNames[index]);
                    allocated            = m_buffers[index] != nullptr;
                }
                if (!allocated)
                {
                    Destroy();
                    desired /= 2u;
                }
            }
            m_capacity = desired;
            LOGI("World radiance cache: {} cells, {} bytes, storage range {}, bounds {}x{}x{}", m_capacity, GetRequiredBytes(),
                 m_device->GetGPUInfo().maxStorageBufferRange, scene.GetVoxelSceneBounds().GetExtent3D().x,
                 scene.GetVoxelSceneBounds().GetExtent3D().y, scene.GetVoxelSceneBounds().GetExtent3D().z);
        }
    }
    return m_capacity != 0;
}

void WorldRadianceCache::Bind(RDGPassDescBase& pass) const
{
    pass.BindValue("uGICacheData", m_cache);
    pass.BindValue("uWorldCacheData", m_frameData);
    for (uint32_t index = 0; index < std::size(m_buffers); ++index)
    {
        pass.BindStorageBuffer(kNames[index], m_buffers[index]);
    }
}

void WorldRadianceCache::BindLighting(RDGPassDescBase& pass) const
{
    pass.BindStorageBuffer(kNames[4], m_buffers[4]);
}

bool WorldRadianceCache::Prepare(VoxelGIRenderer& voxelGI, RenderScene& scene, glm::uvec2 extent, bool reset)
{
    m_recorded                           = false;
    const VoxelGISettings& cacheSettings = voxelGI.GetSettings();
    const uint32_t         records       = cacheSettings.worldCacheSeed && !cacheSettings.screenProbeGI ? 7u : 6u;
    const uint32_t         tileSize      = cacheSettings.worldCacheTileSize;
    const glm::uvec2       tiles         = (extent + (tileSize - 1u)) / tileSize;
    const uint64_t         pathBytes     = uint64_t(tiles.x) * tiles.y * kWorldPathBytes;
    const bool             ready         = (voxelGI.UsesRayTracingPipeline() || voxelGI.UsesFieldTraversal()) && pathBytes > 0
                                        && pathBytes <= m_device->GetGPUInfo().maxStorageBufferRange
                                        && Allocate(scene, reset, records, pathBytes, cacheSettings.worldCacheUnusedFrames);
    if (ready)
    {
        m_tiles       = tiles;
        m_needsClear |= reset;
    }
    return ready;
}

void WorldRadianceCache::Record(VoxelGIRenderer&           voxelGI,
                                RenderScene&               scene,
                                const HybridGIUniformData& receiver,
                                RHISampler*                sampler)
{
    VERIFY_EXPR(m_capacity != 0);
    const VoxelGISettings&  settings   = voxelGI.GetSettings();
    const glm::uvec2        tiles      = m_tiles;
    RenderGraph&            graph      = *m_device->GetCurrentFrameRDG();
    const uint32_t          bits       = m_device->GetGPUInfo().supportBufferInt64Atomics ? 64 : 32;
    const uint32_t          cellGroups = (m_capacity + 255u) / 256u;
    const SceneUniformData& lighting   = *reinterpret_cast<const SceneUniformData*>(scene.GetSceneUniformData());
    m_cache.previousCamera             = m_cache.camera;
    m_cache.camera                     = lighting.viewPos;
    const float scale                  = scene.GetVoxelSceneBounds().GetMaxExtent();
    // Surface-cell demand scales quadratically. Keep density proportional to the
    // granted capacity so memory pressure does not saturate a fixed fine grid.
    // Surface coverage is also limited by independent path origins. Scale
    // the footprint for small render targets at the same paths per cell as 4K.
    const float pathScale  = std::max(1.0f, std::sqrt(129600.0f / float(tiles.x * tiles.y)));
    const float cellScale  = .001f * std::max(pathScale, std::sqrt(2097152.0f / float(m_capacity)));
    const float cellSize   = std::max(scale * cellScale, 1e-7f);
    m_needsClear          |= m_cache.grid.x != cellSize;
    m_cache.grid           = Vec4(cellSize, std::max(scale * .05f, 1e-5f), 0, 0);
    m_cache.grid.z         = settings.worldCacheFeedback ? 1.0f : 0.0f;
    m_cache.limits         = glm::uvec4(m_capacity, settings.worldCacheHistory, settings.worldCacheUnusedFrames, 0u);
    const uint32_t lightingPolicy =
        uint32_t(settings.analyticLighting) | (uint32_t(settings.emissiveLighting) << 1u) | (uint32_t(settings.shadows) << 2u);
    if (m_lightingPolicy != UINT32_MAX && (m_lightingPolicy != lightingPolicy || m_surface != scene.GetSurfaceRevision()))
    {
        m_lightingEpoch = (m_lightingEpoch + 1u) & 0x3fffffffu;
    }
    m_lightingPolicy   = lightingPolicy;
    m_surface          = scene.GetSurfaceRevision();
    m_recordedGeometry = scene.GetGeometryRevision();
    // Pack only enabled finite sources for cell-footprint selection. Far from
    // a local light the camera-selected distance level remains the limiting one.
    m_frameData.lightCount.x = 0;
    m_frameData.lightCount.y = m_records;
    m_frameData.lightCount.z = std::bit_cast<uint32_t>(settings.indirectIntensity);
    const uint32_t count     = uint32_t(lighting.lightInfo.x);
    for (uint32_t index = 0; index <= count && settings.analyticLighting; ++index)
    {
        const GPULight& light = index == count ? lighting.cameraLight : lighting.lights[index];
        if (light.directionType.w > .5f && light.colorIntensity.w > 0.0f)
        {
            m_frameData.localLights[m_frameData.lightCount.x++] = light.positionRange;
        }
    }
    m_frameData.schedule =
        glm::uvec4(m_frame + 1u, settings.worldCacheTileSize, tiles.x * tiles.y,
                   (m_needsClear ? 1u : 0u) | (m_geometry != m_recordedGeometry ? 2u : 0u) | (m_lightingEpoch << 2u));
    if (m_needsClear)
    {
        RDGComputePassDesc clear;
        clear.SetShaderProgramName("WorldCacheInitializeSP");
        clear.SetPassTag("WorldCacheInitialize");
        for (uint32_t index = 0; index < std::size(m_buffers); ++index)
        {
            clear.BindStorageBuffer(kNames[index], m_buffers[index], RDGContentGuarantee::eFullWrite);
        }
        graph.AddComputePass(std::move(clear)).RecordPassCommands([cellGroups](RDGPassCmdEncoder& encoder) {
            encoder.Dispatch(cellGroups, 1, 1);
        });
    }
    RDGComputePassDesc lightingPass;
    lightingPass.SetShaderProgramName("WorldCacheLightingSP");
    lightingPass.SetPassTag("WorldCacheLighting");
    BindLighting(lightingPass);
    lightingPass.BindValue("uSceneData", lighting);
    const std::array<uint32_t, 5> lightingUpdate{m_frameData.schedule.x, m_frameData.schedule.z, m_frameData.schedule.w,
                                                 settings.worldCacheHistory, uint32_t(settings.worldCacheFeedback)};
    graph.AddComputePass(std::move(lightingPass)).RecordPassCommands([lightingUpdate](RDGPassCmdEncoder& encoder) {
        encoder.SetPushConstants(lightingUpdate);
        encoder.Dispatch(1, 1, 1);
    });
    if (m_needsClear)
    {
        m_frameData.schedule.w &= ~3u;
    }
    RDGBufferDesc pathDesc;
    pathDesc.name = "world_cache_paths";
    pathDesc.size = uint64_t(tiles.x) * tiles.y * kWorldPathBytes;
    pathDesc.usageFlags.SetFlag(RHIBufferUsageFlagBits::eStorageBuffer);
    const RDGBuffer    paths = graph.GetResourceManager()->CreateBuffer(pathDesc);
    RDGComputePassDesc start;
    start.SetShaderProgramName(NameID(fmt::format("WorldCacheStart{}SP", bits)));
    start.SetPassTag("WorldCacheStart");
    Bind(start);
    BindHybridReceiverGuides(start, sampler, receiver);
    start.BindStorageBuffer("WorldCachePaths", paths, RDGContentGuarantee::eFullWrite);
    graph.AddComputePass(std::move(start)).RecordPassCommands([tiles](RDGPassCmdEncoder& encoder) {
        encoder.Dispatch((tiles.x + 7u) / 8u, (tiles.y + 7u) / 8u, 1);
    });
    const bool   hardware = voxelGI.UsesRayTracingPipeline();
    const bool   wave     = m_device->GetGPUInfo().supportComputeSubgroupReduction;
    const NameID shadeProgram(fmt::format("WorldCacheShade{}{}{}SP", hardware ? "" : "Field", wave ? "Wave" : "", bits));
    for (uint32_t bounce = 0; bounce <= 3; ++bounce)
    {
        if (hardware)
        {
            RDGRayTracingPassDesc trace;
            trace.SetShaderProgramName(NameID(fmt::format("WorldCacheTraceHits{}SP", bits)));
            trace.SetPassTag("WorldCacheTraceHits");
            Bind(trace);
            BindHybridReceiverGuides(trace, sampler, receiver);
            trace.BindValue("uSceneData", lighting);
            voxelGI.BindHardwareRayInputs(trace, false);
            voxelGI.BindLightingInputs(trace, true);
            voxelGI.BindEnvironmentSamplingInputs(trace);
            trace.BindStorageBuffer("WorldCachePaths", paths);
            SetHybridRayTracingGroups(trace, 1);
            graph.AddRayTracingPass(std::move(trace)).RecordPassCommands([tiles](RDGPassCmdEncoder& encoder) {
                encoder.TraceRays(tiles.x, tiles.y, 1);
            });
        }
        RDGComputePassDesc shade;
        shade.SetShaderProgramName(shadeProgram);
        shade.SetPassTag("WorldCacheShade");
        Bind(shade);
        BindHybridReceiverGuides(shade, sampler, receiver);
        shade.BindValue("uSceneData", lighting);
        shade.BindStorageBuffer("WorldCachePaths", paths);
        if (hardware)
        {
            voxelGI.BindHardwareRayInputs(shade, false);
            voxelGI.BindLightingInputs(shade, true);
        }
        else
        {
            voxelGI.BindNativeFieldInputs(shade);
        }
        voxelGI.BindEnvironmentSamplingInputs(shade);
        graph.AddComputePass(std::move(shade)).RecordPassCommands([bounce, tiles](RDGPassCmdEncoder& encoder) {
            encoder.SetPushConstants(bounce);
            encoder.Dispatch((tiles.x + 7u) / 8u, (tiles.y + 7u) / 8u, 1);
        });
    }
    RDGComputePassDesc resolve;
    resolve.SetShaderProgramName(NameID(fmt::format("WorldCacheResolve{}SP", bits)));
    resolve.SetPassTag("WorldCacheResolve");
    Bind(resolve);
    graph.AddComputePass(std::move(resolve)).RecordPassCommands([cellGroups](RDGPassCmdEncoder& encoder) {
        encoder.Dispatch(cellGroups, 1, 1);
    });
    m_recorded = true;
}

RDGTexture WorldRadianceCache::RecordDebug(const HybridGIUniformData& receiver, RHISampler* sampler, glm::uvec2 extent) const
{
    RenderGraph&   graph = *m_device->GetCurrentFrameRDG();
    RDGTextureDesc desc;
    desc.name                = "world_cache_irradiance";
    desc.texFormat.dimension = TextureDimension::e2D;
    desc.texFormat.width     = extent.x;
    desc.texFormat.height    = extent.y;
    desc.texFormat.depth     = 1;
    desc.texFormat.format    = DataFormat::eR16G16B16A16SFloat;
    desc.usageFlags.SetFlags(RHITextureUsageFlagBits::eStorage, RHITextureUsageFlagBits::eSampled);
    const RDGTexture   output = graph.GetResourceManager()->CreateTexture(desc);
    RDGComputePassDesc pass;
    pass.SetShaderProgramName(
        NameID(fmt::format("WorldCacheDebug{}SP", m_device->GetGPUInfo().supportBufferInt64Atomics ? 64 : 32)));
    pass.SetPassTag("WorldCacheDebug");
    Bind(pass);
    BindHybridReceiverGuides(pass, sampler, receiver);
    pass.BindStorageImage("worldIrradiance", output, RDGContentGuarantee::eFullWrite);
    const glm::uvec2 groups = (extent + glm::uvec2(7)) / 8u;
    graph.AddComputePass(std::move(pass)).RecordPassCommands([groups](RDGPassCmdEncoder& encoder) {
        encoder.Dispatch(groups.x, groups.y, 1);
    });
    return output;
}

void WorldRadianceCache::OnRenderGraphExecuted(RenderFrameResult result)
{
    if (m_recorded && result == RenderFrameResult::eSubmitted)
    {
        m_needsClear = false;
        m_geometry   = m_recordedGeometry;
        ++m_frame;
    }
    else if (result != RenderFrameResult::eDeclined && result != RenderFrameResult::eSubmitted)
    {
        Destroy();
    }
    m_recorded = false;
}
} // namespace zen::rc
