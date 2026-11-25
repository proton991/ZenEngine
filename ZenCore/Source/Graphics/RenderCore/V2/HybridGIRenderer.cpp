#include "Math/Math.h"
#include "Utils/Errors.h"
#include "Graphics/RenderCore/V2/Renderer/HybridGIRenderer.h"
#include "Graphics/RenderCore/V2/RenderDevice.h"
#include "Graphics/RenderCore/V2/RenderScene.h"
#include "Graphics/RenderCore/V2/RenderConfig.h"
#include "SceneGraph/Camera.h"
#include "Graphics/Shared/EnvironmentSampling.h"
#include "Graphics/Shared/HybridGI.h"
#include "Graphics/Shared/ReSTIRGI.h"
#include "Graphics/RenderCore/V2/VoxelResourcePlanning.h"
#include <cstring>
#include <string_view>

namespace zen::rc
{
namespace
{
const char* const kHistoryNames[]   = {"Sky",     "Specular",   "Moments",      "Length",        "Position",
                                       "Normal",  "Receiver",   "Bounce",       "BounceMoments", "Reflection",
                                       "FastSky", "FastBounce", "FastSpecular", "ResponseAge"};
const DataFormat  kHistoryFormats[] = {
    DataFormat::eR32G32B32A32SFloat, DataFormat::eR16G16B16A16SFloat, DataFormat::eR32G32B32A32SFloat,
    DataFormat::eR16G16B16A16SFloat, DataFormat::eR32G32B32A32SFloat, DataFormat::eR16G16SFloat,
    DataFormat::eR32G32UInt,         DataFormat::eR32G32B32A32SFloat, DataFormat::eR32G32SFloat,
    DataFormat::eR32G32B32A32SFloat, DataFormat::eR32G32B32A32SFloat, DataFormat::eR32G32B32A32SFloat,
    DataFormat::eR32G32B32A32SFloat, DataFormat::eR32G32B32A32SFloat};
// The trace pass learns and reads the visibility guide per workgroup of this many pixels squared.
constexpr uint32_t kGuideTile = 16;
// Match the Counts, Field, Young and Exact suffix order used by ShaderProgram.cpp.
// Variants without Field trace HW RT only; Compute features trace the distance field.
NameID TraceProgram(const char* base, bool counts, bool field, bool exactGuide = false, bool young = false)
{
    return NameID(fmt::format("{}{}{}{}{}SP", base, counts ? "Counts" : "", field ? "Field" : "", young ? "Young" : "",
                              exactGuide ? "Exact" : ""));
}

RDGTexture DenoiseSignalTexture(RDGResourceManager& resources, NameID name, glm::uvec2 extent, DataFormat format)
{
    RDGTextureDesc desc;
    desc.name                = name;
    desc.texFormat.dimension = TextureDimension::e2D;
    desc.texFormat.width     = extent.x;
    desc.texFormat.height    = extent.y;
    desc.texFormat.depth     = 1;
    desc.texFormat.format    = format;
    desc.usageFlags.SetFlags(RHITextureUsageFlagBits::eStorage, RHITextureUsageFlagBits::eSampled);
    return resources.CreateTexture(desc);
}
} // namespace

HybridGIRenderer::HybridGIRenderer(RenderDevice* device) : m_worldCache(device), m_screenProbes(device), m_device(device)
{
    RHISamplerCreateInfo info{};
    m_sampler = m_device->CreateSampler(info);
}

uint64_t HybridGIRenderer::GetRayCountCaptureBytes(uint32_t width, uint32_t height)
{
    // Per tile: ray, candidate and field-work counts from the trace pass.
    return ((uint64_t(width) + kGuideTile - 1) / kGuideTile) * ((uint64_t(height) + kGuideTile - 1) / kGuideTile)
             * ZEN_GI_TRACE_COUNT_WORDS * sizeof(uint32_t)
         + uint64_t(width) * height * 8u * sizeof(uint32_t);
}

void HybridGIRenderer::InvalidateHistory(const char* reason)
{
    m_valid       = false;
    m_resetReason = reason;
}

void HybridGIRenderer::Prepare(const RenderView& view, RenderScene& scene)
{
    m_recorded = false;
    m_prepared = view.width != 0 && view.height != 0;
    if (m_scene != &scene)
    {
        m_scene = &scene;
        m_valid = false;
        m_previousModels.clear();
        m_previousVertices = nullptr;
        m_resetReason      = "scene";
    }
    if (m_valid && !m_probePath && !scene.HasVertexGeneration(m_previousVertices.Get(), m_previousVertexGeneration))
    {
        InvalidateHistory("vertex_ring_wrapped");
    }
    m_preparedHistoryValid              = m_valid;

    m_preparedResetReason               = m_resetReason;

    m_preparedResponsiveFrames          = m_bounceResponsiveFrames;

    const sg::CameraUniformData& camera = *reinterpret_cast<const sg::CameraUniformData*>(scene.GetCameraUniformData());
    m_recordedProjection                = camera.proj;
    m_recordedVertices                  = scene.GetVertexBuffer();
    m_recordedVertexGeneration          = scene.GetVertexGeneration();
    m_recordedProjectionView            = camera.projViewMatrix;
    const SceneUniformData& lighting    = *reinterpret_cast<const SceneUniformData*>(scene.GetSceneUniformData());
    m_recordedViewPosition              = lighting.viewPos;
    m_recordedViewDirection             = Vec4(glm::normalize(Vec3(glm::inverse(camera.view)[2])), 0);
    m_uniforms.reconstruction           = BuildGBufferUniformData(camera.projViewMatrix);
    m_recordedModels.clear();
    for (const sg::Node* node : scene.GetRenderableNodes())
    {
        const uint32_t index = node->GetRenderableIndex();
        if (m_recordedModels.size() <= index)
        {
            m_recordedModels.resize(index + 1);
        }
        m_recordedModels[index] = scene.GetInstanceTransform(index);
    }
    if (!m_valid)
    {
        m_uniforms.previousProjectionView = camera.projViewMatrix;
    }
}

void HybridGIRenderer::BindReceiverInputs(RDGPassDescBase& pass) const
{
    pass.BindValue("uPreviousCamera", m_uniforms.previousProjectionView);
    pass.BindStorageBuffer("PreviousVertices",
                           m_valid && m_previousVertices ? m_previousVertices.Get() : m_recordedVertices.Get());
}

HybridReceiverDraw HybridGIRenderer::ReceiverDraw(uint32_t node, uint32_t material) const
{
    HybridReceiverDraw result;
    result.indices       = glm::uvec4(node, material, 0, 0);
    result.previousModel = PreviousModel(node);
    return result;
}

Mat4 HybridGIRenderer::PreviousModel(uint32_t node) const
{
    return m_valid && node < m_previousModels.size() ? m_previousModels[node] : m_recordedModels[node];
}

bool HybridGIRenderer::PrepareHistory(uint32_t width,
                                      uint32_t height,
                                      bool     reflections,
                                      bool     reference,
                                      uint32_t restirMask,
                                      bool     relax,
                                      bool     skyLight,
                                      bool     guideEnabled,
                                      bool     antiLag)
{
    const bool     restir           = restirMask != 0;
    const uint64_t storageAlignment = std::max(uint64_t(1), uint64_t(m_device->GetGPUInfo().storageBufferAlignment));
    bool           valid            = true;
    for (uint32_t side = 0; side < 2 && valid; ++side)
    {
        for (uint32_t texture = 0; texture < Count && valid; ++texture)
        {
            // Disabled signals use 1x1 placeholders. Enabling them resets their own histories.
            // relax is false only for the reference diagnostic, whose mean keeps bounce moments.
            const bool       full   = texture == Sky           ? !relax || skyLight
                                    : texture == FastSky       ? relax && skyLight
                                    : texture == ResponseAge   ? relax && antiLag
                                    : texture >= FastBounce    ? relax
                                    : texture == BounceMoments ? !relax
                                    : texture == Reflection    ? reflections || relax
                                                               : true;
            const glm::uvec2 extent = full ? glm::uvec2(width, height) : glm::uvec2(1);
            // Long reference accumulations must not repeatedly round the specular mean to FP16.
            const DataFormat historyFormat = (reference && texture == Specular) || (relax && texture == Length)
                                               ? DataFormat::eR32G32B32A32SFloat
                                               : kHistoryFormats[texture];
            RHITexture*&     history       = m_history[side][texture];
            if (history != nullptr
                && (history->GetWidth() != extent.x || history->GetHeight() != extent.y
                    || history->GetFormat() != historyFormat))
            {
                m_device->DestroyTexture(history);
                history = nullptr;
            }
            if (history == nullptr)
            {
                TextureFormat format;
                format.dimension         = TextureDimension::e2D;
                format.depth             = 1;
                format.width             = extent.x;
                format.height            = extent.y;
                format.format            = historyFormat;
                m_history[side][texture] = m_device->CreateTextureStorage(
                    format, {.copyUsage = true}, NameID(fmt::format("hybrid_history_{}_{}", side, texture)));
                valid                              = m_history[side][texture] != nullptr;

                m_historyNeedsClear[side][texture] = valid;
            }

            if (valid && m_historyNeedsClear[side][texture])
            {
                // Retain initialization across a declined frame, including newly resized histories.
                m_device->GetCurrentFrameRDG()
                    ->AddTransferPass("InitializeHybridHistory")
                    .ClearTexture(m_history[side][texture], Color(0));
            }
        }
    }
    const glm::uvec2 tiles = (glm::uvec2(width, height) + glm::uvec2(kGuideTile - 1)) / kGuideTile;
    // No-guide shaders never index either buffer. Keep only a descriptor placeholder
    // until environment learning or a diagnostic capture requires the tile arrays.
    const uint32_t guideBytes       = (guideEnabled ? tiles.x * tiles.y * (ZEN_HYBRID_GUIDE_TILES + 1u) : 1u) * sizeof(float);
    const uint64_t paddedGuideBytes = (uint64_t(guideBytes) + storageAlignment - 1) / storageAlignment * storageAlignment;
    for (uint32_t side = 0; side < 2 && valid; ++side)
    {
        if (m_guide[side] != nullptr && m_guide[side]->GetRequiredSize() != paddedGuideBytes)
        {
            m_device->DestroyBuffer(m_guide[side]);
            m_guide[side] = nullptr;
            // The old guide storage is gone even if this recording is later declined.
            m_previousGuideWritten = false;
        }
        if (m_guide[side] == nullptr)
        {
            // Per tile: the environment-tile weights and the number of frames they average.
            // Both sides are bound on the first frame, even while guide reuse is disabled.
            const HeapVector<uint8_t> initial(guideBytes, 0);
            m_guide[side] =
                m_device->CreateStorageBuffer(guideBytes, initial.data(), NameID(fmt::format("hybrid_guide_{}", side)));
            valid = m_guide[side] != nullptr;
        }
    }
    uint64_t reservoirBytes = 0;
    valid                   = valid
                           && (!restir
                               || ValidateGIStorageBuffer(uint64_t(width) * height, ZEN_RESTIR_RESERVOIR_BYTES, m_device->GetGPUInfo(),
                                                          reservoirBytes)
                                      == GIResourceStatus::eSuccess);
    for (uint32_t side = 0; side < 2 && valid; ++side)
    {
        for (uint32_t type = 0; type < 2 && valid; ++type)
        {
            const uint32_t bytes =
                (restirMask & (1u << type)) != 0 ? static_cast<uint32_t>(reservoirBytes) : ZEN_RESTIR_RESERVOIR_BYTES;
            const uint64_t paddedBytes = (uint64_t(bytes) + storageAlignment - 1) / storageAlignment * storageAlignment;
            if (m_reservoir[side][type] != nullptr && (!restir || m_reservoir[side][type]->GetRequiredSize() != paddedBytes))
            {
                m_device->DestroyBuffer(m_reservoir[side][type]);
                m_reservoir[side][type] = nullptr;
                m_preparedHistoryValid  = false;
                InvalidateHistory("restir_layout");
            }
            if (restir && m_reservoir[side][type] == nullptr)
            {
                m_reservoir[side][type] =
                    m_device->CreateStorageBuffer(bytes, nullptr, NameID(fmt::format("restir_gi_{}_{}", side, type)));
                valid                  = m_reservoir[side][type] != nullptr;
                m_preparedHistoryValid = false;
                InvalidateHistory("restir_allocated");
            }
        }
    }
    if (!restir && m_restirLobeIntegral != nullptr)
    {
        m_device->DestroyTexture(m_restirLobeIntegral);
        m_restirLobeIntegral    = nullptr;
        m_restirLobeInitialized = false;
    }
    if (valid && restir && m_restirLobeIntegral == nullptr)
    {
        TextureFormat format;
        format.dimension = TextureDimension::e2D;
        format.width = format.height = 128;
        format.depth                 = 1;
        format.format                = DataFormat::eR32SFloat;
        m_restirLobeIntegral         = m_device->CreateTextureStorage(format, {}, "restir_lobe_integral");
        valid                        = m_restirLobeIntegral != nullptr;
    }
    if (!valid)
    {
        Destroy();
    }
    return valid;
}

void SetHybridRayTracingGroups(RDGRayTracingPassDesc& pass, uint32_t maxDepth)
{
    pass.SetShaderGroups({{RHIRayTracingShaderGroupType::eRayGeneration, 0},
                          {RHIRayTracingShaderGroupType::eMiss, 1},
                          {RHIRayTracingShaderGroupType::eMiss, 2},
                          {RHIRayTracingShaderGroupType::eHit, kUnusedRayTracingShader, 3, 4},
                          {RHIRayTracingShaderGroupType::eHit, kUnusedRayTracingShader, kUnusedRayTracingShader, 5}},
                         maxDepth);
}

void BindHybridReceiverGuides(RDGPassDescBase& pass, RHISampler* sampler, const HybridGIUniformData& uniforms)
{
    pass.BindValue("uHybridData", uniforms);
    pass.BindSampledTexture("receiverDepth", sampler, "offscreen_depth");
    pass.BindSampledTexture("receiverNormal", sampler, "offscreen_normal");
    pass.BindSampledTexture("receiverRoughness", sampler, "offscreen_roughness");
    pass.BindSampledTexture("receiverSurface", sampler, "offscreen_receiver");
    pass.BindSampledTexture("receiverMotion", sampler, "offscreen_motion");
    pass.BindSampledTexture("receiverPosition", sampler, "offscreen_position");
}

void HybridGIRenderer::BindGuides(RDGPassDescBase& pass) const
{
    if (m_extent == m_outputExtent)
    {
        BindHybridReceiverGuides(pass, m_sampler, m_uniforms);
    }
    else
    {
        pass.BindValue("uHybridData", m_uniforms);
        const char* inputs[] = {"receiverDepth",   "receiverNormal", "receiverRoughness",
                                "receiverSurface", "receiverMotion", "receiverPosition"};
        for (uint32_t index = 0; index < 6; ++index)
        {
            pass.BindSampledTexture(inputs[index], m_sampler, m_receiverGuides[index]);
        }
    }
}

void HybridGIRenderer::BindTraceInputs(RDGDispatchPassDesc& pass,
                                       VoxelGIRenderer&     voxelGI,
                                       const glm::uvec4&    reuseData,
                                       bool                 worldField) const
{
    BindGuides(pass);
    pass.BindValue("uHybridReuseData", reuseData);
    if (reuseData.w != 0u)
    {
        for (uint32_t texture : {Length, Position, Normal, Receiver, Reflection})
        {
            pass.BindSampledTexture(NameID(fmt::format("previous{}", kHistoryNames[texture])), m_sampler,
                                    m_history[1 - m_current][texture]->GetDefaultView());
        }
    }
    voxelGI.BindEnvironmentSamplingInputs(pass);
    if (worldField)
    {
        voxelGI.BindNativeFieldInputs(pass);
    }
    else
    {
        voxelGI.BindHitRadianceInputs(pass);
        if (m_uniforms.provider.x != 0)
        {
            voxelGI.BindHardwareRayInputs(pass);
        }
        else
        {
            voxelGI.BindRayInputs(pass);
        }
        if (voxelGI.UsesFieldTraversal())
        {
            voxelGI.BindFieldInputs(pass);
        }
    }
    pass.BindValue("uSceneData", m_scene->GetSceneUniformData(), sizeof(SceneUniformData));
    pass.BindStorageBuffer("HybridGuidePrevious", m_guide[1 - m_current]);
}

void HybridGIRenderer::DestroyRadianceCache()
{
    for (RHIBuffer*& buffer : m_cacheBuffers)
    {
        m_device->DestroyBuffer(buffer);
        buffer = nullptr;
    }
    m_cacheCapacity   = 0;
    m_cacheNeedsClear = true;
}

void HybridGIRenderer::BindRadianceCache(RDGPassDescBase& pass) const
{
    pass.BindValue("uGICacheData", m_cacheUniforms);
    const char* names[] = {"GICacheKeys", "GICacheAccumulation", "GICacheResolved"};
    for (uint32_t index = 0; index < 3; ++index)
    {
        pass.BindStorageBuffer(names[index], m_cacheBuffers[index],
                               m_cacheUniforms.grid.w != 0.0f ? RDGContentGuarantee::eFullWrite : RDGContentGuarantee::eNone);
    }
}

void HybridGIRenderer::RecordRadianceCache(VoxelGIRenderer& voxelGI, bool enabled, bool noGuide)
{
    const bool     requested        = voxelGI.GetSettings().radianceCache || voxelGI.GetSettings().nativeProbeBenchmark;
    const bool     reconsider       = requested != m_cacheRequested || !m_valid || m_frame % 32u == 0u;
    const uint32_t previousCapacity = m_cacheCapacity;
    if (!enabled)
    {
        DestroyRadianceCache();
    }
    else if (reconsider)
    {
        const uint32_t planned =
            PlanGIRadianceCache(m_device->GetGPUMemoryStats(), m_device->GetGPUInfo(),
                                m_scene->GetVoxelSceneBounds().GetExtent3D(), uint64_t(m_cacheCapacity) * 40);
        // A live cache only shrinks under pressure. Re-growing waits for the next history reset.
        uint32_t desired = m_cacheCapacity != 0 && m_valid ? std::min(m_cacheCapacity, planned) : planned;
        if (desired != m_cacheCapacity)
        {
            DestroyRadianceCache();
            const uint32_t strides[] = {8, 16, 16};
            const char*    names[]   = {"gi_cache_keys", "gi_cache_accumulation", "gi_cache_resolved"};
            bool           allocated = false;
            while (desired != 0 && !allocated)
            {
                allocated = true;
                for (uint32_t index = 0; index < 3 && allocated; ++index)
                {
                    m_cacheBuffers[index] = m_device->CreateStorageBuffer(desired * strides[index], nullptr, names[index]);
                    allocated             = m_cacheBuffers[index] != nullptr;
                }
                if (!allocated)
                {
                    DestroyRadianceCache();
                    desired /= 2;
                }
            }
            m_cacheCapacity = desired;
        }
    }
    // Log the chosen size, zero included, when it is first requested or changes; not on every re-plan.
    if (requested && (requested != m_cacheRequested || m_cacheCapacity != previousCapacity))
    {
        LOGI("GI radiance cache: {} entries, {} bytes, {}-bit keys, enabled={}", m_cacheCapacity,
             uint64_t(m_cacheCapacity) * 40, m_device->GetGPUInfo().supportBufferInt64Atomics ? 64 : 32, enabled);
    }
    m_cacheRequested = requested;
    if (m_cacheCapacity != 0)
    {
        RenderGraph*   graph            = m_device->GetCurrentFrameRDG();
        const uint32_t groups           = (m_cacheCapacity + 255u) / 256u;
        const uint32_t bits             = m_device->GetGPUInfo().supportBufferInt64Atomics ? 64 : 32;
        m_cacheNeedsClear              |= !m_valid;
        m_cacheUniforms.camera          = m_recordedViewPosition;
        const float sceneScale          = m_scene->GetVoxelSceneBounds().GetMaxExtent();
        m_cacheUniforms.grid.x          = std::max(sceneScale * 0.0002f, 1e-7f);
        m_cacheUniforms.grid.y          = std::max(sceneScale * 0.02f, 1e-5f);
        m_cacheUniforms.previousCamera  = m_uniforms.previousViewPosition;
        m_cacheUniforms.limits          = glm::uvec4(m_cacheCapacity, std::min(voxelGI.GetSettings().historyFrames, 255u), 120u,
                                                     m_uniforms.bounce.z != 0u ? 1u : 0u);
        m_cacheUniforms.grid.z          = m_recordedViewPosition != m_uniforms.previousViewPosition ? 1.0f : 0.0f;
        if (m_cacheNeedsClear)
        {
            m_cacheUniforms.grid.w = 1.0f;
            RDGComputePassDesc clear;
            clear.SetShaderProgramName(NameID(fmt::format("GICacheResolve{}SP", bits)));
            clear.SetPassTag("InitializeGICache");
            BindRadianceCache(clear);
            graph->AddComputePass(std::move(clear)).RecordPassCommands([groups](RDGPassCmdEncoder& encoder) {
                encoder.Dispatch(groups, 1, 1);
            });
            m_cacheUniforms.grid.w = 0.0f;
        }
        RDGRayTracingPassDesc update;
        update.SetShaderProgramName(NameID(fmt::format("GICacheUpdate{}{}SP", bits, noGuide ? "NoGuide" : "")));
        update.SetPassTag("GICacheUpdate");
        BindGuides(update);
        voxelGI.BindHardwareRayInputs(update, false);
        voxelGI.BindLightingInputs(update, true);
        voxelGI.BindEnvironmentSamplingInputs(update);
        update.BindValue("uSceneData", m_scene->GetSceneUniformData(), sizeof(SceneUniformData));
        BindRadianceCache(update);
        SetHybridRayTracingGroups(update, 2);
        const glm::uvec2 extent = (m_extent + glm::uvec2(3)) / 4u;
        graph->AddRayTracingPass(std::move(update)).RecordPassCommands([extent](RDGPassCmdEncoder& encoder) {
            encoder.TraceRays(extent.x, extent.y, 1);
        });
        RDGComputePassDesc resolve;
        resolve.SetShaderProgramName(NameID(fmt::format("GICacheResolve{}SP", bits)));
        resolve.SetPassTag("GICacheResolve");
        BindRadianceCache(resolve);
        graph->AddComputePass(std::move(resolve)).RecordPassCommands([groups](RDGPassCmdEncoder& encoder) {
            encoder.Dispatch(groups, 1, 1);
        });
    }
}

void HybridGIRenderer::RecordNativeProbeBenchmark(VoxelGIRenderer& voxelGI)
{
    m_nativeProbeRaySlots = 0;
    if (voxelGI.GetSettings().nativeProbeBenchmark && m_cacheCapacity != 0 && voxelGI.UsesRayTracingPipeline())
    {
        RenderGraph*     graph      = m_device->GetCurrentFrameRDG();
        const glm::uvec2 tiles      = (m_outputExtent + glm::uvec2(15)) / 16u;
        const uint32_t   baseProbes = tiles.x * tiles.y;
        const uint32_t   probes     = baseProbes + (baseProbes + 3u) / 4u;
        RDGBufferDesc    buffer;
        buffer.name = "native_probe_benchmark_rays";
        buffer.size = uint64_t(probes) * 64u * sizeof(Vec4);
        buffer.usageFlags.SetFlag(RHIBufferUsageFlagBits::eStorageBuffer);
        const RDGBuffer       rays = graph->GetResourceManager()->CreateBuffer(buffer);
        RDGRayTracingPassDesc trace;
        trace.SetShaderProgramName(
            NameID(fmt::format("NativeProbeBenchmark{}SP", m_device->GetGPUInfo().supportBufferInt64Atomics ? 64 : 32)));
        trace.SetPassTag("NativeProbeBenchmark");
        BindHybridReceiverGuides(trace, m_sampler, m_uniforms);
        voxelGI.BindHardwareRayInputs(trace, false);
        voxelGI.BindLightingInputs(trace, true);
        voxelGI.BindEnvironmentSamplingInputs(trace);
        trace.BindValue("uSceneData", m_scene->GetSceneUniformData(), sizeof(SceneUniformData));
        BindRadianceCache(trace);
        trace.BindStorageBuffer("NativeProbeRays", rays, RDGContentGuarantee::eFullWrite);
        HeapVector<RHIRayTracingShaderGroup> groups = {{RHIRayTracingShaderGroupType::eRayGeneration, 0},
                                                       {RHIRayTracingShaderGroupType::eMiss, 1},
                                                       {RHIRayTracingShaderGroupType::eHit, kUnusedRayTracingShader, 2, 3}};
        trace.SetShaderGroups(std::move(groups), 1);
        graph->AddRayTracingPass(std::move(trace)).RecordPassCommands([probes](RDGPassCmdEncoder& encoder) {
            encoder.TraceRays(64, probes, 1);
        });
        m_nativeProbeRaySlots = probes * 64u;
    }
}

void HybridGIRenderer::Dispatch(RDGComputePassDesc&& pass, bool fullResolution)
{
    const glm::uvec2 groups = ((fullResolution ? m_outputExtent : m_extent) + glm::uvec2(7)) / 8u;
    m_device->GetCurrentFrameRDG()->AddComputePass(std::move(pass)).RecordPassCommands([groups](RDGPassCmdEncoder& encoder) {
        encoder.Dispatch(groups.x, groups.y, 1);
    });
}

void HybridGIRenderer::RecordReSTIRResample(VoxelGIRenderer&         voxelGI,
                                            const ReSTIRUniformData& restirData,
                                            uint32_t                 stage,
                                            uint32_t                 signal,
                                            RDGBuffer                receiverGuides,
                                            RDGTexture               rawBounce,
                                            RDGTexture               rawSpecular,
                                            RDGTexture               rawDiffuseDistance,
                                            RDGTexture               rawReflection)
{
    RenderGraph*          graph    = m_device->GetCurrentFrameRDG();
    const bool            pipeline = voxelGI.UsesRayTracingPipeline();
    const char* const     names[]  = {"Diffuse", "Reflection", ""};
    RDGComputePassDesc    compute;
    RDGRayTracingPassDesc rays;
    RDGDispatchPassDesc&  resample =
        pipeline ? static_cast<RDGDispatchPassDesc&>(rays) : static_cast<RDGDispatchPassDesc&>(compute);
    resample.SetShaderProgramName(
        NameID(fmt::format("ReSTIR{}{}{}SP", signal == 2 ? "Temporal" : "Spatial", names[signal], pipeline ? "Pipeline" : "")));
    resample.SetPassTag(signal == 2 ? NameID("ReSTIRTemporal")
                                    : NameID(fmt::format("ReSTIRSpatial{}{}", names[signal], stage - 1)));
    BindGuides(resample);
    voxelGI.BindHardwareRayInputs(resample, false);
    resample.BindValue("uSceneData", m_scene->GetSceneUniformData(), sizeof(SceneUniformData));
    ReSTIRUniformData parameters = restirData;
    parameters.options.w         = stage;
    resample.BindValue("uReSTIRData", parameters);
    resample.BindSampledTexture("restirLobeIntegral", m_sampler, m_restirLobeIntegral->GetDefaultView());
    // Temporal writes every texel's packed receiver; spatial reuse reads its neighbors'.
    resample.BindStorageBuffer("GIReceiverGuides", receiverGuides,
                               signal == 2 ? RDGContentGuarantee::eFullWrite : RDGContentGuarantee::eNone);
    for (uint32_t type = 0; type < 2; ++type)
    {
        const char* input         = type == 0 ? "GIReservoirCurrent" : "ReflectionReservoirCurrent";
        const char* output        = type == 0 ? "GIReservoirOutput" : "ReflectionReservoirOutput";
        const char* previousInput = type == 0 ? "GIReservoirPrevious" : "ReflectionReservoirPrevious";
        resample.BindStorageBuffer(input, m_reservoir[m_current][type]);
        resample.BindStorageBuffer(output, m_reservoir[m_current][type]);
        resample.BindStorageBuffer(previousInput, m_reservoir[1 - m_current][type]);
    }
    resample.BindSampledTexture("previousPosition", m_sampler, m_history[1 - m_current][Position]->GetDefaultView());
    resample.BindSampledTexture("previousNormal", m_sampler, m_history[1 - m_current][Normal]->GetDefaultView());
    resample.BindSampledTexture("previousReceiver", m_sampler, m_history[1 - m_current][Receiver]->GetDefaultView());
    resample.BindSampledTexture("previousLength", m_sampler, m_history[1 - m_current][Length]->GetDefaultView());
    resample.BindStorageImage("rawBounce", rawBounce);
    resample.BindStorageImage("rawSpecular", rawSpecular);
    resample.BindStorageImage("rawDiffuseDistance", rawDiffuseDistance);
    resample.BindStorageImage("rawReflection", rawReflection);
    if (pipeline)
    {
        rays.SetShaderGroups({{RHIRayTracingShaderGroupType::eRayGeneration, 0},
                              {RHIRayTracingShaderGroupType::eMiss, 1},
                              {RHIRayTracingShaderGroupType::eHit, kUnusedRayTracingShader, kUnusedRayTracingShader, 2}});
        const glm::uvec2 extent = m_extent;
        graph->AddRayTracingPass(std::move(rays)).RecordPassCommands([extent](RDGPassCmdEncoder& encoder) {
            encoder.TraceRays(extent.x, extent.y, 1);
        });
    }
    else
    {
        Dispatch(std::move(compute));
    }
}

void HybridGIRenderer::PrepareGuides()
{
    if (m_extent != m_outputExtent)
    {
        RDGResourceManager* resources = m_device->GetCurrentFrameRDG()->GetResourceManager();
        const char*         names[]   = {"hybrid_receiver_depth",   "hybrid_receiver_normal", "hybrid_receiver_roughness",
                                         "hybrid_receiver_surface", "hybrid_receiver_motion", "hybrid_receiver_position"};
        const char*         outputs[] = {"reducedDepth",   "reducedNormal", "reducedRoughness",
                                         "reducedSurface", "reducedMotion", "reducedPosition"};
        const DataFormat    formats[] = {DataFormat::eR32SFloat,  DataFormat::eR16G16UNORM,  DataFormat::eR8G8B8A8UNORM,
                                         DataFormat::eR32G32UInt, DataFormat::eR32G32SFloat, DataFormat::eR32G32B32A32SFloat};
        RDGComputePassDesc  pass;
        pass.SetShaderProgramName(m_recordedSettings.footprintReceivers ? "HybridDownsampleFootprintSP" : "HybridDownsampleSP");
        pass.SetPassTag("HybridDownsample");
        BindHybridReceiverGuides(pass, m_sampler, m_uniforms);
        for (uint32_t index = 0; index < 6; ++index)
        {
            RDGTextureDesc desc;
            desc.name                = names[index];
            desc.texFormat.dimension = TextureDimension::e2D;
            desc.texFormat.width     = m_extent.x;
            desc.texFormat.height    = m_extent.y;
            desc.texFormat.depth     = 1;
            desc.texFormat.format    = formats[index];
            desc.usageFlags.SetFlags(RHITextureUsageFlagBits::eStorage, RHITextureUsageFlagBits::eSampled);
            m_receiverGuides[index] = resources->CreateTexture(desc);
            pass.BindStorageImage(outputs[index], m_receiverGuides[index], RDGContentGuarantee::eFullWrite);
        }
        Dispatch(std::move(pass));
    }
}

void HybridGIRenderer::RecordDenoiser(RDGTexture rawSky,
                                      RDGTexture rawBounce,
                                      RDGTexture rawSpecular,
                                      RDGTexture rawReflection,
                                      RDGTexture rawDiffuseDistance)
{
    RenderGraph*        graph     = m_device->GetCurrentFrameRDG();
    RDGResourceManager* resources = graph->GetResourceManager();
    RDGTexture          pyramids[2];
    const bool          skyLight =
        HasSkyLight(m_recordedSettings, *reinterpret_cast<const SceneUniformData*>(m_scene->GetSceneUniformData()));
    const char*        skyVariant = skyLight ? "" : "NoSky";
    RDGComputePassDesc preblur;
    preblur.SetShaderProgramName(skyLight ? "GIDenoisePreblurSP" : "GIDenoisePreblurNoSkySP");
    preblur.SetPassTag("GIDenoisePreblur");
    BindGuides(preblur);
    preblur.BindValue("uSceneData", m_scene->GetSceneUniformData(), sizeof(SceneUniformData));
    preblur.BindSampledTexture("rawSky", m_sampler, rawSky);
    preblur.BindSampledTexture("rawBounce", m_sampler, rawBounce);
    preblur.BindSampledTexture("rawSpecular", m_sampler, rawSpecular);
    preblur.BindSampledTexture("rawReflection", m_sampler, rawReflection);
    preblur.BindSampledTexture("rawDiffuseDistance", m_sampler, rawDiffuseDistance);
    preblur.BindSampledTexture("previousLength", m_sampler, m_history[1 - m_current][Length]->GetDefaultView());
    for (uint32_t texture : {Position, Normal, Receiver, Reflection})
    {
        preblur.BindSampledTexture(NameID(fmt::format("previous{}", kHistoryNames[texture])), m_sampler,
                                   m_history[1 - m_current][texture]->GetDefaultView());
    }
    for (uint32_t kind = 0; kind < 2; ++kind)
    {
        const uint32_t firstLevel = kind;
        RDGTextureDesc pyramid;
        pyramid.name                = kind == 0 ? "gi_signal_pyramid" : "gi_guide_pyramid";
        pyramid.texFormat.dimension = TextureDimension::e2D;
        // Padding keeps every 8x8 group and its lower mip texels exclusively owned by that group.
        // Only the signal pyramid needs level zero; history fix starts reading guides at level one.
        pyramid.texFormat.width       = ((m_extent.x + 7u) / 8u * 8u) >> firstLevel;
        pyramid.texFormat.height      = ((m_extent.y + 7u) / 8u * 8u) >> firstLevel;
        pyramid.texFormat.depth       = 1;
        pyramid.texFormat.arrayLayers = kind == 0 ? 3u : 2u;
        pyramid.texFormat.mipmaps     = 4 - firstLevel;
        pyramid.texFormat.format      = kind == 0 ? DataFormat::eR16G16B16A16SFloat : DataFormat::eR32G32B32A32UInt;
        pyramid.usageFlags.SetFlags(RHITextureUsageFlagBits::eStorage, RHITextureUsageFlagBits::eSampled);
        pyramids[kind] = resources->CreateTexture(pyramid);
        for (uint32_t mip = 0; mip < pyramid.texFormat.mipmaps; ++mip)
        {
            RDGTextureViewDesc         view;
            RHITextureSubResourceRange range;
            range.aspect.SetFlag(RHITextureAspectFlagBits::eColor);
            range.baseMipLevel = mip;
            range.layerCount   = pyramid.texFormat.arrayLayers;
            view.range         = range;
            preblur.BindStorageImage(NameID(fmt::format("{}Mip{}", kind == 0 ? "signal" : "guide", mip + firstLevel)),
                                     pyramids[kind], view, RDGContentGuarantee::eFullWrite);
        }
    }
    Dispatch(std::move(preblur));

    const bool validation = m_recordedSettings.denoiserValidation;
    if (validation)
    {
        for (uint32_t index = 0; index < 2; ++index)
        {
            RDGTextureDesc desc;
            desc.name                  = index == 0 ? "gi_temporal_validation" : "gi_repair_validation";
            desc.texFormat.dimension   = TextureDimension::e2D;
            desc.texFormat.width       = m_extent.x;
            desc.texFormat.height      = m_extent.y;
            desc.texFormat.depth       = 1;
            desc.texFormat.arrayLayers = index == 0 ? 3u : 2u;
            desc.texFormat.format      = DataFormat::eR32G32B32A32SFloat;
            desc.usageFlags.SetFlags(RHITextureUsageFlagBits::eStorage, RHITextureUsageFlagBits::eSampled);
            m_denoiserValidation[index] = resources->CreateTexture(desc);
        }
    }
    RDGComputePassDesc temporal;
    temporal.SetShaderProgramName(NameID(fmt::format("GIDenoiseTemporal{}{}{}{}SP", validation ? "Validation" : "",
                                                     m_recordedSettings.dataDrivenAntiLag ? "AntiLag" : "",
                                                     m_recordedSettings.bicubicHistory ? "Bicubic" : "", skyVariant)));
    // The world-cache seed exists for the default anti-lag, bicubic denoiser without validation images.
    if (m_recordedSettings.worldCacheSeed && m_recordedSettings.worldRadianceCache && m_worldCache.IsRecorded()
        && m_recordedSettings.dataDrivenAntiLag && m_recordedSettings.bicubicHistory && !validation)
    {
        temporal.SetShaderProgramName(
            NameID(fmt::format("GIDenoiseTemporalAntiLagBicubicSeed{}{}SP",
                               m_device->GetGPUInfo().supportBufferInt64Atomics ? 64 : 32, skyVariant)));
        m_worldCache.Bind(temporal);
    }
    if (validation)
    {
        temporal.BindStorageImage("temporalValidation", m_denoiserValidation[0], RDGContentGuarantee::eFullWrite);
    }
    if (m_recordedSettings.dataDrivenAntiLag)
    {
        temporal.BindSampledTexture("previousResponseAge", m_sampler, m_history[1 - m_current][ResponseAge]->GetDefaultView());
    }
    temporal.SetPassTag("GIDenoiseTemporal");
    temporal.allowCulling = false;
    BindGuides(temporal);
    temporal.BindValue("uSceneData", m_scene->GetSceneUniformData(), sizeof(SceneUniformData));
    temporal.BindSampledTexture("preblurSignals", m_sampler, pyramids[0]);
    temporal.BindSampledTexture("rawReflection", m_sampler, rawReflection);
    const uint32_t   signalHistory[] = {Sky, Specular, Bounce};
    RDGTexture*      targets[]       = {&m_outputSky, &m_outputSpecular, &m_outputBounce};
    const char*      signals[]       = {"Sky", "Specular", "Bounce"};
    const glm::uvec2 skyExtent(m_history[m_current][Sky]->GetWidth(), m_history[m_current][Sky]->GetHeight());
    for (uint32_t signal = 0; signal < 3; ++signal)
    {
        *targets[signal] = DenoiseSignalTexture(*resources, NameID(fmt::format("gi_temporal_{}", signals[signal])),
                                                signal == 0 ? skyExtent : m_extent, kHistoryFormats[signalHistory[signal]]);
        temporal.BindStorageImage(NameID(fmt::format("next{}", signals[signal])), *targets[signal],
                                  RDGContentGuarantee::eFullWrite);
    }
    for (uint32_t texture :
         {Sky, Bounce, Specular, FastSky, FastBounce, FastSpecular, Length, Moments, Reflection, Position, Normal, Receiver})
    {
        temporal.BindSampledTexture(NameID(fmt::format("previous{}", kHistoryNames[texture])), m_sampler,
                                    m_history[1 - m_current][texture]->GetDefaultView());
        if (texture != Sky && texture != Specular && texture != Bounce)
        {
            temporal.BindStorageImage(NameID(fmt::format("next{}", kHistoryNames[texture])),
                                      m_history[m_current][texture]->GetDefaultView(), RDGContentGuarantee::eFullWrite);
        }
    }
    Dispatch(std::move(temporal));

    // History fix, then three spatial iterations unless the filter is off. Their inputs and outputs
    // have the same signal layout.
    const uint32_t stages = m_recordedSettings.filter ? 4u : 1u;
    for (uint32_t stage = 0; stage < stages; ++stage)
    {
        RDGComputePassDesc pass;
        const NameID tag = stage == 0 ? NameID("GIDenoiseHistoryFix") : NameID(fmt::format("GIDenoiseAtrous{}", stage - 1));
        pass.SetShaderProgramName(
            NameID(fmt::format("{}{}{}{}{}SP", tag.CStr(), stage == 0 && validation ? "Validation" : "",
                               stage == 0 && m_recordedSettings.dataDrivenAntiLag ? "AntiLag" : "",
                               stage == 0 && m_recordedSettings.antiFirefly ? "AntiFirefly" : "", skyVariant)));
        pass.SetPassTag(tag);
        BindGuides(pass);
        pass.BindValue("uSceneData", m_scene->GetSceneUniformData(), sizeof(SceneUniformData));
        pass.BindSampledTexture("historyLength", m_sampler, m_history[m_current][Length]->GetDefaultView());
        pass.BindSampledTexture("reflectionHistory", m_sampler, m_history[m_current][Reflection]->GetDefaultView());
        if (stage == 0)
        {
            pass.allowCulling = false;
            if (m_recordedSettings.dataDrivenAntiLag)
            {
                pass.BindStorageImage("outputResponseAge", m_history[m_current][ResponseAge]->GetDefaultView(),
                                      RDGContentGuarantee::eFullWrite);
            }
            if (validation)
            {
                pass.BindStorageImage("repairValidation", m_denoiserValidation[1], RDGContentGuarantee::eFullWrite);
            }
            pass.BindSampledTexture("preblurSignals", m_sampler, pyramids[0]);
            pass.BindSampledTexture("preblurGuides", m_sampler, pyramids[1]);
            for (uint32_t texture : {FastSky, FastBounce, FastSpecular})
            {
                pass.BindSampledTexture(NameID(fmt::format("input{}", kHistoryNames[texture])), m_sampler,
                                        m_history[m_current][texture]->GetDefaultView());
            }
        }
        else
        {
            pass.BindSampledTexture("fastMoments", m_sampler, m_history[m_current][Moments]->GetDefaultView());
        }
        for (uint32_t signal = 0; signal < 3; ++signal)
        {
            pass.BindSampledTexture(NameID(fmt::format("input{}", signals[signal])), m_sampler, *targets[signal]);
            // Clamp/repair is the slow history consumed next frame. Spatial filtering stays out of that feedback.
            *targets[signal] =
                stage == 0 ? resources->ImportTexture(m_history[m_current][signalHistory[signal]])
                           : DenoiseSignalTexture(*resources, NameID(fmt::format("gi_denoise_{}_{}", stage, signals[signal])),
                                                  signal == 0 ? skyExtent : m_extent, DataFormat::eR16G16B16A16SFloat);
            pass.BindStorageImage(NameID(fmt::format("output{}", signals[signal])), *targets[signal],
                                  RDGContentGuarantee::eFullWrite);
            if (stage == 0)
            {
                m_historyFixed[signal] = *targets[signal];
            }
        }
        Dispatch(std::move(pass));
    }
}

void HybridGIRenderer::ReconstructOutput(VoxelGIRenderer& voxelGI)
{
    m_missingReceiver = {};
    if (m_extent != m_outputExtent)
    {
        const bool skyLight =
            HasSkyLight(voxelGI.GetSettings(), *reinterpret_cast<const SceneUniformData*>(m_scene->GetSceneUniformData()));
        RDGResourceManager* resources = m_device->GetCurrentFrameRDG()->GetResourceManager();
        RDGComputePassDesc  pass;
        // Reduced resolution always runs the denoiser: reference diagnostics trace every pixel.
        pass.SetShaderProgramName(skyLight ? "HybridReconstructSP" : "HybridReconstructNoSkySP");
        pass.SetPassTag("HybridReconstruct");
        BindHybridReceiverGuides(pass, m_sampler, m_uniforms);
        pass.BindValue("uSceneData", m_scene->GetSceneUniformData(), sizeof(SceneUniformData));
        pass.BindSampledTexture("reducedPosition", m_sampler, m_receiverGuides[5]);
        pass.BindSampledTexture("reducedNormal", m_sampler, m_receiverGuides[1]);
        pass.BindSampledTexture("reducedSurface", m_sampler, m_receiverGuides[3]);
        pass.BindSampledTexture("reducedRoughness", m_sampler, m_receiverGuides[2]);
        RDGTexture* targets[] = {&m_outputSky, &m_outputSpecular, &m_outputBounce};
        const char* inputs[]  = {"inputSky", "inputSpecular", "inputBounce"};
        const char* outputs[] = {"outputSky", "outputSpecular", "outputBounce"};
        const char* names[]   = {"hybrid_reconstructed_sky", "hybrid_reconstructed_specular", "hybrid_reconstructed_bounce"};
        for (uint32_t index = 0; index < 3; ++index)
        {
            pass.BindSampledTexture(inputs[index], m_sampler, *targets[index]);
            const char* fixedInputs[] = {"fixedSky", "fixedSpecular", "fixedBounce"};
            pass.BindSampledTexture(fixedInputs[index], m_sampler, m_historyFixed[index]);
            const bool     enabled = index == 0 ? skyLight : (index != 2 || m_uniforms.bounce.x != 0);
            RDGTextureDesc desc;
            desc.name                = names[index];
            desc.texFormat.dimension = TextureDimension::e2D;
            desc.texFormat.width     = enabled ? m_outputExtent.x : 1u;
            desc.texFormat.height    = enabled ? m_outputExtent.y : 1u;
            desc.texFormat.depth     = 1;
            desc.texFormat.format    = DataFormat::eR16G16B16A16SFloat;
            desc.usageFlags.SetFlags(RHITextureUsageFlagBits::eStorage, RHITextureUsageFlagBits::eSampled);
            *targets[index] = resources->CreateTexture(desc);
            pass.BindStorageImage(outputs[index], *targets[index], RDGContentGuarantee::eFullWrite);
        }
        RDGTextureDesc missingDesc;
        missingDesc.name                = "hybrid_missing_receiver";
        missingDesc.texFormat.dimension = TextureDimension::e2D;
        missingDesc.texFormat.width     = m_outputExtent.x;
        missingDesc.texFormat.height    = m_outputExtent.y;
        missingDesc.texFormat.depth     = 1;
        missingDesc.texFormat.format    = DataFormat::eR8UInt;
        missingDesc.usageFlags.SetFlags(RHITextureUsageFlagBits::eStorage, RHITextureUsageFlagBits::eSampled);
        const RDGTexture missing = resources->CreateTexture(missingDesc);
        m_missingReceiver        = missing;
        pass.BindStorageImage("missingReceiver", missing, RDGContentGuarantee::eFullWrite);
        Dispatch(std::move(pass), true);
    }
}

bool HybridGIRenderer::HasSkyLight(const VoxelGISettings& settings, const SceneUniformData& scene)
{
    return settings.environmentLighting && scene.environment.x > 0.0f && scene.environment.z > 0.0f;
}

bool HybridGIRenderer::PrepareOutputHistory(bool enabled, bool skyLight)
{
    const DataFormat formats[] = {DataFormat::eR16G16B16A16SFloat,
                                  DataFormat::eR16G16B16A16SFloat,
                                  DataFormat::eR16G16B16A16SFloat,
                                  DataFormat::eR32G32B32A32SFloat,
                                  DataFormat::eR16G16UNORM,
                                  DataFormat::eR32G32UInt,
                                  DataFormat::eR16UInt};
    bool             valid     = true;
    m_stabilizeOutput          = enabled;
    for (uint32_t side = 0; side < 2 && valid; ++side)
    {
        for (uint32_t index = 0; index < OutputCount && valid; ++index)
        {
            RHITexture*&     texture = m_outputHistory[side][index];
            const glm::uvec2 extent  = index == OutputSky && !skyLight ? glm::uvec2(1) : m_outputExtent;
            if (texture != nullptr && (!enabled || texture->GetWidth() != extent.x || texture->GetHeight() != extent.y))
            {
                m_device->DestroyTexture(texture);
                texture              = nullptr;
                m_outputHistoryValid = false;
            }
            if (enabled && texture == nullptr)
            {
                TextureFormat format;
                format.dimension = TextureDimension::e2D;
                format.depth     = 1;
                format.width     = extent.x;
                format.height    = extent.y;
                format.format    = formats[index];
                texture =
                    m_device->CreateTextureStorage(format, {}, NameID(fmt::format("gi_output_history_{}_{}", side, index)));
                valid                = texture != nullptr;
                m_outputHistoryValid = false;
            }
        }
    }
    if (!valid)
    {
        LOGE("GI output history allocation failed");
        Destroy();
    }
    return valid;
}

void HybridGIRenderer::StabilizeOutput()
{
    if (m_stabilizeOutput)
    {
        const bool skyLight =
            HasSkyLight(m_recordedSettings, *reinterpret_cast<const SceneUniformData*>(m_scene->GetSceneUniformData()));
        RDGComputePassDesc pass;
        pass.SetShaderProgramName(skyLight ? "GIDenoiseStabilizeSP" : "GIDenoiseStabilizeNoSkySP");
        pass.SetPassTag("GIDenoiseStabilize");
        HybridGIUniformData uniforms = m_uniforms;
        uniforms.sampling.w          = uniforms.sampling.w != 0 && m_outputHistoryValid ? 1u : 0u;
        BindHybridReceiverGuides(pass, m_sampler, uniforms);
        pass.BindValue("uSceneData", m_scene->GetSceneUniformData(), sizeof(SceneUniformData));
        const char* const inputs[]   = {"inputSky", "inputSpecular", "inputBounce"};
        const char* const previous[] = {"previousSky",    "previousSpecular", "previousBounce", "previousPosition",
                                        "previousNormal", "previousSurface",  "previousAge"};
        const char* const outputs[]  = {"outputSky",    "outputSpecular", "outputBounce", "outputPosition",
                                        "outputNormal", "outputSurface",  "outputAge"};
        RDGTexture*       targets[]  = {&m_outputSky, &m_outputSpecular, &m_outputBounce};
        for (uint32_t index = 0; index < 3; ++index)
        {
            pass.BindSampledTexture(inputs[index], m_sampler, *targets[index]);
            if (!m_outputHistoryValid)
            {
                // Descriptors must name initialized resources even when the history branch is disabled.
                pass.BindSampledTexture(previous[index], m_sampler, *targets[index]);
            }
        }
        if (!m_outputHistoryValid)
        {
            pass.BindSampledTexture("previousPosition", m_sampler, "offscreen_position");
            pass.BindSampledTexture("previousNormal", m_sampler, "offscreen_normal");
            pass.BindSampledTexture("previousSurface", m_sampler, "offscreen_receiver");
            pass.BindSampledTexture("previousAge", m_sampler, "offscreen_receiver");
        }
        RDGResourceManager* resources = m_device->GetCurrentFrameRDG()->GetResourceManager();
        for (uint32_t index = 0; index < OutputCount; ++index)
        {
            if (m_outputHistoryValid)
            {
                pass.BindSampledTexture(previous[index], m_sampler, m_outputHistory[1 - m_current][index]->GetDefaultView());
            }
            RHITexture* texture = m_outputHistory[m_current][index];
            pass.BindStorageImage(outputs[index], texture->GetDefaultView(), RDGContentGuarantee::eFullWrite);
            if (index < 3)
            {
                *targets[index] = resources->ImportTexture(texture);
            }
        }
        Dispatch(std::move(pass), true);
    }
}

bool HybridGIRenderer::BuildRenderGraph(const RenderView& view, VoxelGIRenderer& voxelGI)
{
    const VoxelGISettings& settings       = voxelGI.GetSettings();
    const bool             reflections    = voxelGI.UsesReflections();
    const uint32_t         hardwareMask   = voxelGI.GetHardwareFeatureMask();
    const bool             field          = voxelGI.UsesFieldTraversal();
    const uint64_t         visibility     = voxelGI.GetVisibilityGeneration();
    const bool             countRequested = m_rayCountOutput != nullptr || m_rayCountReadback != nullptr;
    const uint64_t         countBytes     = GetRayCountCaptureBytes(view.width, view.height);
    const bool             countValid =
        !countRequested
        || (m_rayCountOutput != nullptr && m_rayCountReadback != nullptr && m_rayCountOutput->GetRequiredSize() == countBytes
            && m_rayCountReadback->GetRequiredSize() == countBytes);
    m_rayCountsRecorded     = false;
    m_captureRecorded       = false;
    m_denoiserValidation[0] = {};
    m_denoiserValidation[1] = {};
    if (!countValid)
    {
        LOGE("Hybrid ray-count capture requires two correctly sized buffers");
    }
    const bool       reference      = settings.referenceSamples != 0;
    const bool       probeRequested = settings.screenProbeGI && !reference && !countRequested
                                   && (voxelGI.UsesRayTracingPipeline() || voxelGI.UsesFieldTraversal());
    const glm::uvec2 outputExtent(view.width, view.height);
    // World-space transport survives camera cuts and screen-history resets.
    // Reset it when its environment or traced representation changes.
    const bool worldReset     = m_environment != m_scene->GetEnvironmentRevision()
                             || settings.environmentLighting != m_previousSettings.environmentLighting
                             || settings.worldCacheFeedback != m_previousSettings.worldCacheFeedback
                             || settings.rayProvider != m_previousSettings.rayProvider
                             || settings.rayTracingPipeline != m_previousSettings.rayTracingPipeline
                             || settings.normalBiasVoxels != m_previousSettings.normalBiasVoxels
                             || settings.fieldResolution != m_previousSettings.fieldResolution
                             || m_previousHardwareMask != hardwareMask || m_previousVisibility != visibility;
    const bool cacheRequested = !reference && (settings.worldRadianceCache || probeRequested);
    const bool cacheReady =
        countValid && m_prepared && cacheRequested && m_worldCache.Prepare(voxelGI, *m_scene, outputExtent, worldReset);
    if (!cacheReady)
    {
        m_worldCache.Destroy();
    }
    const bool probes = probeRequested && cacheReady
                     && m_screenProbes.Prepare(outputExtent, settings.specularOcclusion, voxelGI.UsesRayTracingPipeline());
    if (!probes && m_probePath)
    {
        m_screenProbes.Destroy();
    }
    // The GI denoiser runs on every frame except the reference diagnostic's plain mean.
    const bool     relax           = !reference;
    const bool     restirRequested = !probes && settings.restirGI && !reference && !countRequested;
    const uint32_t restirMask =
        restirRequested
            ? (((hardwareMask & 8u) != 0 ? 1u : 0u)
               | (settings.restirSpecular && reflections && settings.specularOcclusion && (hardwareMask & 16u) != 0 ? 2u : 0u))
            : 0u;
    const bool     restir = restirMask != 0;
    const uint32_t divisor =
        reference || countRequested || probes
            ? 1u
            : ResolvePixelResolutionDivisor(probeRequested ? 0u : settings.pixelResolutionDivisor, view.height);
    const glm::uvec2 extent = (outputExtent + glm::uvec2(divisor - 1u)) / divisor;
    if (countValid && m_prepared && (m_extent != extent || m_outputExtent != outputExtent || m_probePath != probes))
    {
        // The receiver pass has already captured this frame's vertices.
        RHIResourcePtr<RHIBuffer> recordedVertices = m_recordedVertices;
        DestroyPixelHistory();
        m_recordedVertices    = std::move(recordedVertices);
        m_extent              = extent;
        m_outputExtent        = outputExtent;
        m_resetReason         = m_probePath != probes ? "gather_method" : "resize";
        m_preparedResetReason = m_resetReason;
    }
    m_probePath         = probes;
    const bool skyLight = HasSkyLight(settings, *reinterpret_cast<const SceneUniformData*>(m_scene->GetSceneUniformData()));
    // The qualified no-environment path has no learned guide and uses 8x8 groups.
    const bool noGuide = !countRequested && !m_deterministicGuideCapture && !skyLight;
    const bool ready   = countValid && m_prepared
                      && (probes
                          || (PrepareHistory(m_extent.x, m_extent.y, reflections, reference, restirMask, relax, skyLight,
                                             !noGuide, settings.dataDrivenAntiLag)
                              && PrepareOutputHistory(relax && settings.temporal && settings.filter, skyLight)));
    m_failureReason    = ready       ? nullptr
                       : !countValid ? "ray-count capture buffers are missing or mis-sized"
                       : !m_prepared ? "the view's receivers were not prepared"
                                     : "GI history allocation failed";
    if (ready)
    {
        const bool hadValidHistory = m_valid;
        m_resetReason              = m_valid ? "none" : m_resetReason;
        if (!probes && m_geometry != m_scene->GetGeometryRevision())
        {
            InvalidateHistory("geometry_or_opacity");
        }
        else if (m_environment != m_scene->GetEnvironmentRevision()
                 || settings.environmentLighting != m_previousSettings.environmentLighting)
        {
            InvalidateHistory("environment");
        }
        else if ((settings.referenceSamples != 0 && m_recordedProjectionView != m_uniforms.previousProjectionView)
                 || glm::dot(Vec3(m_recordedViewDirection), Vec3(m_uniforms.previousViewDirection)) < 0.5f
                 || m_recordedProjection != m_previousProjection
                 || glm::distance(Vec3(m_recordedViewPosition), Vec3(m_uniforms.previousViewPosition)) > 0.25f)
        {
            InvalidateHistory("camera_cut_or_projection");
        }
        else if (settings.dataDrivenAntiLag != m_previousSettings.dataDrivenAntiLag
                 || settings.bicubicHistory != m_previousSettings.bicubicHistory
                 || settings.footprintReceivers != m_previousSettings.footprintReceivers
                 || settings.youngReceiverSamples != m_previousSettings.youngReceiverSamples
                 || settings.radianceCache != m_previousSettings.radianceCache
                 || settings.nativeProbeBenchmark != m_previousSettings.nativeProbeBenchmark
                 || settings.worldRadianceCache != m_previousSettings.worldRadianceCache
                 || settings.worldCacheFeedback != m_previousSettings.worldCacheFeedback
                 || settings.rayTracingPipeline != m_previousSettings.rayTracingPipeline
                 || (relax && reflections != m_previousReflections)
                 || settings.referenceSamples != m_previousSettings.referenceSamples
                 || settings.referenceTileStride != m_previousSettings.referenceTileStride
                 || settings.samplingPhase != m_previousSettings.samplingPhase || settings.samples != m_previousSettings.samples
                 || settings.rayProvider != m_previousSettings.rayProvider
                 || settings.normalBiasVoxels != m_previousSettings.normalBiasVoxels
                 || settings.temporal != m_previousSettings.temporal
                 || settings.historyFrames != m_previousSettings.historyFrames
                 || settings.specularOcclusion != m_previousSettings.specularOcclusion
                 || settings.fieldResolution != m_previousSettings.fieldResolution
                 || settings.restirGI != m_previousSettings.restirGI
                 || settings.restirSpatial != m_previousSettings.restirSpatial
                 || settings.restirSpecular != m_previousSettings.restirSpecular)
        {
            InvalidateHistory("settings");
        }
        // Backend availability and a published field generation can change the traced representation
        // without changing the requested settings. Histories and the visibility guide follow it.
        if (m_previousHardwareMask != hardwareMask || m_previousVisibility != visibility)
        {
            InvalidateHistory("provider_changed");
        }
        m_recordedRadianceGeneration = voxelGI.GetRadianceGeneration();
        // One injection since the last frame keeps bounce history: the trace measures the change between
        // the previous and the new cache along the same rays, and the temporal pass moves the history by
        // it, so a light that moves every frame still accumulates. A new bounce source or intensity, or
        // several injections in between, make the old history incomparable.
        const uint64_t radianceSteps = m_recordedRadianceGeneration - m_radianceGeneration;
        // A failed snapshot allocation binds the current cache as a safe descriptor fallback.
        // It cannot measure a lighting change: restart hit-radiance histories on that frame.
        const bool missingPrevious = radianceSteps != 0 && !voxelGI.HasPreviousRadiance();
        const bool radianceReset =
            settings.indirectIntensity != m_previousSettings.indirectIntensity || radianceSteps > 1 || missingPrevious;
        const bool hardwareBounce     = (hardwareMask & 8u) != 0;
        const bool hardwareReflection = (hardwareMask & 16u) != 0;
        // Traced hit lighting has no previous-frame cache to measure a correlated lighting delta.
        // Restart only those hit histories when lights or secondary-surface materials change.
        const bool tracedLightingChanged = m_lighting != m_scene->GetLightingRevision()
                                        || m_surface != m_scene->GetSurfaceRevision()
                                        || settings.analyticLighting != m_previousSettings.analyticLighting
                                        || settings.emissiveLighting != m_previousSettings.emissiveLighting
                                        || settings.shadows != m_previousSettings.shadows;
        const bool bounceReset           = m_frame == 0 || radianceReset || (hardwareBounce && tracedLightingChanged);
        const bool reflectionReset =
            reflections != m_previousReflections || radianceReset || (hardwareReflection && tracedLightingChanged);
        if (bounceReset || reflectionReset || radianceSteps != 0)
        {
            m_bounceResponsiveFrames = 8;
        }
        m_uniforms.bounce = glm::uvec4(1u, m_valid && !bounceReset ? 1u : 0u, m_bounceResponsiveFrames != 0 ? 1u : 0u,
                                       !relax && radianceSteps == 1 && !missingPrevious && !hardwareBounce ? 1u : 0u);
        m_uniforms.reflection =
            glm::uvec4(reflections ? 1u : 0u, m_valid && !reflectionReset ? 1u : 0u, m_bounceResponsiveFrames != 0 ? 1u : 0u,
                       !relax && radianceSteps == 1 && !missingPrevious && !hardwareReflection ? 1u : 0u);
        const bool hardwareTrace = voxelGI.UsesHardwareQueries();
        // Only geometry-dependent weights survive an unchanged receiver image.
        // Lighting and guide values are always read from this frame's inputs.
        const bool unchangedReceivers = m_valid && m_recordedProjectionView == m_uniforms.previousProjectionView
                                     && m_surface == m_scene->GetSurfaceRevision();
        // w: accepted-frame threshold below which receivers take extra diffuse samples (0 disables).
        const glm::uvec4 reuseData(0u, 0u, 0u, relax && settings.youngReceiverSamples && !countRequested ? 4u : 0u);
        m_recordedSettings   = settings;
        m_recordedVisibility = visibility;
        // Keep the tile and 64-frame proposal schedules aligned while changing the random scramble.
        m_uniforms.sampling =
            glm::uvec4(m_frame + 1024u * settings.samplingPhase, reference ? settings.referenceSamples : settings.samples,
                       settings.referenceSamples != 0 ? 256u : settings.historyFrames, m_valid ? 1 : 0);
        // A guide from a successful frame can survive geometry, opacity and sampling-setting resets.
        // Provider changes use different visibility geometry and must relearn the proposal.
        // A reset reason alone cannot make a fresh or failed-frame guide safe to read.
        const std::string_view reason = m_resetReason;
        const bool             guideValid =
            hadValidHistory && m_previousGuideWritten && (m_valid || reason == "geometry_or_opacity" || reason == "settings");
        m_uniforms.provider   = glm::uvec4(hardwareTrace ? 1u : 0u, guideValid ? 1u : 0u, hardwareMask,
                                           reference ? settings.referenceTileStride : 1u);
        m_uniforms.rejection  = Vec4(ZEN_HYBRID_PLANE_REJECTION, settings.specularOcclusion ? 1.0f : 0.0f,
                                     settings.temporal || settings.referenceSamples != 0 ? 1.0f : 0.0f,
                                     settings.referenceSamples != 0 ? 1.0f : 0.0f);
        m_uniforms.resolution = glm::uvec4(view.width, view.height, divisor, unchangedReceivers ? 1u : 0u);
        m_uniforms.reservoir  = glm::uvec4(restirMask, 4u, 20u, relax ? 1u : 0u);
        PrepareGuides();
        if (cacheReady)
        {
            m_worldCache.Record(voxelGI, *m_scene, m_uniforms, m_sampler);
        }
        if (probes)
        {
            // Only enabled reflections store intensity-scaled hit radiance in the probe history.
            const bool probeReset = !m_valid || reflections != m_previousReflections
                                 || (reflections && settings.indirectIntensity != m_previousSettings.indirectIntensity);
            m_screenProbes.Record(voxelGI, *m_scene, m_worldCache, m_uniforms, m_recordedProjectionView, m_sampler, probeReset,
                                  skyLight);
            m_outputSky            = m_screenProbes.GetSky();
            m_outputBounce         = m_screenProbes.GetBounce();
            m_outputSpecular       = m_screenProbes.GetSpecular();
            m_rawSky               = m_outputSky;
            m_rawBounce            = m_outputBounce;
            m_rawSpecular          = m_outputSpecular;
            m_recordedGuideWritten = false;
            m_recorded             = true;
            if (m_captureOutput != nullptr && m_captureReadback != nullptr)
            {
                RenderGraph&       graph = *m_device->GetCurrentFrameRDG();
                RDGComputePassDesc capture;
                capture.SetShaderProgramName(
                    NameID(fmt::format("ScreenProbeCapture{}SP", m_device->GetGPUInfo().supportBufferInt64Atomics ? 64 : 32)));
                capture.SetPassTag("HybridCapture");
                m_worldCache.Bind(capture);
                BindHybridReceiverGuides(capture, m_sampler, m_uniforms);
                capture.BindSampledTexture("rawSky", m_sampler, m_outputSky);
                capture.BindSampledTexture("rawBounce", m_sampler, m_outputBounce);
                capture.BindSampledTexture("rawSpecular", m_sampler, m_outputSpecular);
                capture.BindSampledTexture("reconstructedSky", m_sampler, m_outputSky);
                capture.BindSampledTexture("reconstructedBounce", m_sampler, m_outputBounce);
                capture.BindSampledTexture("reconstructedSpecular", m_sampler, m_outputSpecular);
                capture.BindStorageBuffer("HybridCapture", m_captureOutput, RDGContentGuarantee::eFullWrite);
                Dispatch(std::move(capture), true);
                graph.AddTransferPass("ReadHybridCapture")
                    .CopyBuffer(m_captureOutput, m_captureReadback, {0, 0, m_captureOutput->GetRequiredSize()})
                    .NeverCull();
                m_captureRecorded = true;
            }
        }
        else
        {
            RecordRadianceCache(voxelGI,
                                (settings.radianceCache || settings.nativeProbeBenchmark) && voxelGI.UsesRayTracingPipeline()
                                    && !reference,
                                noGuide);
            RecordNativeProbeBenchmark(voxelGI);
            RenderGraph*        graph     = m_device->GetCurrentFrameRDG();
            RDGResourceManager* resources = graph->GetResourceManager();
            RDGTextureDesc      desc;
            desc.texFormat.dimension = TextureDimension::e2D;
            desc.texFormat.depth     = 1;
            desc.texFormat.width     = m_extent.x;
            desc.texFormat.height    = m_extent.y;
            desc.texFormat.format    = DataFormat::eR16G16B16A16SFloat;
            desc.usageFlags.SetFlag(RHITextureUsageFlagBits::eStorage);
            desc.usageFlags.SetFlag(RHITextureUsageFlagBits::eSampled);
            desc.name                    = "hybrid_raw_sky";
            const RDGTexture rawSky      = resources->CreateTexture(desc);
            desc.name                    = "hybrid_raw_specular";
            const RDGTexture rawSpecular = resources->CreateTexture(desc);
            RDGTextureDesc   bounceDesc  = desc;
            bounceDesc.name              = "hybrid_raw_bounce";
            const RDGTexture rawBounce   = resources->CreateTexture(bounceDesc);
            // Previous-cache radiance measures a bounce lighting change; only the reference mean uses it.
            RDGTextureDesc previousBounceDesc = bounceDesc;
            if (hardwareBounce || relax)
            {
                previousBounceDesc.texFormat.width = previousBounceDesc.texFormat.height = 1;
            }
            previousBounceDesc.name         = "hybrid_raw_bounce_previous";
            const RDGTexture previous       = resources->CreateTexture(previousBounceDesc);
            RDGTextureDesc   reflectionDesc = desc;
            if (!reflections)
            {
                reflectionDesc.texFormat.width = reflectionDesc.texFormat.height = 1;
            }
            RDGTextureDesc previousReflectionDesc = reflectionDesc;
            if (hardwareReflection || relax)
            {
                previousReflectionDesc.texFormat.width = previousReflectionDesc.texFormat.height = 1;
            }
            previousReflectionDesc.name         = "hybrid_raw_reflection_previous";
            const RDGTexture previousReflection = resources->CreateTexture(previousReflectionDesc);
            reflectionDesc.name                 = "hybrid_raw_reflection";
            reflectionDesc.texFormat.format     = DataFormat::eR32G32SFloat;
            const RDGTexture rawReflection      = resources->CreateTexture(reflectionDesc);
            RDGTextureDesc   distanceDesc       = desc;
            distanceDesc.name                   = "gi_raw_diffuse_distance";
            distanceDesc.texFormat.format       = DataFormat::eR32SFloat;
            if (!relax)
            {
                distanceDesc.texFormat.width = distanceDesc.texFormat.height = 1;
            }
            const RDGTexture rawDiffuseDistance = resources->CreateTexture(distanceDesc);
            m_recordedGuideWritten              = !noGuide;
            const glm::uvec2 guideGroups        = (m_extent + glm::uvec2(kGuideTile - 1)) / kGuideTile;
            const bool       pipelineTrace      = voxelGI.UsesRayTracingPipeline();
            RDGBuffer        guidePooled;
            RDGBuffer        guideLearning;
            if (pipelineTrace && !noGuide)
            {
                RDGBufferDesc buffer;
                buffer.usageFlags.SetFlag(RHIBufferUsageFlagBits::eStorageBuffer);
                buffer.name   = "hybrid_guide_pooled";
                buffer.size   = uint64_t(guideGroups.x) * guideGroups.y * ZEN_HYBRID_GUIDE_POOLED_FLOATS * sizeof(float);
                guidePooled   = resources->CreateBuffer(buffer);
                buffer.name   = "hybrid_guide_learning";
                buffer.size   = uint64_t(guideGroups.x) * guideGroups.y * (ZEN_HYBRID_GUIDE_TILES + 1u)
                              * (m_deterministicGuideCapture ? 10u : 1u) * sizeof(uint32_t);
                guideLearning = resources->CreateBuffer(buffer);
                RDGComputePassDesc prepare;
                prepare.SetShaderProgramName(
                    TraceProgram("HybridGuidePrepare", countRequested, false, m_deterministicGuideCapture));
                prepare.SetPassTag("HybridGuidePrepare");
                prepare.BindValue("uHybridData", m_uniforms);
                prepare.BindValue("uSceneData", m_scene->GetSceneUniformData(), sizeof(SceneUniformData));
                voxelGI.BindLightingInputs(prepare);
                voxelGI.BindEnvironmentSamplingInputs(prepare);
                prepare.BindStorageBuffer("HybridGuidePrevious", m_guide[1 - m_current]);
                prepare.BindStorageBuffer("HybridGuideNext", m_guide[m_current]);
                prepare.BindStorageBuffer("HybridGuidePooled", guidePooled, RDGContentGuarantee::eFullWrite);
                prepare.BindStorageBuffer("HybridGuideLearning", guideLearning, RDGContentGuarantee::eFullWrite);
                if (countRequested)
                {
                    prepare.BindStorageBuffer("HybridRayCounts", m_rayCountOutput, RDGContentGuarantee::eFullWrite);
                }
                graph->AddComputePass(std::move(prepare)).RecordPassCommands([guideGroups](RDGPassCmdEncoder& encoder) {
                    encoder.Dispatch(guideGroups.x, guideGroups.y, 1);
                });
            }
            RDGComputePassDesc    traceCompute;
            RDGRayTracingPassDesc traceRays;
            RDGDispatchPassDesc&  trace =
                pipelineTrace ? static_cast<RDGDispatchPassDesc&>(traceRays) : static_cast<RDGDispatchPassDesc&>(traceCompute);
            ReSTIRUniformData restirData;
            restirData.options = glm::uvec4(settings.restirSpatial ? 1u : 0u, (restirMask & 2u) != 0 ? 1u : 0u,
                                            (restirMask & 1u) != 0 ? 1u : 0u, 0u);
            restirData.spatial = glm::uvec4(std::clamp(settings.restirSpatialSamples, 1u, 4u),
                                            settings.restirSpatialVisibility ? 1u : 0u, 0u, 0u);
            if (restir && !m_restirLobeInitialized)
            {
                RDGComputePassDesc integral;
                integral.SetShaderProgramName("ReSTIRLobeIntegralSP");
                integral.SetPassTag("ReSTIRLobeIntegral");
                integral.BindStorageImage("lobeIntegral", m_restirLobeIntegral->GetDefaultView(),
                                          RDGContentGuarantee::eFullWrite);
                graph->AddComputePass(std::move(integral)).RecordPassCommands([](RDGPassCmdEncoder& encoder) {
                    encoder.Dispatch(16, 16, 1);
                });
            }
            trace.SetShaderProgramName(
                pipelineTrace
                    ? TraceProgram(noGuide ? (restir ? "HybridTraceReSTIRNoGuidePipeline" : "HybridTraceNoGuidePipeline")
                                           : (restir ? "HybridTraceReSTIRPipeline" : "HybridTracePipeline"),
                                   countRequested, false, m_deterministicGuideCapture, reuseData.w != 0u)
                    : TraceProgram(noGuide         ? (restir          ? "HybridTraceReSTIRHardwareNoGuide"
                                                      : hardwareTrace ? "HybridTraceHardwareNoGuide"
                                                                      : "HybridTraceNoGuide")
                                   : restir        ? "HybridTraceReSTIRHardware"
                                   : hardwareTrace ? "HybridTraceHardware"
                                                   : "HybridTrace",
                                   countRequested, field, m_deterministicGuideCapture, reuseData.w != 0u));
            trace.SetPassTag("HybridTrace");
            const bool worldTrace = m_worldCache.IsRecorded() && !countRequested;
            if (worldTrace)
            {
                trace.SetShaderProgramName(NameID(fmt::format("WorldCacheTrace{}{}{}{}{}SP",
                                                              field    ? "Field"
                                                              : restir ? "ReSTIR"
                                                                       : "",
                                                              noGuide ? "NoGuide" : "", reuseData.w != 0u ? "Young" : "",
                                                              m_device->GetGPUInfo().supportBufferInt64Atomics ? 64 : 32,
                                                              m_deterministicGuideCapture ? "Exact" : "")));
                m_worldCache.Bind(trace);
            }
            else if (m_cacheCapacity != 0 && settings.radianceCache)
            {
                trace.SetShaderProgramName(NameID(fmt::format("GICacheTrace{}{}{}{}{}SP",
                                                              countRequested ? "Counts"
                                                              : restir       ? "ReSTIR"
                                                                             : "",
                                                              countRequested ? ""
                                                              : noGuide      ? "NoGuide"
                                                                             : "",
                                                              reuseData.w != 0u ? "Young" : "",
                                                              m_device->GetGPUInfo().supportBufferInt64Atomics ? 64 : 32,
                                                              m_deterministicGuideCapture ? "Exact" : "")));
                BindRadianceCache(trace);
            }
            BindTraceInputs(trace, voxelGI, reuseData, worldTrace && field);
            if (restir)
            {
                trace.BindStorageBuffer("GIReservoirCurrent", m_reservoir[m_current][0], RDGContentGuarantee::eFullWrite);
                trace.BindStorageBuffer("ReflectionReservoirCurrent", m_reservoir[m_current][1],
                                        RDGContentGuarantee::eFullWrite);
                trace.BindValue("uReSTIRData", restirData);
                trace.BindSampledTexture("restirLobeIntegral", m_sampler, m_restirLobeIntegral->GetDefaultView());
            }
            trace.BindStorageImage("rawSpecular", rawSpecular, RDGContentGuarantee::eFullWrite);
            trace.BindStorageImage("rawDiffuseDistance", rawDiffuseDistance, RDGContentGuarantee::eFullWrite);
            trace.BindStorageImage("rawReflection", rawReflection, RDGContentGuarantee::eFullWrite);
            trace.BindStorageImage("rawReflectionPrevious", previousReflection, RDGContentGuarantee::eFullWrite);
            if (pipelineTrace && !noGuide)
            {
                trace.BindStorageBuffer("HybridGuidePooled", guidePooled);
                trace.BindStorageBuffer("HybridGuideLearning", guideLearning);
                trace.BindStorageBuffer("HybridGuideNext", m_guide[m_current]);
            }
            else
            {
                trace.BindStorageBuffer("HybridGuideNext", m_guide[m_current],
                                        noGuide ? RDGContentGuarantee::eNone : RDGContentGuarantee::eFullWrite);
            }
            if (countRequested)
            {
                trace.BindStorageBuffer("HybridRayCounts", m_rayCountOutput,
                                        pipelineTrace ? RDGContentGuarantee::eNone : RDGContentGuarantee::eFullWrite);
            }
            trace.BindStorageImage("rawSky", rawSky, RDGContentGuarantee::eFullWrite);
            trace.BindStorageImage("rawBounce", rawBounce, RDGContentGuarantee::eFullWrite);
            trace.BindStorageImage("rawBouncePrevious", previous, RDGContentGuarantee::eFullWrite);
            if (pipelineTrace)
            {
                SetHybridRayTracingGroups(traceRays, 2);
                const glm::uvec2 extent = m_extent;
                graph->AddRayTracingPass(std::move(traceRays)).RecordPassCommands([extent](RDGPassCmdEncoder& encoder) {
                    encoder.TraceRays(extent.x, extent.y, 1);
                });
            }
            else
            {
                const glm::uvec2 traceGroups = noGuide ? (m_extent + glm::uvec2(7)) / 8u : guideGroups;
                graph->AddComputePass(std::move(traceCompute)).RecordPassCommands([traceGroups](RDGPassCmdEncoder& encoder) {
                    encoder.Dispatch(traceGroups.x, traceGroups.y, 1);
                });
            }
            if (pipelineTrace && !noGuide)
            {
                RDGComputePassDesc finish;
                finish.SetShaderProgramName(m_deterministicGuideCapture ? "HybridGuideFinishExactSP" : "HybridGuideFinishSP");
                finish.SetPassTag("HybridGuideFinish");
                finish.BindValue("uHybridData", m_uniforms);
                finish.BindValue("uSceneData", m_scene->GetSceneUniformData(), sizeof(SceneUniformData));
                voxelGI.BindLightingInputs(finish);
                voxelGI.BindEnvironmentSamplingInputs(finish);
                finish.BindStorageBuffer("HybridGuidePrevious", m_guide[1 - m_current]);
                finish.BindStorageBuffer("HybridGuidePooled", guidePooled);
                finish.BindStorageBuffer("HybridGuideLearning", guideLearning);
                finish.BindStorageBuffer("HybridGuideNext", m_guide[m_current], RDGContentGuarantee::eFullWrite);
                graph->AddComputePass(std::move(finish)).RecordPassCommands([guideGroups](RDGPassCmdEncoder& encoder) {
                    encoder.Dispatch(guideGroups.x, guideGroups.y, 1);
                });
            }
            if (restir)
            {
                // Temporal updates only its own pixel. Spatial updates one checkerboard parity
                // at a time and reads neighbors of the other parity, so both safely work in place.
                // This avoids another 398 MB of transient reservoirs at 4K.
                // Pass 0 is temporal. Spatial reuse then runs one dispatch per allocated signal and parity, which
                // halves its live state; without spatial reuse one parity-free dispatch per signal resolves the reservoirs.
                // Temporal packs each texel's receiver into 32 bytes so spatial reuse reads a neighbor with one load.
                RDGBufferDesc guidesDesc;
                guidesDesc.usageFlags.SetFlag(RHIBufferUsageFlagBits::eStorageBuffer);
                guidesDesc.name                = "restir_receiver_guides";
                guidesDesc.size                = uint64_t(m_extent.x) * m_extent.y * 2u * sizeof(glm::uvec4);
                const RDGBuffer receiverGuides = graph->GetResourceManager()->CreateBuffer(guidesDesc);
                const uint32_t  spatialPasses  = 2u * (settings.restirSpatial ? 2u : 1u);
                for (uint32_t index = 0; index <= spatialPasses; ++index)
                {
                    const uint32_t stage   = index == 0 ? 0u : 1u + (index - 1u) / 2u;
                    const uint32_t signal  = index == 0 ? 0u : (index - 1u) % 2u;
                    const bool     skipped = index != 0 && (restirMask & (1u << signal)) == 0;
                    if (!skipped)
                    {
                        RecordReSTIRResample(voxelGI, restirData, stage, index == 0 ? 2u : signal, receiverGuides, rawBounce,
                                             rawSpecular, rawDiffuseDistance, rawReflection);
                    }
                }
            }
            if (countRequested)
            {
                graph->AddTransferPass("ReadHybridRayCounts")
                    .CopyBuffer(m_rayCountOutput, m_rayCountReadback, {0, 0, countBytes})
                    .NeverCull();
                m_rayCountsRecorded = true;
            }

            if (relax)
            {
                RecordDenoiser(rawSky, rawBounce, rawSpecular, rawReflection, rawDiffuseDistance);
            }
            else
            {
                // Reference diagnostic: a plain per-pixel mean over static frames, without spatial filtering.
                RDGComputePassDesc temporal;
                temporal.SetShaderProgramName("HybridTemporalReferenceSP");
                temporal.SetPassTag("HybridTemporal");
                temporal.allowCulling = false;
                BindGuides(temporal);
                temporal.BindValue("uSceneData", m_scene->GetSceneUniformData(), sizeof(SceneUniformData));
                temporal.BindSampledTexture("rawSky", m_sampler, rawSky);
                temporal.BindSampledTexture("rawSpecular", m_sampler, rawSpecular);
                temporal.BindSampledTexture("rawBounce", m_sampler, rawBounce);
                temporal.BindSampledTexture("rawBouncePrevious", m_sampler, previous);
                temporal.BindSampledTexture("rawReflection", m_sampler, rawReflection);
                temporal.BindSampledTexture("rawReflectionPrevious", m_sampler, previousReflection);
                for (uint32_t texture = 0; texture < FastSky; ++texture)
                {
                    temporal.BindSampledTexture(NameID(fmt::format("previous{}", kHistoryNames[texture])), m_sampler,
                                                m_history[1 - m_current][texture]->GetDefaultView());
                    temporal.BindStorageImage(NameID(fmt::format("next{}", kHistoryNames[texture])),
                                              m_history[m_current][texture]->GetDefaultView(), RDGContentGuarantee::eFullWrite);
                }
                Dispatch(std::move(temporal));
                m_outputSky      = resources->ImportTexture(m_history[m_current][Sky]);
                m_outputSpecular = resources->ImportTexture(m_history[m_current][Specular]);
                m_outputBounce   = resources->ImportTexture(m_history[m_current][Bounce]);
            }
            ReconstructOutput(voxelGI);
            StabilizeOutput();
            if (m_captureOutput != nullptr && m_captureReadback != nullptr)
            {
                RDGComputePassDesc capture;
                capture.SetShaderProgramName(m_denoiserValidation[0] ? "HybridCaptureValidationSP" : "HybridCaptureSP");
                if (m_worldCache.IsRecorded() && !m_denoiserValidation[0])
                {
                    capture.SetShaderProgramName(NameID(
                        fmt::format("WorldCacheCapture{}SP", m_device->GetGPUInfo().supportBufferInt64Atomics ? 64 : 32)));
                    m_worldCache.Bind(capture);
                }
                if (m_denoiserValidation[0])
                {
                    capture.BindSampledTexture("temporalValidation", m_sampler, m_denoiserValidation[0]);
                    capture.BindSampledTexture("repairValidation", m_sampler, m_denoiserValidation[1]);
                }
                capture.SetPassTag("HybridCapture");
                BindHybridReceiverGuides(capture, m_sampler, m_uniforms);
                capture.BindSampledTexture("rawSky", m_sampler, rawSky);
                capture.BindSampledTexture("rawSpecular", m_sampler, rawSpecular);
                capture.BindSampledTexture("rawBounce", m_sampler, rawBounce);
                capture.BindSampledTexture("reconstructedSky", m_sampler, m_outputSky);
                capture.BindSampledTexture("reconstructedSpecular", m_sampler, m_outputSpecular);
                capture.BindSampledTexture("reconstructedBounce", m_sampler, m_outputBounce);
                capture.BindSampledTexture("moments", m_sampler, m_history[m_current][Moments]->GetDefaultView());
                capture.BindSampledTexture("historyLength", m_sampler, m_history[m_current][Length]->GetDefaultView());
                capture.BindSampledTexture("rawReflection", m_sampler, rawReflection);
                capture.BindSampledTexture("reflectionHistory", m_sampler, m_history[m_current][Reflection]->GetDefaultView());
                capture.BindStorageBuffer("HybridCapture", m_captureOutput, RDGContentGuarantee::eFullWrite);
                Dispatch(std::move(capture), true);
                graph->AddTransferPass("ReadHybridCapture")
                    .CopyBuffer(m_captureOutput, m_captureReadback, {0, 0, m_captureOutput->GetRequiredSize()})
                    .NeverCull();
            }
            m_rawSky      = rawSky;
            m_rawSpecular = rawSpecular;
            m_rawBounce   = rawBounce;

            // The capture pass is recorded whenever both buffers are supplied.
            m_captureRecorded = m_captureOutput != nullptr && m_captureReadback != nullptr;
            m_recorded        = true;
        }
    }
    return m_recorded;
}

void HybridGIRenderer::BindLightingInputs(RDGPassDescBase& pass) const
{
    pass.BindSampledTexture("hybridSurface", m_sampler, "offscreen_receiver");
    pass.BindSampledTexture("hybridDepth", m_sampler, "offscreen_depth");
    pass.BindSampledTexture("hybridSky", m_sampler, m_outputSky);
    pass.BindSampledTexture("hybridSpecular", m_sampler, m_outputSpecular);
    pass.BindSampledTexture("hybridBounce", m_sampler, m_outputBounce);
}

void HybridGIRenderer::BindForwardLightingInputs(RDGPassDescBase& pass) const
{
    BindLightingInputs(pass);
    // Blended layers compare their lobe with the receiver's before reusing its reflections.
    pass.BindSampledTexture("hybridNormal", m_sampler, "offscreen_normal");
    pass.BindSampledTexture("hybridRoughness", m_sampler, "offscreen_roughness");
}

DebugOutputDescription HybridGIRenderer::BuildDebugView(const RenderView& view, const DebugSelection& selection) const
{
    DebugOutputDescription description;
    description.output = selection.output;
    description.available =
        m_recorded && selection.output >= DebugOutput::eGIRawSky && selection.output <= DebugOutput::eGICache;
    description.reason = "Hybrid GI is not active in this view.";
    if (description.available && selection.output == DebugOutput::eGICache && m_worldCache.IsRecorded())
    {
        description.available = selection.face == 0;
        description.reason    = description.available ? "" : "The world cache exposes one irradiance view.";
        if (description.available)
        {
            description.width          = m_outputExtent.x;
            description.height         = m_outputExtent.y;
            description.faceCount      = 1;
            description.format         = DataFormat::eR16G16B16A16SFloat;
            description.interpretation = "World cache total irradiance/pi, evaluated with the pixel normal; Reinhard display.";
            DebugVisualizationData data;
            RDGGraphicsPassDesc    pass       = MakeDebugVisualizationPass(view, "WorldCacheDisplaySP", data);
            const RDGTexture       irradiance = m_worldCache.RecordDebug(m_uniforms, m_sampler, m_outputExtent);
            pass.BindSampledTexture("worldIrradiance", m_sampler, irradiance);
            AddDebugVisualizationPass(*m_device->GetCurrentFrameRDG(), std::move(pass));
        }
    }
    else if (description.available && selection.output == DebugOutput::eGICache)
    {
        description.available = m_cacheCapacity != 0 && selection.face < (m_rayCountsRecorded ? 4u : 2u);
        description.reason    = "Enable the HW radiance cache; query-rate and trace faces also require ray counts.";
        if (description.available)
        {
            description.reason.clear();
            description.width     = m_extent.x;
            description.height    = m_extent.y;
            description.faceCount = m_rayCountsRecorded ? 4u : 2u;
            description.format    = DataFormat::eR32G32B32A32SFloat;
            description.interpretation =
                "Faces: world hash cells, occupied slots (green; red is age), query hit rate, traces per texel.";
            DebugVisualizationData data;
            data.selection.x         = selection.face;
            data.selection.z         = ((m_extent.x + 15u) / 16u) * ((m_extent.y + 15u) / 16u) * 4u;
            data.range.y             = float(std::max(m_uniforms.sampling.y * 3u, 1u));
            RDGGraphicsPassDesc pass = MakeDebugVisualizationPass(
                view, m_device->GetGPUInfo().supportBufferInt64Atomics ? "GICacheDebug64SP" : "GICacheDebug32SP", data);
            BindRadianceCache(pass);
            if (m_extent == m_outputExtent)
            {
                pass.BindSampledTexture("cachePosition", m_sampler, "offscreen_position");
                pass.BindSampledTexture("cacheNormal", m_sampler, "offscreen_normal");
            }
            else
            {
                pass.BindSampledTexture("cachePosition", m_sampler, m_receiverGuides[5]);
                pass.BindSampledTexture("cacheNormal", m_sampler, m_receiverGuides[1]);
            }
            pass.BindStorageBuffer("CacheTraceCounts", m_rayCountsRecorded ? m_rayCountOutput : m_cacheBuffers[1]);
            AddDebugVisualizationPass(*m_device->GetCurrentFrameRDG(), std::move(pass));
        }
    }
    else if (description.available && m_probePath)
    {
        description.available =
            selection.output == DebugOutput::eGIRawSky || selection.output == DebugOutput::eGIReconstructedSky;
        description.reason = description.available ? "" : "Per-pixel denoiser diagnostics do not apply to screen probes.";
        if (description.available)
        {
            description.width          = m_outputExtent.x;
            description.height         = m_outputExtent.y;
            description.faceCount      = 5;
            description.format         = DataFormat::eR16G16B16A16SFloat;
            description.interpretation = "Gathered probe irradiance/pi; faces sky, reflection and bounce; Reinhard display. "
                                         "Face 3 shows probe age from 0 to 256 frames. Face 4 marks uncovered receivers white.";
            DebugVisualizationData data;
            // Face 3 shows the age the gather stores in its bounce output's alpha.
            data.selection.x         = selection.face >= 3 ? selection.face - 2u : 0u;
            RDGGraphicsPassDesc pass = MakeDebugVisualizationPass(view, "WorldCacheDisplaySP", data);
            pass.BindSampledTexture("worldIrradiance", m_sampler,
                                    selection.face == 1   ? m_outputSpecular
                                    : selection.face >= 2 ? m_outputBounce
                                                          : m_outputSky);
            AddDebugVisualizationPass(*m_device->GetCurrentFrameRDG(), std::move(pass));
        }
    }
    else if (description.available && selection.output == DebugOutput::eGIValidation)
    {
        description.available = bool(m_denoiserValidation[0]) && selection.face < 7;
        description.reason    = "Enable ReLAX and denoiserValidation; faces 0..6 select diagnostics.";
        if (description.available)
        {
            description.reason.clear();
            description.width     = m_extent.x;
            description.height    = m_extent.y;
            description.faceCount = 7;
            description.format    = DataFormat::eR32G32B32A32SFloat;
            description.interpretation =
                "Faces 0..5: slow age, fast age, footprint, clamp sigmas, repair mip, rejection reason. RGB: sky, bounce, reflection. "
                "Face 6: visibility slow/fast age and rejection. Reasons 0..8: accepted, reset, outside, identity, plane, normal, geometric, roughness, view.";
            DebugVisualizationData data;
            data.selection.x         = selection.face;
            data.range.y             = selection.face == 0 || selection.face == 6
                                         ? float(m_uniforms.sampling.z * m_uniforms.resolution.z * m_uniforms.resolution.z)
                                         : 4.0f;
            RDGGraphicsPassDesc pass = MakeDebugVisualizationPass(view, "GIDenoiseDebugSP", data);
            pass.BindSampledTexture("validationImage", m_sampler,
                                    m_denoiserValidation[selection.face < 3 || selection.face >= 5 ? 0 : 1]);
            AddDebugVisualizationPass(*m_device->GetCurrentFrameRDG(), std::move(pass));
        }
    }
    else if (description.available)
    {
        description.reason.clear();
        description.width             = view.width;
        description.height            = view.height;
        description.format            = DataFormat::eR16G16B16A16SFloat;
        const char* interpretations[] = {
            "Raw GI radiance converted to sRGB; values above one clip. Face 0 sky, face 1 reflection, face 2 bounce.",
            "Output GI radiance converted to sRGB; values above one clip. Face 0 sky, face 1 reflection, face 2 bounce.",
            "History length, black at zero and white at the configured limit. Face 0 diffuse, face 1 reflection.",
            "Raw specular escape fraction: black is blocked, white escapes. Grey averages reference samples.",
            "History rejection (face 0 diffuse, face 1 reflection): green accepted, red reset, yellow outside view, "
            "magenta identity, cyan plane, blue shading normal, orange geometric normal, white roughness, grey view.",
            "Reconstruction fallback: red uses nearest history-fixed diffuse, blue reflection, magenta both; "
            "dark grey interpolates. Native-resolution GI has no fallback."};
        const uint32_t mode          = static_cast<uint32_t>(selection.output) - static_cast<uint32_t>(DebugOutput::eGIRawSky);
        description.interpretation   = interpretations[mode];
        const bool             relax = m_uniforms.reservoir.w != 0u;
        DebugVisualizationData data;
        data.selection.x         = mode;
        data.selection.y         = selection.face == 1 ? 1u : 0u;
        data.selection.z         = m_missingReceiver ? 1u : 0u;
        data.range.y             = static_cast<float>(m_uniforms.sampling.z)
                                 * (relax ? static_cast<float>(m_uniforms.resolution.z * m_uniforms.resolution.z) : 1.0f);
        RDGGraphicsPassDesc pass = MakeDebugVisualizationPass(view, "HybridDebugSP", data);
        if (selection.output == DebugOutput::eGIRawSky || selection.output == DebugOutput::eGIReconstructedSky
            || selection.output == DebugOutput::eGIHitMiss)
        {
            const RDGTexture raw      = selection.face == 1 ? m_rawSpecular : selection.face == 2 ? m_rawBounce : m_rawSky;
            const RDGTexture filtered = selection.face == 1 ? m_outputSpecular
                                      : selection.face == 2 ? m_outputBounce
                                                            : m_outputSky;
            const RDGTexture source   = selection.output == DebugOutput::eGIRawSky  ? raw
                                      : selection.output == DebugOutput::eGIHitMiss ? m_rawSpecular
                                                                                    : filtered;
            pass.BindSampledTexture("sourceImage", m_sampler, source);
        }
        else
        {
            const uint32_t layer =
                selection.output == DebugOutput::eGIHistoryLength && data.selection.y != 0 ? Reflection : Length;
            pass.BindSampledTexture("sourceImage", m_sampler, m_history[m_current][layer]->GetDefaultView());
        }
        pass.BindSampledTexture("historyLength", m_sampler, m_history[m_current][Length]->GetDefaultView());
        // Native resolution has no fallback; the receiver identities only satisfy the binding.
        if (m_missingReceiver)
        {
            pass.BindSampledTexture("missingReceiver", m_sampler, m_missingReceiver);
        }
        else
        {
            pass.BindSampledTexture("missingReceiver", m_sampler, "offscreen_receiver");
        }
        AddDebugVisualizationPass(*m_device->GetCurrentFrameRDG(), std::move(pass));
    }
    return description;
}

void HybridGIRenderer::OnRenderGraphExecuted(RenderFrameResult result)
{
    m_screenProbes.OnRenderGraphExecuted(result);
    m_worldCache.OnRenderGraphExecuted(result);
    const bool succeeded = result == RenderFrameResult::eSubmitted;
    m_captureRecorded    = m_captureRecorded && m_recorded && succeeded;

    if (m_recorded && succeeded)
    {
        m_cacheNeedsClear    = false;
        m_outputHistoryValid = m_stabilizeOutput;
        for (uint32_t side = 0; side < 2; ++side)
        {
            std::fill(std::begin(m_historyNeedsClear[side]), std::end(m_historyNeedsClear[side]), false);
        }
        m_current               = 1 - m_current;
        m_restirLobeInitialized = m_restirLobeIntegral != nullptr;
        m_valid                 = true;
        ++m_frame;
        m_previousModels                  = m_recordedModels;
        m_previousVertices                = m_recordedVertices;
        m_previousVertexGeneration        = m_recordedVertexGeneration;
        m_previousProjection              = m_recordedProjection;
        m_uniforms.previousProjectionView = m_recordedProjectionView;
        m_uniforms.previousViewPosition   = m_recordedViewPosition;
        m_uniforms.previousViewDirection  = m_recordedViewDirection;
        m_geometry                        = m_scene->GetGeometryRevision();
        m_surface                         = m_scene->GetSurfaceRevision();
        m_environment                     = m_scene->GetEnvironmentRevision();
        m_lighting                        = m_scene->GetLightingRevision();
        m_radianceGeneration              = m_recordedRadianceGeneration;
        m_previousSettings                = m_recordedSettings;
        m_previousReflections             = m_uniforms.reflection.x != 0;
        m_previousHardwareMask            = m_uniforms.provider.z;
        m_previousVisibility              = m_recordedVisibility;
        m_previousGuideWritten            = m_recordedGuideWritten;
        if (m_bounceResponsiveFrames != 0)
        {
            --m_bounceResponsiveFrames;
        }
    }
    else if (result == RenderFrameResult::eDeclined && m_prepared)
    {
        m_valid                  = m_preparedHistoryValid;

        m_resetReason            = m_preparedResetReason;

        m_bounceResponsiveFrames = m_preparedResponsiveFrames;
    }
    else if (result != RenderFrameResult::eDeclined)
    {
        m_outputHistoryValid = false;
        if (!succeeded)
        {
            Destroy();
        }
        m_valid       = false;
        m_resetReason = succeeded ? "inactive" : "failed_frame";
    }
    m_recorded         = false;
    m_prepared         = false;
    m_recordedVertices = nullptr;
}

void HybridGIRenderer::Destroy()
{
    m_screenProbes.Destroy();
    m_worldCache.Destroy();
    DestroyPixelHistory();
}

void HybridGIRenderer::DestroyPixelHistory()
{
    m_nativeProbeRaySlots = 0;
    DestroyRadianceCache();
    for (uint32_t side = 0; side < 2; ++side)
    {
        for (uint32_t texture = 0; texture < OutputCount; ++texture)
        {
            m_device->DestroyTexture(m_outputHistory[side][texture]);
            m_outputHistory[side][texture] = nullptr;
        }
        for (uint32_t texture = 0; texture < Count; ++texture)
        {
            m_device->DestroyTexture(m_history[side][texture]);
            m_history[side][texture] = nullptr;
        }
        m_device->DestroyBuffer(m_guide[side]);
        m_guide[side] = nullptr;
        for (uint32_t type = 0; type < 2; ++type)
        {
            m_device->DestroyBuffer(m_reservoir[side][type]);
            m_reservoir[side][type] = nullptr;
        }
    }
    m_device->DestroyTexture(m_restirLobeIntegral);
    m_outputHistoryValid    = false;
    m_stabilizeOutput       = false;
    m_restirLobeIntegral    = nullptr;
    m_restirLobeInitialized = false;
    m_previousGuideWritten  = false;
    m_recordedGuideWritten  = false;
    m_previousVertices      = nullptr;
    m_recordedVertices      = nullptr;
    m_valid                 = false;
    m_preparedHistoryValid  = false;
    m_extent                = glm::uvec2(0);
    m_outputExtent          = glm::uvec2(0);
}
} // namespace zen::rc
