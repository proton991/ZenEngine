#include "Editor/Rendering/EditorViewport.h"
#include "Graphics/RenderCore/V2/ScenePresentation.h"
#include "EditorShaders.h"
#include "Graphics/RenderCore/V2/Renderer/RendererServer.h"
#include "Graphics/RenderCore/V2/Renderer/RendererUtils.h"
#include "Graphics/RenderCore/V2/Renderer/DeferredLightingRenderer.h"
#include "Graphics/RenderCore/V2/ShaderProgram.h"
#include "Platform/ConfigLoader.h"
#include "Utils/Errors.h"
#include <algorithm>
#include <cstring>
#include <functional>
#include <string_view>

namespace zen::editor
{
namespace
{
struct PickDraw
{
    rc::SceneMeshDraw draw;
    uint32_t          id{0};
};

void RecordPick(rc::RDGPassCmdEncoder& encoder, const HeapVector<PickDraw>& draws)
{
    for (const PickDraw& entry : draws)
    {
        const glm::uvec3 constants(entry.draw.nodeIndex, entry.draw.materialIndex, entry.id);

        encoder.SetPushConstants(constants);

        encoder.DrawIndexed(entry.draw.indexCount, 1, entry.draw.firstIndex, 0, 0);
    }
}
} // namespace

EditorViewport::EditorViewport(rc::RenderDevice&      device,
                               const EditorScene&     scene,
                               const EditorSelection& selection,
                               EditorCamera&          camera) :
    m_device(device), m_editorScene(scene), m_selection(selection), m_camera(camera), m_sceneRendering(device), m_target(device)
{}

bool EditorViewport::Init()
{
    const std::string defaultEnvironment =
        platform::ConfigLoader::GetInstance().GetString("environment_texture", "papermill.ktx");

    const platform::ConfigLoader& config = platform::ConfigLoader::GetInstance();

    EditorEnvironment environment;

    if (config.ReadNumber("environment_intensity", environment.intensity)
        && config.ReadNumber("environment_rotation_degrees", environment.rotationDegrees)
        && config.ReadBool("environment_lighting", environment.lighting)
        && config.ReadBool("skybox_visible", environment.skybox) && std::isfinite(environment.intensity)
        && environment.intensity >= 0.0f && std::isfinite(environment.rotationDegrees))
    {
        m_sceneRendering.InitializeEnvironment(defaultEnvironment, environment);
    }
    else
    {
        m_sceneRendering.InitializeEnvironment(defaultEnvironment, {});
    }

    VERIFY_EXPR_MSG(m_device.GetRendererServer() != nullptr, "Initialize RendererServer before the editor scene viewport");

    bool valid = RegisterEditorShader(m_device, "EditorPickSP", "SceneRenderer/offscreen.vert.spv", "Editor/pick.frag.spv")
              && RegisterEditorShader(m_device, "EditorPickReadSP", "Editor/read_pick.comp.spv", nullptr)
              && RegisterEditorShader(m_device, "EditorBoundsSP", "Editor/bounds.vert.spv", "Editor/bounds.frag.spv");

    if (valid)
    {
        RHIBufferCreateInfo info;

        info.size         = sizeof(uint32_t);

        info.allocateType = RHIBufferAllocateType::eGPU;

        info.usageFlags.SetFlags(RHIBufferUsageFlagBits::eStorageBuffer, RHIBufferUsageFlagBits::eTransferSrcBuffer);

        info.tag          = "EditorPickOutput";

        m_pickOutput      = m_device.CreateBuffer(info);

        info.allocateType = RHIBufferAllocateType::eCPURead;

        info.usageFlags.SetFlag(RHIBufferUsageFlagBits::eTransferDstBuffer);

        info.tag                     = "EditorPickReadback";

        m_pickReadback               = m_device.CreateBuffer(info);

        RHISamplerCreateInfo sampler = RHISamplerCreateInfo::CreateLinearRepeat();

        // Use the same cached fallback as the scene renderer: bindless slot zero
        // cannot name a different sampler during an active scene epoch.
        m_materialSampler = m_device.CreateSampler(sampler);

        sampler.minFilter = sampler.magFilter = sampler.mipFilter = RHISamplerFilter::eNearest;

        m_pickSampler                                             = m_device.CreateSampler(sampler);

        sampler                                                   = RHISamplerCreateInfo::CreateLinearRepeat();

        sampler.repeatU = sampler.repeatV = RHISamplerRepeatMode::eClampToEdge;

        m_imageSampler                    = m_device.CreateSampler(sampler);

        m_boundsBuffers.resize(GRenderFrameState.GetNumFramesInFlight());

        info.size         = sizeof(Vec3) * 24;

        info.allocateType = RHIBufferAllocateType::eGPU;

        info.usageFlags.SetFlags(RHIBufferUsageFlagBits::eVertexBuffer, RHIBufferUsageFlagBits::eTransferDstBuffer);

        info.tag = "EditorSelectionBounds";

        for (RHIBuffer*& buffer : m_boundsBuffers)
        {
            buffer = m_device.CreateBuffer(info);

            valid  = valid && buffer != nullptr;
        }

        valid = valid && m_pickOutput != nullptr && m_pickReadback != nullptr && m_pickSampler != nullptr
             && m_materialSampler != nullptr && m_imageSampler != nullptr;
    }

    return valid;
}

void EditorViewport::Destroy()
{
    SetGPUTimingsEnabled(false);
    m_sceneRendering.Destroy();

    ReleasePreviews();

    m_target.Destroy();

    for (RHIBuffer* buffer : m_boundsBuffers)
    {
        m_device.DestroyBuffer(buffer);
    }

    m_boundsBuffers.clear();

    m_device.DestroyBuffer(m_pickOutput);

    m_device.DestroyBuffer(m_pickReadback);

    m_pickOutput = m_pickReadback = nullptr;

    m_pickInFlight = m_pickRequested = m_pickRecorded = false;
}

const EditorEnvironment& EditorViewport::GetEnvironment() const
{
    return m_sceneRendering.GetEnvironment();
}

bool EditorViewport::SetEnvironmentTexture(const std::string& path, std::string& error)
{
    return m_sceneRendering.SetEnvironmentTexture(path, error);
}

bool EditorViewport::SetEnvironmentLighting(float intensity, float rotationDegrees, bool lighting, bool skybox)
{
    return m_sceneRendering.SetEnvironmentLighting(intensity, rotationDegrees, lighting, skybox);
}

bool EditorViewport::PrepareScene(const LoadedScene& candidate, std::string& error)
{
    return m_sceneRendering.PrepareScene(candidate, m_camera.GetCamera(), error);
}

bool EditorViewport::CommitScene(std::string& error)
{
    const bool valid = m_sceneRendering.CommitScene(error);
    if (valid)
    {
        m_pickRequested = false;
    }
    return valid;
}

void EditorViewport::DiscardScene()
{
    m_sceneRendering.DiscardScene();
}

void EditorViewport::RefreshSceneResources()
{
    m_pickRequested = false;

    CreatePreviews();
}

void EditorViewport::ReleasePreviews()
{
    // UI registrations retain their own references until the UI unregisters them.
    for (RHITexture* preview : m_previews)
    {
        m_device.DestroyTexture(preview);
    }

    m_previews.clear();

    m_previewGeneration = 0;
}

void EditorViewport::CreatePreviews()
{
    ReleasePreviews();

    const SceneAssetIndex& assets = m_editorScene.GetAssets();

    m_previewGeneration           = m_editorScene.GetGeneration();

    for (const SceneAssetItem& item : assets.Query(""))
    {
        const sg::Texture* source = item.id.kind == SceneAssetKind::Texture ? assets.ResolveTexture(item.id) : nullptr;

        TexturePreview preview;

        if (source != nullptr)
        {
            RHITexture* texture = nullptr;

            if (MakeTexturePreview(*source, 128, preview))
            {
                rc::TextureFormat format;

                format.width  = preview.width;

                format.height = preview.height;

                format.depth  = 1;

                format.format = DataFormat::eR8G8B8A8UNORM;

                texture       = m_device.CreateTextureSampled(format, {.copyUsage = true}, "EditorAssetPreview");
            }

            if (texture != nullptr)
            {
                RHIBufferTextureCopyRegion region{};

                region.textureSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);

                region.textureSubresources.layerCount = 1;

                region.textureSize                    = {preview.width, preview.height, 1};

                m_device.UpdateTexture(texture, {&region, 1}, uint32_t(preview.pixels.size()), preview.pixels.data());
            }

            m_previews.resize(std::max<size_t>(m_previews.size(), item.id.index + 1));

            m_previews[item.id.index] = texture;
        }
    }
}

RHITexture* EditorViewport::GetAssetPreview(SceneAssetId id) const
{
    const bool current = id.kind == SceneAssetKind::Texture && id.generation == m_previewGeneration && id.generation != 0
                      && id.index < m_previews.size();

    return current ? m_previews[id.index] : nullptr;
}

bool EditorViewport::Resize(uint32_t width, uint32_t height, bool quantize)
{
    // Quantize splitter motion to eight physical pixels. The frontend letterboxes
    // the image to its actual aspect ratio and uses that same rectangle for input.
    if (quantize)
    {
        width  = std::min(8192u, (width + 7u) & ~7u);
        height = std::min(8192u, (height + 7u) & ~7u);
    }

    return m_target.Resize(width, height);
}

const rc::RenderView& EditorViewport::GetRenderView() const
{
    return m_target.GetView();
}

bool EditorViewport::HasScene() const
{
    return m_sceneRendering.GetScene() != nullptr;
}

const rc::RenderScene* EditorViewport::GetRenderScene() const
{
    return m_sceneRendering.GetScene();
}

EditorRenderSnapshot EditorViewport::GetSnapshot() const
{
    const rc::RendererServer& server = *m_device.GetRendererServer();

    EditorRenderSnapshot snapshot{m_device.GetGPUMemoryStats(),
                                  server.GetVoxelGISettings(),
                                  server.GetRequestedRenderOption(),
                                  server.GetRenderOption(),
                                  server.GetRenderingStatus(),
                                  server.GetEffectiveVoxelizer(),
                                  server.GetDebugOutputDescription(),
                                  server.RequestDeferredLightingRenderer()->GetLightMarkersEnabled(),
                                  server.RequestDeferredLightingRenderer()->GetLightMarkerSize(),
                                  server.RequestVoxelGI() != nullptr && server.RequestVoxelGI()->UsesHardwareQueries(),
                                  server.RequestVoxelGI() != nullptr ? server.RequestVoxelGI()->GetProviderReason()
                                                                     : "GI inactive"};
    snapshot.giSelectionReason = server.GetGISelectionReason();
    snapshot.preparingGI       = server.IsPreparingGI();
    snapshot.gpuTimingsEnabled = m_gpuTimingsEnabled;
    if (m_gpuTimingsEnabled)
    {
        snapshot.giPassTimings = m_giPassTimings;
    }
    return snapshot;
}

void EditorViewport::UpdateGPUTimings(const rc::RDGMetricsSnapshot& timings)
{
    // Upload, preview and readback graphs can complete after the scene graph.
    // Keep the last scene measurement instead of replacing it with unrelated work.
    if (timings.graph == "frame_rdg")
    {
        m_giPassTimings.clear();
        for (const rc::RDGNodeMetrics& node : timings.nodes)
        {
            const std::string_view name(node.name.CStr());
            if (name.starts_with("Hybrid") || name.starts_with("Voxel") || name == "BuildSceneBLAS" || name == "UpdateSceneBLAS"
                || name == "BuildSceneTLAS")
            {
                m_giPassTimings.push_back(node);
            }
        }
    }
}

void EditorViewport::SetGPUTimingsEnabled(bool enabled)
{
    if (enabled != m_gpuTimingsEnabled)
    {
        rc::RDGMetrics& metrics = m_device.GetRDGMetrics();
        if (enabled)
        {
            m_previousMetricsOptions      = metrics.GetOptions();
            rc::RDGMetricsOptions options = m_previousMetricsOptions;
            options.logging.enabled       = true;
            options.logging.sampleEvery   = 30;
            options.logging.minInterval   = std::chrono::milliseconds(500);
            options.gpuTimings            = true;
            options.maxNodeDetails        = std::max(options.maxNodeDetails, 512u);
            metrics.Configure(options);
            metrics.SetGPUSink(std::bind_front(&EditorViewport::UpdateGPUTimings, this));
        }
        else
        {
            metrics.SetGPUSink({});
            metrics.Configure(m_previousMetricsOptions);
            m_giPassTimings.clear();
        }
        m_gpuTimingsEnabled = enabled;
    }
}

bool EditorViewport::ApplyRenderingSettings(const rc::RenderingSettings& settings, bool retryResources, std::string& error)
{
    return m_sceneRendering.ApplyRenderingSettings(settings, retryResources, error);
}

void EditorViewport::SetRenderMode(rc::RenderOption mode)
{
    m_device.GetRendererServer()->SetRenderOption(mode);
}

RHISampler* EditorViewport::GetImageSampler() const
{
    return m_imageSampler;
}

uint64_t EditorViewport::GetTargetRevision() const
{
    return m_target.GetRevision();
}

void EditorViewport::RequestPick(Vec2 normalized, const PickStamp& stamp)
{
    m_pickPosition   = glm::clamp(normalized, Vec2(0), Vec2(0.999999f));

    m_requestedStamp = stamp;

    m_pickRequested  = true;
}

void EditorViewport::BuildPick(rc::RenderGraph& graph)
{
    // Keep a queued request until the older readback is discarded, even if its view
    // has changed. Clearing it sooner would let that superseded result apply again.
    if (m_pickRequested && !m_pickInFlight
        && m_requestedStamp != MakePickStamp(m_editorScene, m_camera, m_selection, GetTargetRevision()))
    {
        m_pickRequested = false;
    }

    if (m_sceneRendering.GetScene() && m_pickRequested && !m_pickInFlight)
    {
        HeapVector<PickDraw> draws;

        m_pickNodes.clear();

        m_pickNodes.resize(m_sceneRendering.GetScene()->GetRenderableNodes().size() + 1);

        for (const sg::Node* node : m_sceneRendering.GetScene()->GetRenderableNodes())
        {
            if (node->selectable)
            {
                m_pickNodes[node->GetRenderableIndex() + 1] = {m_editorScene.GetGeneration(), node->GetIndex()};
            }
        }

        for (const rc::SceneMeshDraw& draw : rc::SnapshotSceneDraws(*m_sceneRendering.GetScene()))
        {
            draws.push_back({draw, m_pickNodes[draw.nodeIndex + 1].generation != 0 ? draw.nodeIndex + 1 : 0});
        }

        RHIGfxPipelineStates states{};

        states.rasterizationState.cullMode = RHIPolygonCullMode::eDisabled;

        states.depthStencilState = RHIGfxPipelineDepthStencilState::Create(true, true, RHIDepthCompareOperator::eLess);

        states.colorBlendState.AddAttachments(1);

        rc::RDGGraphicsPassDesc pass;

        pass.SetShaderProgramName("EditorPickSP");

        pass.SetPassTag("EditorObjectIDs");

        pass.SetPipelineStates(states);

        pass.SetRenderArea(0, 0, m_target.GetView().width, m_target.GetView().height);

        pass.AddColorOutput(DataFormat::eR32UInt, m_target.GetView().width, m_target.GetView().height, "EditorObjectIDs");

        pass.AddDepthStencilOutput(DataFormat::eD32SFloat, m_target.GetView().width, m_target.GetView().height,
                                   "EditorPickDepth", RHIRenderTargetLoadOp::eClear, RHIRenderTargetStoreOp::eStore);

        pass.BindStorageBuffer("NodeBuffer", m_sceneRendering.GetScene()->GetNodesDataSSBO());

        pass.BindStorageBuffer("MaterialBuffer", m_sceneRendering.GetScene()->GetMaterialsDataSSBO());

        pass.BindStorageBuffer("UVBuffer", m_sceneRendering.GetScene()->GetUVBuffer());

        rc::BindSceneTextureArray(pass, m_materialSampler, m_sceneRendering.GetScene()->GetSceneTextureViews(),
                                  m_sceneRendering.GetScene()->GetSceneSamplers());

        pass.BindValue("uCameraData", m_sceneRendering.GetScene()->GetCameraUniformData(), sizeof(sg::CameraUniformData));

        pass.BindVertexBuffer(m_sceneRendering.GetScene()->GetVertexBuffer());

        pass.BindIndexBuffer(m_sceneRendering.GetScene()->GetIndexBuffer());

        graph.AddGraphicsPass(std::move(pass)).RecordPassCommands([draws = std::move(draws)](rc::RDGPassCmdEncoder& encoder) {
            RecordPick(encoder, draws);
        });

        rc::RDGComputePassDesc read;

        read.SetShaderProgramName("EditorPickReadSP");

        read.SetPassTag("EditorReadPick");

        read.BindSampledTexture("uIds", m_pickSampler, "EditorObjectIDs");

        read.BindStorageBuffer("PickOutput", m_pickOutput, rc::RDGContentGuarantee::eFullWrite);

        const glm::uvec2 pixel(uint32_t(m_pickPosition.x * m_target.GetView().width),
                               uint32_t(m_pickPosition.y * m_target.GetView().height));

        graph.AddComputePass(std::move(read)).RecordPassCommands([pixel](rc::RDGPassCmdEncoder& encoder) {
            encoder.SetPushConstants(pixel);

            encoder.Dispatch(1, 1, 1);
        });

        graph.AddTransferPass("EditorPickReadback")
            .CopyBuffer(m_pickOutput, m_pickReadback, {0, 0, sizeof(uint32_t)})
            .NeverCull();

        m_recordedStamp = m_requestedStamp;

        m_pickRecorded  = true;

        m_pickRequested = false;
    }
}

void EditorViewport::BuildBounds(rc::RenderGraph& graph)
{
    sg::AABB bounds;

    if (m_editorScene.GetBounds(m_selection.GetNode(), bounds))
    {
        Vec3 corners[8];

        for (uint32_t index = 0; index < 8; ++index)
        {
            corners[index] =
                Vec3((index & 1) ? bounds.GetMax().x : bounds.GetMin().x, (index & 2) ? bounds.GetMax().y : bounds.GetMin().y,
                     (index & 4) ? bounds.GetMax().z : bounds.GetMin().z);
        }

        Vec3 vertices[24];

        uint32_t count = 0;

        for (uint32_t index = 0; index < 8; ++index)
        {
            for (uint32_t axis = 0; axis < 3; ++axis)
            {
                if ((index & (1u << axis)) == 0)
                {
                    vertices[count++] = corners[index];

                    vertices[count++] = corners[index | (1u << axis)];
                }
            }
        }

        RHIBuffer* buffer = m_boundsBuffers[rc::ToIndex(GRenderFrameState.GetFrameSlot())];

        m_device.UpdateBuffer(buffer, sizeof(vertices), reinterpret_cast<const uint8_t*>(vertices));

        RHIGfxPipelineStates states{};

        states.primitiveType               = RHIDrawPrimitiveType::eLineList;

        states.rasterizationState.cullMode = RHIPolygonCullMode::eDisabled;

        states.depthStencilState = RHIGfxPipelineDepthStencilState::Create(false, false, RHIDepthCompareOperator::eAlways);

        states.colorBlendState.AddAttachments(1);

        rc::RDGGraphicsPassDesc pass;

        pass.SetShaderProgramName("EditorBoundsSP");

        pass.SetPassTag("EditorSelectionBounds");

        pass.SetPipelineStates(states);

        pass.SetRenderArea(0, 0, m_target.GetView().width, m_target.GetView().height);

        pass.AddColorOutput(m_target.GetView().color, RHIRenderTargetLoadOp::eLoad);

        pass.BindVertexBuffer(buffer);

        const Mat4 projection = m_camera.GetCamera().GetProjectionMatrix() * m_camera.GetCamera().GetViewMatrix();

        graph.AddGraphicsPass(std::move(pass)).RecordPassCommands([projection](rc::RDGPassCmdEncoder& encoder) {
            encoder.SetPushConstants(projection);

            encoder.Draw(24, 1);
        });
    }
}

void EditorViewport::BuildOverlays(rc::RenderGraph& graph)
{
    BuildPick(graph);

    if (m_device.GetRendererServer()->GetDebugSelection().output == rc::DebugOutput::eFinal)
    {
        BuildBounds(graph);
    }
}

void EditorViewport::OnSubmitted(bool succeeded)
{
    m_target.OnSubmitted(succeeded);
    if (m_gpuTimingsEnabled)
    {
        m_device.GetRDGMetrics().CollectGPUResults();
    }
    if (m_pickRecorded)
    {
        m_pickInFlight = succeeded;

        m_pickRecorded = false;

        if (succeeded)
        {
            m_pickRetirement = m_device.CaptureResourceRetirement();
        }
    }
}

bool EditorViewport::TakePickResult(PickResult& result)
{
    bool available = false;

    if (m_pickInFlight && m_pickRetirement.IsCompleteAt(m_device.GetCachedCompletedSerials()))
    {
        // A newer click supersedes this readback even when the selection stamp is unchanged.
        uint8_t* bytes = !m_pickRequested ? m_pickReadback->Map() : nullptr;

        if (bytes != nullptr)
        {
            uint32_t id = 0;

            std::memcpy(&id, bytes, sizeof(id));

            m_pickReadback->Unmap();

            result    = {id < m_pickNodes.size() ? m_pickNodes[id] : NodeId{}, m_recordedStamp};

            available = true;
        }

        m_pickInFlight = false;
    }

    return available;
}

bool EditorViewport::IsPickPending() const
{
    return m_pickInFlight || m_pickRequested;
}
} // namespace zen::editor
