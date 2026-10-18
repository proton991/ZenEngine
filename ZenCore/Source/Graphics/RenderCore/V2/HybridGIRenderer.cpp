#include "Graphics/RenderCore/V2/Renderer/HybridGIRenderer.h"
#include "Graphics/RenderCore/V2/RenderDevice.h"
#include "Graphics/RenderCore/V2/RenderScene.h"
#include "SceneGraph/Camera.h"
#include "Graphics/Shared/EnvironmentSampling.h"
#include <string_view>

namespace zen::rc
{
namespace
{
const char* const kHistoryNames[]   = {"Sky",      "Specular", "Moments",       "Length",     "Position",     "Normal",
                                       "Receiver", "Bounce",   "BounceMoments", "Reflection", "SearchWeight", "Budget"};
const DataFormat  kHistoryFormats[] = {
    DataFormat::eR32G32B32A32SFloat, DataFormat::eR16G16B16A16SFloat, DataFormat::eR32G32B32A32SFloat,
    DataFormat::eR16G16B16A16SFloat, DataFormat::eR32G32B32A32SFloat, DataFormat::eR16G16SFloat,
    DataFormat::eR32G32UInt,         DataFormat::eR32G32B32A32SFloat, DataFormat::eR32G32SFloat,
    DataFormat::eR32G32B32A32SFloat, DataFormat::eR32SFloat,          DataFormat::eR8UInt};
// The trace pass learns and reads the visibility guide per workgroup of this many pixels squared.
constexpr uint32_t kGuideTile = 16;
} // namespace

HybridGIRenderer::HybridGIRenderer(RenderDevice* device) : m_device(device)
{
    RHISamplerCreateInfo info{};
    m_sampler = m_device->CreateSampler(info);
}

uint64_t HybridGIRenderer::GetRayCountCaptureBytes(uint32_t width, uint32_t height)
{
    // Per tile: Compute and HW RT counts from the trace pass, then from the diffuse fallback pass.
    return ((uint64_t(width) + kGuideTile - 1) / kGuideTile) * ((uint64_t(height) + kGuideTile - 1) / kGuideTile) * 16u
         * sizeof(uint32_t);
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
    if (m_valid && !scene.HasVertexGeneration(m_previousVertices.Get(), m_previousVertexGeneration))
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
    result.previousModel = m_valid && node < m_previousModels.size() ? m_previousModels[node] : m_recordedModels[node];
    return result;
}

bool HybridGIRenderer::PrepareHistory(uint32_t width,
                                      uint32_t height,
                                      bool     bounce,
                                      bool     reflections,
                                      bool     search,
                                      bool     reference,
                                      bool     budget)
{
    if (m_extent != glm::uvec2(width, height))
    {
        // The current receiver pass has already captured this frame's buffer.
        RHIResourcePtr<RHIBuffer> recordedVertices = m_recordedVertices;
        Destroy();
        m_recordedVertices    = std::move(recordedVertices);
        m_extent              = glm::uvec2(width, height);
        m_resetReason         = "resize";

        m_preparedResetReason = m_resetReason;
    }
    bool valid = true;
    for (uint32_t side = 0; side < 2 && valid; ++side)
    {
        for (uint32_t texture = 0; texture < Count && valid; ++texture)
        {
            // Disabled signals use 1x1 placeholders. Enabling them resets their own histories.
            const bool       full   = texture == Budget                             ? budget
                                    : texture == SearchWeight                       ? search
                                    : texture == Reflection                         ? reflections
                                    : texture == Bounce || texture == BounceMoments ? bounce
                                                                                    : true;
            const glm::uvec2 extent = full ? glm::uvec2(width, height) : glm::uvec2(1);
            // Long reference accumulations must not repeatedly round the specular mean to FP16.
            const DataFormat historyFormat =
                reference && texture == Specular ? DataFormat::eR32G32B32A32SFloat : kHistoryFormats[texture];
            RHITexture*& history = m_history[side][texture];
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
    for (uint32_t side = 0; side < 2 && valid; ++side)
    {
        if (m_guide[side] == nullptr)
        {
            // Per tile: the environment-tile weights and the number of frames they average.
            m_guide[side] = m_device->CreateStorageBuffer(tiles.x * tiles.y * (ZEN_HYBRID_GUIDE_TILES + 1u) * sizeof(float),
                                                          nullptr, NameID(fmt::format("hybrid_guide_{}", side)));
            valid         = m_guide[side] != nullptr;
        }
    }
    if (!valid)
    {
        Destroy();
    }
    return valid;
}

void HybridGIRenderer::BindGuides(RDGPassDescBase& pass) const
{
    pass.BindValue("uHybridData", m_uniforms);
    pass.BindSampledTexture("receiverDepth", m_sampler, "offscreen_depth");
    pass.BindSampledTexture("receiverNormal", m_sampler, "offscreen_normal");
    pass.BindSampledTexture("receiverRoughness", m_sampler, "offscreen_roughness");
    pass.BindSampledTexture("receiverSurface", m_sampler, "offscreen_receiver");
    pass.BindSampledTexture("receiverMotion", m_sampler, "offscreen_motion");
    pass.BindSampledTexture("receiverPosition", m_sampler, "offscreen_position");
}

void HybridGIRenderer::BindTraceInputs(RDGComputePassDesc& pass, VoxelGIRenderer& voxelGI, const glm::uvec4& reuseData) const
{
    BindGuides(pass);

    pass.BindValue("uHybridReuseData", reuseData);

    pass.BindSampledTexture("previousSkyMoments", m_sampler, m_history[1 - m_current][Moments]->GetDefaultView());

    pass.BindSampledTexture("previousHistoryLength", m_sampler, m_history[1 - m_current][Length]->GetDefaultView());

    pass.BindSampledTexture("previousBudget", m_sampler, m_history[1 - m_current][Budget]->GetDefaultView());

    voxelGI.BindEnvironmentSamplingInputs(pass);

    voxelGI.BindEnvironmentHarmonics(pass);

    voxelGI.BindHitRadianceInputs(pass);

    if (m_uniforms.provider.x != 0)
    {
        voxelGI.BindHardwareRayInputs(pass);
    }
    else
    {
        voxelGI.BindRayInputs(pass);

        voxelGI.BindSoftwareRayInputs(pass);
    }

    pass.BindValue("uSceneData", m_scene->GetSceneUniformData(), sizeof(SceneUniformData));

    pass.BindStorageBuffer("HybridGuidePrevious", m_guide[1 - m_current]);
}

void HybridGIRenderer::Dispatch(RDGComputePassDesc&& pass)
{
    const glm::uvec2 groups = (m_extent + glm::uvec2(7)) / 8u;
    m_device->GetCurrentFrameRDG()->AddComputePass(std::move(pass)).RecordPassCommands([groups](RDGPassCmdEncoder& encoder) {
        encoder.Dispatch(groups.x, groups.y, 1);
    });
}

bool HybridGIRenderer::BuildRenderGraph(const RenderView& view, VoxelGIRenderer& voxelGI)
{
    const VoxelGISettings& settings       = voxelGI.GetSettings();
    const bool             rays           = voxelGI.UsesRayBounce();
    const bool             reflections    = voxelGI.UsesReflections();
    const uint32_t         hardwareMask   = voxelGI.GetHardwareFeatureMask();
    const bool             countRequested = m_rayCountOutput != nullptr || m_rayCountReadback != nullptr;
    const uint64_t         countBytes     = GetRayCountCaptureBytes(view.width, view.height);
    const bool             countValid =
        !countRequested
        || (m_rayCountOutput != nullptr && m_rayCountReadback != nullptr && m_rayCountOutput->GetRequiredSize() == countBytes
            && m_rayCountReadback->GetRequiredSize() == countBytes);
    m_rayCountsRecorded = false;
    if (!countValid)
    {
        LOGE("Hybrid ray-count capture requires two correctly sized buffers");
    }
    const bool reference     = settings.referenceSamples != 0;
    const bool searchEnabled = settings.filter && !reference;
    const bool ready         = countValid && m_prepared
                            && PrepareHistory(view.width, view.height, rays, reflections, searchEnabled, reference,
                                              settings.UsesAdaptiveSampling());
    if (ready)
    {
        const bool hadValidHistory = m_valid;
        m_resetReason              = m_valid ? "none" : m_resetReason;
        if (m_geometry != m_scene->GetGeometryRevision())
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
        else if (settings.referenceSamples != m_previousSettings.referenceSamples
                 || settings.samples != m_previousSettings.samples || settings.rayProvider != m_previousSettings.rayProvider
                 || settings.adaptiveSampling != m_previousSettings.adaptiveSampling
                 || settings.featureProviders != m_previousSettings.featureProviders
                 || settings.normalBiasVoxels != m_previousSettings.normalBiasVoxels
                 || settings.temporal != m_previousSettings.temporal
                 || settings.previousFrameRadiance != m_previousSettings.previousFrameRadiance
                 || settings.historyFrames != m_previousSettings.historyFrames
                 || settings.specularOcclusion != m_previousSettings.specularOcclusion)
        {
            InvalidateHistory("settings");
        }
        if (m_previousHardwareMask != hardwareMask)
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
        const bool bounceReset     = rays != m_previousRayBounce || radianceReset;
        const bool reflectionReset = reflections != m_previousReflections || radianceReset;
        if (bounceReset || reflectionReset || radianceSteps != 0)
        {
            m_bounceResponsiveFrames = 8;
        }
        m_uniforms.bounce =
            glm::uvec4(rays ? 1u : 0u, m_valid && !bounceReset ? 1u : 0u, m_bounceResponsiveFrames != 0 ? 1u : 0u,
                       radianceSteps == 1 && !missingPrevious ? 1u : 0u);
        m_uniforms.reflection =
            glm::uvec4(reflections ? 1u : 0u, m_valid && !reflectionReset ? 1u : 0u, m_bounceResponsiveFrames != 0 ? 1u : 0u,
                       radianceSteps == 1 && !missingPrevious ? 1u : 0u);
        // Sky, specular occlusion, ray bounce and reflections share this dispatch.
        const bool hardwareTrace = (hardwareMask & 27u) != 0;
        // Only geometry-dependent weights survive an unchanged receiver image.
        // Lighting and guide values are always read from this frame's inputs.
        const bool       unchangedReceivers = m_valid && m_recordedProjectionView == m_uniforms.previousProjectionView
                                           && m_surface == m_scene->GetSurfaceRevision();
        const glm::uvec4 reuseData(
            unchangedReceivers && m_previousSettings.filter && m_previousSettings.referenceSamples == 0 ? 1u : 0u,
            unchangedReceivers ? 1u : 0u, 0u, 0u);
        m_recordedSettings  = settings;
        m_uniforms.sampling = glm::uvec4(m_frame, settings.referenceSamples != 0 ? settings.referenceSamples : settings.samples,
                                         settings.referenceSamples != 0 ? 256u : settings.historyFrames, m_valid ? 1 : 0);
        // A guide from a successful frame can survive geometry, opacity and sampling-setting resets.
        // Provider changes use different visibility geometry and must relearn the proposal.
        // A reset reason alone cannot make a fresh or failed-frame guide safe to read.
        const std::string_view reason = m_resetReason;
        const bool guideValid         = hadValidHistory && (m_valid || reason == "geometry_or_opacity" || reason == "settings");
        m_uniforms.provider =
            glm::uvec4(hardwareTrace ? 1u : 0u, guideValid ? 1u : 0u, hardwareMask, settings.UsesAdaptiveSampling() ? 1u : 0u);
        m_uniforms.rejection          = Vec4(0.001f, settings.specularOcclusion ? 1.0f : 0.0f,
                                             settings.temporal || settings.referenceSamples != 0 ? 1.0f : 0.0f,
                                             settings.referenceSamples != 0 ? 1.0f : 0.0f);
        RenderGraph*        graph     = m_device->GetCurrentFrameRDG();
        RDGResourceManager* resources = graph->GetResourceManager();
        RDGTextureDesc      desc;
        desc.texFormat.dimension = TextureDimension::e2D;
        desc.texFormat.depth     = 1;
        desc.texFormat.width     = view.width;
        desc.texFormat.height    = view.height;
        desc.texFormat.format    = DataFormat::eR16G16B16A16SFloat;
        desc.usageFlags.SetFlag(RHITextureUsageFlagBits::eStorage);
        desc.usageFlags.SetFlag(RHITextureUsageFlagBits::eSampled);
        desc.name                    = "hybrid_raw_sky";
        const RDGTexture rawSky      = resources->CreateTexture(desc);
        desc.name                    = "hybrid_raw_specular";
        const RDGTexture rawSpecular = resources->CreateTexture(desc);
        // Bounce textures are 1x1 placeholders for cone bounce; the passes skip them.
        RDGTextureDesc bounceDesc = desc;
        if (!rays)
        {
            bounceDesc.texFormat.width = bounceDesc.texFormat.height = 1;
        }
        bounceDesc.name                 = "hybrid_raw_bounce";
        const RDGTexture rawBounce      = resources->CreateTexture(bounceDesc);
        bounceDesc.name                 = "hybrid_raw_bounce_previous";
        const RDGTexture previous       = resources->CreateTexture(bounceDesc);
        RDGTextureDesc   reflectionDesc = desc;
        if (!reflections)
        {
            reflectionDesc.texFormat.width = reflectionDesc.texFormat.height = 1;
        }
        reflectionDesc.name                 = "hybrid_raw_reflection_previous";
        const RDGTexture previousReflection = resources->CreateTexture(reflectionDesc);
        reflectionDesc.name                 = "hybrid_raw_reflection";
        reflectionDesc.texFormat.format     = DataFormat::eR32G32SFloat;
        const RDGTexture rawReflection      = resources->CreateTexture(reflectionDesc);
        desc.name                           = "hybrid_sky_guide";
        const RDGTexture skyGuide           = resources->CreateTexture(desc);
        // Half resolution traces diffuse rays for one rotating leader per 2x2 block; an upsample
        // reconstructs the other receivers and lists those that need full-resolution rays. It needs
        // valid history (dark and dissimilar receivers are found from it) and temporal accumulation
        // to gather each receiver's own samples, and applies to the software trace variant.
        const bool halfRequested       = settings.diffuseResolution == VoxelGISettings::DiffuseResolution::Half
                                      || (settings.diffuseResolution == VoxelGISettings::DiffuseResolution::Auto
                                          && !voxelGI.UsesHardwareQueries(VoxelGISettings::Feature::Sky));
        const bool halfDiffuse         = halfRequested && !reference && settings.temporal && m_valid && !hardwareTrace;
        m_halfDiffuse                  = halfDiffuse;
        const glm::uvec2   guideGroups = (m_extent + glm::uvec2(kGuideTile - 1)) / kGuideTile;
        RDGComputePassDesc trace;
        trace.SetShaderProgramName(halfDiffuse      ? (countRequested ? "HybridTraceHalfCountsSP" : "HybridTraceHalfSP")
                                   : countRequested ? (hardwareTrace ? "HybridTraceHardwareCountsSP" : "HybridTraceCountsSP")
                                                    : (hardwareTrace ? "HybridTraceHardwareSP" : "HybridTraceSP"));
        trace.SetPassTag("HybridTrace");
        BindTraceInputs(trace, voxelGI, reuseData);
        trace.BindStorageImage("nextBudget", m_history[m_current][Budget]->GetDefaultView(), RDGContentGuarantee::eFullWrite);
        trace.BindStorageImage("rawSpecular", rawSpecular, RDGContentGuarantee::eFullWrite);
        trace.BindStorageImage("rawReflection", rawReflection, RDGContentGuarantee::eFullWrite);
        trace.BindStorageImage("rawReflectionPrevious", previousReflection, RDGContentGuarantee::eFullWrite);
        trace.BindStorageBuffer("HybridGuideNext", m_guide[m_current], RDGContentGuarantee::eFullWrite);
        if (countRequested)
        {
            trace.BindStorageBuffer("HybridRayCounts", m_rayCountOutput, RDGContentGuarantee::eFullWrite);
        }
        RDGTexture halfSky, halfBounce, halfBouncePrevious, halfGuide;
        RDGBuffer  fallbackArgs, fallbackPixels, pooledGuide;
        if (halfDiffuse)
        {
            RDGTextureDesc halfDesc   = desc;
            halfDesc.texFormat.width  = (view.width + 1u) / 2u;
            halfDesc.texFormat.height = (view.height + 1u) / 2u;
            halfDesc.name             = "hybrid_half_sky";
            halfSky                   = resources->CreateTexture(halfDesc);
            halfDesc.name             = "hybrid_half_guide";
            halfGuide                 = resources->CreateTexture(halfDesc);
            if (!rays)
            {
                halfDesc.texFormat.width = halfDesc.texFormat.height = 1;
            }
            halfDesc.name      = "hybrid_half_bounce";
            halfBounce         = resources->CreateTexture(halfDesc);
            halfDesc.name      = "hybrid_half_bounce_previous";
            halfBouncePrevious = resources->CreateTexture(halfDesc);
            RDGBufferDesc arguments;
            arguments.name = "hybrid_diffuse_fallback_args";
            arguments.size = 4u * sizeof(uint32_t);
            arguments.usageFlags.SetFlags(RHIBufferUsageFlagBits::eStorageBuffer, RHIBufferUsageFlagBits::eIndirectBuffer);
            fallbackArgs = resources->CreateBuffer(arguments);
            RDGBufferDesc pixels;
            pixels.name = "hybrid_diffuse_fallback_pixels";
            pixels.size = uint64_t(view.width) * view.height * sizeof(uint32_t);
            pixels.usageFlags.SetFlag(RHIBufferUsageFlagBits::eStorageBuffer);
            fallbackPixels = resources->CreateBuffer(pixels);
            // Per tile: guide cell weights, 6x8 row moments and total power (hybrid_trace.glsl).
            RDGBufferDesc pooled;
            pooled.name = "hybrid_pooled_guide";
            pooled.size = uint64_t(guideGroups.x) * guideGroups.y
                        * (ZEN_HYBRID_GUIDE_TILES + 4u * 6u * ZEN_HYBRID_GUIDE_PER_FACE + 4u) * sizeof(float);
            pooled.usageFlags.SetFlag(RHIBufferUsageFlagBits::eStorageBuffer);
            pooledGuide = resources->CreateBuffer(pooled);
            trace.BindStorageImage("halfSky", halfSky, RDGContentGuarantee::eFullWrite);
            trace.BindStorageImage("halfGuide", halfGuide, RDGContentGuarantee::eFullWrite);
            trace.BindStorageImage("halfBounce", halfBounce, RDGContentGuarantee::eFullWrite);
            trace.BindStorageImage("halfBouncePrevious", halfBouncePrevious, RDGContentGuarantee::eFullWrite);
            trace.BindStorageImage("skyGuide", skyGuide, RDGContentGuarantee::eFullWrite);
            // The first workgroup resets the fallback list; every workgroup publishes its pooled guide.
            trace.BindStorageBuffer("HybridDiffuseFallbackArgs", fallbackArgs, RDGContentGuarantee::eFullWrite);
            trace.BindStorageBuffer("HybridPooledGuide", pooledGuide, RDGContentGuarantee::eFullWrite);
        }
        else
        {
            trace.BindStorageImage("skyGuide", skyGuide, RDGContentGuarantee::eFullWrite);
            trace.BindStorageImage("rawSky", rawSky, RDGContentGuarantee::eFullWrite);
            trace.BindStorageImage("rawBounce", rawBounce, RDGContentGuarantee::eFullWrite);
            trace.BindStorageImage("rawBouncePrevious", previous, RDGContentGuarantee::eFullWrite);
        }
        graph->AddComputePass(std::move(trace)).RecordPassCommands([guideGroups](RDGPassCmdEncoder& encoder) {
            encoder.Dispatch(guideGroups.x, guideGroups.y, 1);
        });
        if (halfDiffuse)
        {
            RDGComputePassDesc upsample;
            upsample.SetShaderProgramName("HybridDiffuseUpsampleSP");
            upsample.SetPassTag("HybridDiffuseUpsample");
            BindGuides(upsample);
            upsample.BindSampledTexture("halfSky", m_sampler, halfSky);
            upsample.BindSampledTexture("halfBounce", m_sampler, halfBounce);
            upsample.BindSampledTexture("halfBouncePrevious", m_sampler, halfBouncePrevious);
            upsample.BindSampledTexture("halfGuide", m_sampler, halfGuide);
            upsample.BindSampledTexture("previousSkyMoments", m_sampler, m_history[1 - m_current][Moments]->GetDefaultView());
            upsample.BindSampledTexture("previousHistoryLength", m_sampler, m_history[1 - m_current][Length]->GetDefaultView());
            upsample.BindStorageImage("rawSky", rawSky, RDGContentGuarantee::eFullWrite);
            upsample.BindStorageImage("rawBounce", rawBounce, RDGContentGuarantee::eFullWrite);
            upsample.BindStorageImage("rawBouncePrevious", previous, RDGContentGuarantee::eFullWrite);
            upsample.BindSampledTexture("skyGuide", m_sampler, skyGuide);
            upsample.BindStorageBuffer("HybridDiffuseFallbackArgs", fallbackArgs);
            upsample.BindStorageBuffer("HybridDiffuseFallbackPixels", fallbackPixels, RDGContentGuarantee::eProducedElements);
            Dispatch(std::move(upsample));
            RDGComputePassDesc fallback;
            fallback.SetShaderProgramName(countRequested ? "HybridTraceFallbackCountsSP" : "HybridTraceFallbackSP");
            fallback.SetPassTag("HybridTraceFallback");
            BindTraceInputs(fallback, voxelGI, reuseData);
            fallback.BindStorageImage("nextBudget", m_history[m_current][Budget]->GetDefaultView());
            fallback.BindStorageImage("rawSky", rawSky);
            fallback.BindStorageImage("rawBounce", rawBounce);
            fallback.BindStorageImage("rawBouncePrevious", previous);
            fallback.BindStorageImage("skyGuide", skyGuide);
            fallback.BindStorageBuffer("HybridDiffuseFallbackArgs", fallbackArgs);
            fallback.BindStorageBuffer("HybridDiffuseFallbackPixels", fallbackPixels,
                                       RDGContentGuarantee::eConsumeProducedElements);
            fallback.BindStorageBuffer("HybridPooledGuide", pooledGuide);
            if (countRequested)
            {
                fallback.BindStorageBuffer("HybridRayCounts", m_rayCountOutput);
            }
            fallback.UseIndirectBuffer(fallbackArgs);
            graph->AddComputePass(std::move(fallback)).RecordPassCommands([fallbackArgs](RDGPassCmdEncoder& encoder) {
                encoder.DispatchIndirect(fallbackArgs, sizeof(uint32_t));
            });
        }
        if (countRequested)
        {
            graph->AddTransferPass("ReadHybridRayCounts")
                .CopyBuffer(m_rayCountOutput, m_rayCountReadback, {0, 0, countBytes})
                .NeverCull();
            m_rayCountsRecorded = true;
        }

        const bool         cachedHistory = !reference && (rays || reflections) && m_bounceResponsiveFrames != 0;
        RDGComputePassDesc temporal;
        temporal.SetShaderProgramName(reference ? "HybridTemporalReferenceSP"
                                                : (cachedHistory ? "HybridTemporalCachedSP" : "HybridTemporalSP"));
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
        for (uint32_t texture = 0; texture < SearchWeight; ++texture)
        {
            temporal.BindSampledTexture(NameID(fmt::format("previous{}", kHistoryNames[texture])), m_sampler,
                                        m_history[1 - m_current][texture]->GetDefaultView());
            temporal.BindStorageImage(NameID(fmt::format("next{}", kHistoryNames[texture])),
                                      m_history[m_current][texture]->GetDefaultView(), RDGContentGuarantee::eFullWrite);
        }
        const uint32_t   temporalTile   = cachedHistory ? 16u : 8u;
        const glm::uvec2 temporalGroups = (m_extent + glm::uvec2(temporalTile - 1)) / temporalTile;
        graph->AddComputePass(std::move(temporal)).RecordPassCommands([temporalGroups](RDGPassCmdEncoder& encoder) {
            encoder.Dispatch(temporalGroups.x, temporalGroups.y, 1);
        });

        m_outputSky                     = resources->ImportTexture(m_history[m_current][Sky]);
        m_outputSpecular                = resources->ImportTexture(m_history[m_current][Specular]);
        m_outputBounce                  = resources->ImportTexture(m_history[m_current][Bounce]);
        RDGTexture     previousVariance = resources->ImportTexture(m_history[m_current][Moments]);
        const uint32_t iterations       = searchEnabled ? 5u : 0u;
        if (iterations != 0)
        {
            RDGTextureDesc varianceDesc   = desc;
            varianceDesc.name             = "hybrid_initial_variance";
            varianceDesc.texFormat.format = DataFormat::eR32G32B32A32SFloat;
            previousVariance              = resources->CreateTexture(varianceDesc);
            RDGComputePassDesc prepare;
            prepare.SetShaderProgramName("HybridFilterPrepareSP");
            prepare.SetPassTag("HybridFilterPrepare");
            BindGuides(prepare);
            prepare.BindSampledTexture("moments", m_sampler, m_history[m_current][Moments]->GetDefaultView());
            prepare.BindSampledTexture("historyLength", m_sampler, m_history[m_current][Length]->GetDefaultView());
            prepare.BindSampledTexture("bounceMoments", m_sampler, m_history[m_current][BounceMoments]->GetDefaultView());
            prepare.BindSampledTexture("bounceHistory", m_sampler, m_history[m_current][Bounce]->GetDefaultView());
            prepare.BindSampledTexture("reflectionHistory", m_sampler, m_history[m_current][Reflection]->GetDefaultView());
            prepare.BindStorageImage("initialVariance", previousVariance, RDGContentGuarantee::eFullWrite);
            Dispatch(std::move(prepare));
        }
        for (uint32_t iteration = 0; iteration < iterations; ++iteration)
        {
            // Only the final filter needs the conservative 8x8 support mask.
            RDGTexture skySupport;
            RDGBuffer  searchArgs;
            RDGBuffer  searchPixels;
            RDGBuffer  supportBounds;
            if (iteration == 4)
            {
                RDGBufferDesc arguments;
                arguments.name = "hybrid_sky_search_args";
                arguments.size = 4u * sizeof(uint32_t);
                arguments.usageFlags.SetFlags(RHIBufferUsageFlagBits::eStorageBuffer, RHIBufferUsageFlagBits::eIndirectBuffer);
                searchArgs = resources->CreateBuffer(arguments);
                RDGBufferDesc pixels;
                pixels.name = "hybrid_sky_search_pixels";
                pixels.size = uint64_t(view.width) * view.height * sizeof(uint32_t);
                pixels.usageFlags.SetFlag(RHIBufferUsageFlagBits::eStorageBuffer);
                searchPixels                 = resources->CreateBuffer(pixels);
                RDGTextureDesc supportDesc   = desc;
                supportDesc.name             = "hybrid_sky_support";
                supportDesc.texFormat.width  = (view.width + 7u) / 8u;
                supportDesc.texFormat.height = (view.height + 7u) / 8u;
                supportDesc.texFormat.format = DataFormat::eR32SFloat;
                skySupport                   = resources->CreateTexture(supportDesc);
                RDGBufferDesc bounds;
                bounds.name = "hybrid_sky_support_bounds";
                // Six dominant-axis normal buckets, each with a lower and an upper bound.
                bounds.size = uint64_t(supportDesc.texFormat.width) * supportDesc.texFormat.height * 12u * sizeof(Vec4);
                bounds.usageFlags.SetFlag(RHIBufferUsageFlagBits::eStorageBuffer);
                supportBounds = resources->CreateBuffer(bounds);
                RDGComputePassDesc support;
                support.SetShaderProgramName("HybridSkySupportSP");
                support.SetPassTag("HybridSkySupport");
                support.BindSampledTexture("inputSky", m_sampler, m_outputSky);
                support.BindSampledTexture("inputVariance", m_sampler, previousVariance);
                support.BindSampledTexture("receiverSurface", m_sampler, "offscreen_receiver");
                support.BindSampledTexture("filterPosition", m_sampler, m_history[m_current][Position]->GetDefaultView());
                support.BindStorageImage("skySupport", skySupport, RDGContentGuarantee::eFullWrite);
                support.BindStorageBuffer("HybridSkySearchArgs", searchArgs, RDGContentGuarantee::eFullWrite);
                support.BindStorageBuffer("HybridSkySupportBounds", supportBounds, RDGContentGuarantee::eFullWrite);
                Dispatch(std::move(support));
            }
            desc.name                 = NameID(fmt::format("hybrid_sky_{}", iteration));
            const RDGTexture sky      = resources->CreateTexture(desc);
            desc.name                 = NameID(fmt::format("hybrid_specular_{}", iteration));
            const RDGTexture specular = resources->CreateTexture(desc);
            bounceDesc.name           = NameID(fmt::format("hybrid_bounce_{}", iteration));
            const RDGTexture bounce   = resources->CreateTexture(bounceDesc);
            desc.name                 = NameID(fmt::format("hybrid_variance_{}", iteration));
            desc.texFormat.format     = DataFormat::eR32G32B32A32SFloat;
            const RDGTexture variance = resources->CreateTexture(desc);
            desc.texFormat.format     = DataFormat::eR16G16B16A16SFloat;
            RDGComputePassDesc filter;
            const RDGTexture   sourceSky      = m_outputSky;
            const RDGTexture   sourceVariance = previousVariance;
            filter.SetShaderProgramName(NameID(fmt::format("HybridFilter{}SP", iteration)));
            filter.SetPassTag(NameID(fmt::format("HybridFilter{}", iteration)));
            BindGuides(filter);
            filter.BindSampledTexture("filterPosition", m_sampler, m_history[m_current][Position]->GetDefaultView());
            filter.BindSampledTexture("skyGuide", m_sampler, skyGuide);
            if (iteration == 4)
            {
                filter.BindSampledTexture("skySupport", m_sampler, skySupport);
                filter.BindStorageBuffer("HybridSkySearchArgs", searchArgs);
                // The final filter appends only the receivers that need the dense search.
                filter.BindStorageBuffer("HybridSkySearchPixels", searchPixels, RDGContentGuarantee::eProducedElements);
                filter.BindStorageBuffer("HybridSkySupportBounds", supportBounds);
                filter.BindValue("uHybridReuseData", reuseData);
                filter.BindSampledTexture("previousSearchWeight", m_sampler,
                                          m_history[1 - m_current][SearchWeight]->GetDefaultView());
                filter.BindStorageImage("nextSearchWeight", m_history[m_current][SearchWeight]->GetDefaultView(),
                                        RDGContentGuarantee::eFullWrite);
            }
            filter.BindSampledTexture("inputVariance", m_sampler, previousVariance);
            filter.BindStorageImage("filteredVariance", variance, RDGContentGuarantee::eFullWrite);
            previousVariance = variance;
            filter.BindSampledTexture("inputSky", m_sampler, m_outputSky);
            filter.BindSampledTexture("inputSpecular", m_sampler, m_outputSpecular);
            filter.BindSampledTexture("inputBounce", m_sampler, m_outputBounce);
            filter.BindSampledTexture("bounceHistory", m_sampler, m_history[m_current][Bounce]->GetDefaultView());
            filter.BindSampledTexture("reflectionHistory", m_sampler, m_history[m_current][Reflection]->GetDefaultView());
            filter.BindStorageImage("filteredBounce", bounce, RDGContentGuarantee::eFullWrite);
            m_outputBounce   = bounce;
            m_outputSky      = sky;
            m_outputSpecular = specular;
            filter.BindSampledTexture("moments", m_sampler, m_history[m_current][Moments]->GetDefaultView());
            filter.BindSampledTexture("historyLength", m_sampler, m_history[m_current][Length]->GetDefaultView());
            filter.BindStorageImage("filteredSky", sky, RDGContentGuarantee::eFullWrite);
            filter.BindStorageImage("filteredSpecular", specular, RDGContentGuarantee::eFullWrite);
            Dispatch(std::move(filter));
            if (iteration == 4)
            {
                RDGComputePassDesc search;
                search.SetShaderProgramName("HybridSkySearchSP");
                search.SetPassTag("HybridSkySearch");
                BindGuides(search);
                search.BindSampledTexture("filterPosition", m_sampler, m_history[m_current][Position]->GetDefaultView());
                search.BindSampledTexture("inputSky", m_sampler, sourceSky);
                search.BindSampledTexture("inputVariance", m_sampler, sourceVariance);
                search.BindSampledTexture("skyGuide", m_sampler, skyGuide);
                search.BindStorageImage("filteredSky", sky);
                search.BindStorageImage("filteredVariance", variance);
                search.BindStorageBuffer("HybridSkySearchArgs", searchArgs);
                search.BindStorageBuffer("HybridSkySearchPixels", searchPixels, RDGContentGuarantee::eConsumeProducedElements);
                search.BindSampledTexture("skySupport", m_sampler, skySupport);
                search.BindStorageBuffer("HybridSkySupportBounds", supportBounds);
                search.BindStorageImage("nextSearchWeight", m_history[m_current][SearchWeight]->GetDefaultView());
                search.UseIndirectBuffer(searchArgs);
                graph->AddComputePass(std::move(search)).RecordPassCommands([searchArgs](RDGPassCmdEncoder& encoder) {
                    encoder.DispatchIndirect(searchArgs, sizeof(uint32_t));
                });
            }
        }
        if (m_captureOutput != nullptr && m_captureReadback != nullptr)
        {
            RDGComputePassDesc capture;
            capture.SetShaderProgramName("HybridCaptureSP");
            capture.SetPassTag("HybridCapture");
            BindGuides(capture);
            voxelGI.BindLightingInputs(capture);
            capture.BindValue("uSceneData", m_scene->GetSceneUniformData(), sizeof(SceneUniformData));
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
            Dispatch(std::move(capture));
            graph->AddTransferPass("ReadHybridCapture")
                .CopyBuffer(m_captureOutput, m_captureReadback, {0, 0, m_captureOutput->GetRequiredSize()})
                .NeverCull();
        }
        m_rawSky      = rawSky;
        m_rawSpecular = rawSpecular;
        m_recorded    = true;
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

DebugOutputDescription HybridGIRenderer::BuildDebugView(const RenderView& view, const DebugSelection& selection) const
{
    DebugOutputDescription description;
    description.output = selection.output;
    description.available =
        m_recorded && selection.output >= DebugOutput::eGIRawSky && selection.output <= DebugOutput::eGIRejection;
    description.reason = "Hybrid GI is not active in this view.";
    if (description.available)
    {
        description.reason.clear();
        description.width             = view.width;
        description.height            = view.height;
        description.format            = DataFormat::eR16G16B16A16SFloat;
        const char* interpretations[] = {
            "Raw diffuse sky, linear radiance converted to sRGB; values above one clip.",
            "Reconstructed diffuse sky, linear radiance converted to sRGB; values above one clip.",
            "Diffuse history length, black at zero and white at the configured history limit.",
            "Raw specular escape fraction: black is blocked, white escapes. Grey averages reference samples.",
            "History rejection: green accepted, red reset, yellow outside view, magenta identity, cyan plane/normal."};
        const uint32_t mode        = static_cast<uint32_t>(selection.output) - static_cast<uint32_t>(DebugOutput::eGIRawSky);
        description.interpretation = interpretations[mode];
        DebugVisualizationData data;
        data.selection.x         = mode;
        data.range.y             = static_cast<float>(m_uniforms.sampling.z);
        RDGGraphicsPassDesc pass = MakeDebugVisualizationPass(view, "HybridDebugSP", data);
        if (selection.output == DebugOutput::eGIRawSky || selection.output == DebugOutput::eGIReconstructedSky
            || selection.output == DebugOutput::eGIHitMiss)
        {
            const RDGTexture source = selection.output == DebugOutput::eGIRawSky  ? m_rawSky
                                    : selection.output == DebugOutput::eGIHitMiss ? m_rawSpecular
                                                                                  : m_outputSky;
            pass.BindSampledTexture("sourceImage", m_sampler, source);
        }
        else
        {
            pass.BindSampledTexture("sourceImage", m_sampler, m_history[m_current][Length]->GetDefaultView());
        }
        pass.BindSampledTexture("historyLength", m_sampler, m_history[m_current][Length]->GetDefaultView());
        AddDebugVisualizationPass(*m_device->GetCurrentFrameRDG(), std::move(pass));
    }
    return description;
}

void HybridGIRenderer::OnRenderGraphExecuted(RenderFrameResult result)
{
    const bool succeeded = result == RenderFrameResult::eSubmitted;

    if (m_recorded && succeeded)
    {
        for (uint32_t side = 0; side < 2; ++side)
        {
            std::fill(std::begin(m_historyNeedsClear[side]), std::end(m_historyNeedsClear[side]), false);
        }

        m_current = 1 - m_current;
        m_valid   = true;
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
        m_radianceGeneration              = m_recordedRadianceGeneration;
        m_previousSettings                = m_recordedSettings;
        m_previousRayBounce               = m_uniforms.bounce.x != 0;
        m_previousReflections             = m_uniforms.reflection.x != 0;
        m_previousHardwareMask            = m_uniforms.provider.z;
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
    m_previousVertices = nullptr;
    m_recordedVertices = nullptr;
    for (uint32_t side = 0; side < 2; ++side)
    {
        for (uint32_t texture = 0; texture < Count; ++texture)
        {
            m_device->DestroyTexture(m_history[side][texture]);
            m_history[side][texture] = nullptr;
        }
        m_device->DestroyBuffer(m_guide[side]);
        m_guide[side] = nullptr;
    }
    m_valid                = false;
    m_preparedHistoryValid = false;
    m_extent               = glm::uvec2(0);
}
} // namespace zen::rc
