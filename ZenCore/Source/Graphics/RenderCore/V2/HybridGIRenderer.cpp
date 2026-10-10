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
const char* const kHistoryNames[]   = {"Sky", "Specular", "Moments", "Length", "Position", "Normal", "Receiver"};
const DataFormat  kHistoryFormats[] = {DataFormat::eR32G32B32A32SFloat, DataFormat::eR16G16B16A16SFloat,
                                       DataFormat::eR32G32B32A32SFloat, DataFormat::eR16G16B16A16SFloat,
                                       DataFormat::eR32G32B32A32SFloat, DataFormat::eR16G16B16A16SFloat,
                                       DataFormat::eR32G32UInt};
// The trace pass learns and reads the visibility guide per workgroup of this many pixels squared.
constexpr uint32_t kGuideTile = 16;
} // namespace

HybridGIRenderer::HybridGIRenderer(RenderDevice* device) : m_device(device)
{
    RHISamplerCreateInfo info{};
    m_sampler = m_device->CreateSampler(info);
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
        m_resetReason = "scene";
    }
    const sg::CameraUniformData& camera = *reinterpret_cast<const sg::CameraUniformData*>(scene.GetCameraUniformData());
    m_recordedProjection                = camera.proj;
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
}

HybridReceiverDraw HybridGIRenderer::ReceiverDraw(uint32_t node, uint32_t material) const
{
    HybridReceiverDraw result;
    result.indices       = glm::uvec4(node, material, 0, 0);
    result.previousModel = m_valid && node < m_previousModels.size() ? m_previousModels[node] : m_recordedModels[node];
    return result;
}

bool HybridGIRenderer::PrepareHistory(uint32_t width, uint32_t height)
{
    if (m_extent != glm::uvec2(width, height))
    {
        Destroy();
        m_extent      = glm::uvec2(width, height);
        m_resetReason = "resize";
    }
    bool valid = true;
    for (uint32_t side = 0; side < 2 && valid; ++side)
    {
        for (uint32_t texture = 0; texture < Count && valid; ++texture)
        {
            if (m_history[side][texture] == nullptr)
            {
                TextureFormat format;
                format.dimension         = TextureDimension::e2D;
                format.depth             = 1;
                format.width             = width;
                format.height            = height;
                format.format            = kHistoryFormats[texture];
                m_history[side][texture] = m_device->CreateTextureStorage(
                    format, {.copyUsage = true}, NameID(fmt::format("hybrid_history_{}_{}", side, texture)));
                valid = m_history[side][texture] != nullptr;
                if (valid)
                {
                    // Even a disabled history tap needs a defined descriptor/layout.
                    m_device->GetCurrentFrameRDG()
                        ->AddTransferPass("InitializeHybridHistory")
                        .ClearTexture(m_history[side][texture], Color(0));
                }
            }
        }
    }
    const glm::uvec2 tiles = (glm::uvec2(width, height) + glm::uvec2(kGuideTile - 1)) / kGuideTile;
    for (uint32_t side = 0; side < 2 && valid; ++side)
    {
        if (m_guide[side] == nullptr)
        {
            // Per tile: the environment-tile weights and the number of frames they average.
            m_guide[side] = m_device->CreateStorageBuffer(tiles.x * tiles.y * (ZEN_ENVIRONMENT_TILES + 1u) * sizeof(float),
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

void HybridGIRenderer::Dispatch(RDGComputePassDesc&& pass)
{
    const glm::uvec2 groups = (m_extent + glm::uvec2(7)) / 8u;
    m_device->GetCurrentFrameRDG()->AddComputePass(std::move(pass)).RecordPassCommands([groups](RDGPassCmdEncoder& encoder) {
        encoder.Dispatch(groups.x, groups.y, 1);
    });
}

bool HybridGIRenderer::BuildRenderGraph(const RenderView& view, VoxelGIRenderer& voxelGI)
{
    const VoxelGISettings& settings = voxelGI.GetSettings();
    const bool             ready =
        m_prepared && settings.rayProvider != VoxelGISettings::RayProvider::Legacy && PrepareHistory(view.width, view.height);
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
                 || settings.normalBiasVoxels != m_previousSettings.normalBiasVoxels
                 || settings.temporal != m_previousSettings.temporal
                 || settings.historyFrames != m_previousSettings.historyFrames
                 || settings.specularOcclusion != m_previousSettings.specularOcclusion)
        {
            InvalidateHistory("settings");
        }
        if (m_previousHardware != voxelGI.UsesHardwareQueries())
        {
            InvalidateHistory("provider_changed");
        }
        m_previousHardware  = voxelGI.UsesHardwareQueries();
        m_previousSettings  = settings;
        m_uniforms.sampling = glm::uvec4(m_frame, settings.referenceSamples != 0 ? settings.referenceSamples : settings.samples,
                                         settings.referenceSamples != 0 ? 256u : settings.historyFrames, m_valid ? 1 : 0);
        // A guide from a successful frame can survive geometry, opacity, settings and provider resets.
        // A reset reason alone cannot make a fresh or failed-frame guide safe to read.
        const std::string_view reason = m_resetReason;
        const bool             guideValid =
            hadValidHistory
            && (m_valid || reason == "geometry_or_opacity" || reason == "settings" || reason == "provider_changed");
        m_uniforms.provider           = glm::uvec4(m_previousHardware ? 1u : 0u, guideValid ? 1u : 0u, 0, 0);
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
        desc.name                      = "hybrid_raw_sky";
        const RDGTexture rawSky        = resources->CreateTexture(desc);
        desc.name                      = "hybrid_raw_specular";
        const RDGTexture   rawSpecular = resources->CreateTexture(desc);
        RDGComputePassDesc trace;
        trace.SetShaderProgramName(m_previousHardware ? "HybridTraceHardwareSP" : "HybridTraceSP");
        trace.SetPassTag("HybridTrace");
        BindGuides(trace);
        voxelGI.BindEnvironmentSamplingInputs(trace);
        if (m_previousHardware)
        {
            voxelGI.BindHardwareRayInputs(trace);
        }
        else
        {
            voxelGI.BindRayInputs(trace);
        }
        trace.BindValue("uSceneData", m_scene->GetSceneUniformData(), sizeof(SceneUniformData));
        trace.BindStorageImage("rawSky", rawSky, RDGContentGuarantee::eFullWrite);
        trace.BindStorageImage("rawSpecular", rawSpecular, RDGContentGuarantee::eFullWrite);
        trace.BindStorageBuffer("HybridGuidePrevious", m_guide[1 - m_current]);
        trace.BindStorageBuffer("HybridGuideNext", m_guide[m_current], RDGContentGuarantee::eFullWrite);
        const glm::uvec2 guideGroups = (m_extent + glm::uvec2(kGuideTile - 1)) / kGuideTile;
        graph->AddComputePass(std::move(trace)).RecordPassCommands([guideGroups](RDGPassCmdEncoder& encoder) {
            encoder.Dispatch(guideGroups.x, guideGroups.y, 1);
        });

        desc.name                   = "hybrid_sky_guide";
        const RDGTexture   skyGuide = resources->CreateTexture(desc);
        RDGComputePassDesc temporal;
        temporal.SetShaderProgramName("HybridTemporalSP");
        temporal.SetPassTag("HybridTemporal");
        temporal.allowCulling = false;
        BindGuides(temporal);
        temporal.BindValue("uSceneData", m_scene->GetSceneUniformData(), sizeof(SceneUniformData));
        temporal.BindSampledTexture("rawSky", m_sampler, rawSky);
        temporal.BindSampledTexture("rawSpecular", m_sampler, rawSpecular);
        voxelGI.BindEnvironmentHarmonics(temporal);
        temporal.BindStorageImage("skyGuide", skyGuide, RDGContentGuarantee::eFullWrite);
        for (uint32_t texture = 0; texture < Count; ++texture)
        {
            temporal.BindSampledTexture(NameID(fmt::format("previous{}", kHistoryNames[texture])), m_sampler,
                                        m_history[1 - m_current][texture]->GetDefaultView());
            temporal.BindStorageImage(NameID(fmt::format("next{}", kHistoryNames[texture])),
                                      m_history[m_current][texture]->GetDefaultView(), RDGContentGuarantee::eFullWrite);
        }
        Dispatch(std::move(temporal));

        m_outputSky                     = resources->ImportTexture(m_history[m_current][Sky]);
        m_outputSpecular                = resources->ImportTexture(m_history[m_current][Specular]);
        RDGTexture     previousVariance = resources->ImportTexture(m_history[m_current][Moments]);
        const uint32_t iterations       = settings.filter && settings.referenceSamples == 0 ? 5u : 0u;
        for (uint32_t iteration = 0; iteration < iterations; ++iteration)
        {
            desc.name                 = NameID(fmt::format("hybrid_sky_{}", iteration));
            const RDGTexture sky      = resources->CreateTexture(desc);
            desc.name                 = NameID(fmt::format("hybrid_specular_{}", iteration));
            const RDGTexture specular = resources->CreateTexture(desc);
            desc.name                 = NameID(fmt::format("hybrid_variance_{}", iteration));
            desc.texFormat.format     = DataFormat::eR32G32SFloat;
            const RDGTexture variance = resources->CreateTexture(desc);
            desc.texFormat.format     = DataFormat::eR16G16B16A16SFloat;
            RDGComputePassDesc filter;
            filter.SetShaderProgramName("HybridFilterSP");
            filter.SetPassTag(NameID(fmt::format("HybridFilter{}", iteration)));
            BindGuides(filter);
            voxelGI.BindRayInputs(filter);
            filter.BindValue("uSceneData", m_scene->GetSceneUniformData(), sizeof(SceneUniformData));
            filter.BindSampledTexture("skyGuide", m_sampler, skyGuide);
            filter.BindSampledTexture("inputVariance", m_sampler, previousVariance);
            filter.BindStorageImage("filteredVariance", variance, RDGContentGuarantee::eFullWrite);
            previousVariance = variance;
            filter.BindSampledTexture("inputSky", m_sampler, m_outputSky);
            filter.BindSampledTexture("inputSpecular", m_sampler, m_outputSpecular);
            m_outputSky      = sky;
            m_outputSpecular = specular;
            filter.BindSampledTexture("moments", m_sampler, m_history[m_current][Moments]->GetDefaultView());
            filter.BindSampledTexture("historyLength", m_sampler, m_history[m_current][Length]->GetDefaultView());
            filter.BindStorageImage("filteredSky", sky, RDGContentGuarantee::eFullWrite);
            filter.BindStorageImage("filteredSpecular", specular, RDGContentGuarantee::eFullWrite);
            const glm::uvec2 groups = (m_extent + glm::uvec2(7)) / 8u;
            graph->AddComputePass(std::move(filter)).RecordPassCommands([groups, iteration](RDGPassCmdEncoder& encoder) {
                encoder.SetPushConstants(1u << iteration);
                encoder.Dispatch(groups.x, groups.y, 1);
            });
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
            capture.BindSampledTexture("reconstructedSky", m_sampler, m_outputSky);
            capture.BindSampledTexture("reconstructedSpecular", m_sampler, m_outputSpecular);
            capture.BindSampledTexture("moments", m_sampler, m_history[m_current][Moments]->GetDefaultView());
            capture.BindSampledTexture("historyLength", m_sampler, m_history[m_current][Length]->GetDefaultView());
            capture.BindStorageBuffer("HybridCapture", m_captureOutput, RDGContentGuarantee::eFullWrite);
            Dispatch(std::move(capture));
            graph->AddTransferPass("ReadHybridCapture")
                .CopyBuffer(m_captureOutput, m_captureReadback, {0, 0, m_captureOutput->GetRequiredSize()})
                .NeverCull();
        }
        m_recorded = true;
    }
    return m_recorded;
}

void HybridGIRenderer::BindLightingInputs(RDGPassDescBase& pass) const
{
    pass.BindSampledTexture("hybridSurface", m_sampler, "offscreen_receiver");
    pass.BindSampledTexture("hybridDepth", m_sampler, "offscreen_depth");
    pass.BindSampledTexture("hybridSky", m_sampler, m_outputSky);
    pass.BindSampledTexture("hybridSpecular", m_sampler, m_outputSpecular);
}

void HybridGIRenderer::OnRenderGraphExecuted(bool succeeded)
{
    if (m_recorded && succeeded)
    {
        m_current = 1 - m_current;
        m_valid   = true;
        ++m_frame;
        m_previousModels                  = m_recordedModels;
        m_previousProjection              = m_recordedProjection;
        m_uniforms.previousProjectionView = m_recordedProjectionView;
        m_uniforms.previousViewPosition   = m_recordedViewPosition;
        m_uniforms.previousViewDirection  = m_recordedViewDirection;
        m_geometry                        = m_scene->GetGeometryRevision();
        m_environment                     = m_scene->GetEnvironmentRevision();
    }
    else
    {
        if (!succeeded)
        {
            Destroy();
        }
        m_valid       = false;
        m_resetReason = succeeded ? "inactive" : "failed_frame";
    }
    m_recorded = false;
    m_prepared = false;
}

void HybridGIRenderer::Destroy()
{
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
    m_valid  = false;
    m_extent = glm::uvec2(0);
}
} // namespace zen::rc
