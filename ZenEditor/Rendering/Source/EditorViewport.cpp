#include "Editor/Rendering/EditorViewport.h"
#include "EditorShaders.h"
#include "Graphics/RenderCore/V2/Renderer/RendererServer.h"
#include "Graphics/RenderCore/V2/Renderer/RendererUtils.h"
#include "Graphics/RenderCore/V2/Renderer/DeferredLightingRenderer.h"
#include "Graphics/RenderCore/V2/ShaderProgram.h"
#include "Platform/ConfigLoader.h"
#include "Utils/Errors.h"
#include <algorithm>
#include <cstring>

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
    m_device(device), m_editorScene(scene), m_selection(selection), m_camera(camera)
{}

bool EditorViewport::Init()
{
    m_defaultEnvironment = platform::ConfigLoader::GetInstance().GetString("environment_texture", "papermill.ktx");

    const platform::ConfigLoader& config = platform::ConfigLoader::GetInstance();

    EditorEnvironment environment;

    if (config.ReadNumber("environment_intensity", environment.intensity)
        && config.ReadNumber("environment_rotation_degrees", environment.rotationDegrees)
        && config.ReadBool("environment_lighting", environment.lighting)
        && config.ReadBool("skybox_visible", environment.skybox) && std::isfinite(environment.intensity)
        && environment.intensity >= 0.0f && std::isfinite(environment.rotationDegrees))
    {
        m_environment = environment;
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
    DiscardScene();

    m_device.GetRendererServer()->SetRenderScene(nullptr);

    if (m_scene)
    {
        m_scene->Destroy();

        m_scene.Reset();
    }

    ReleasePreviews();

    m_device.DestroyTexture(m_view.color);

    m_device.DestroyTexture(m_view.depth);

    m_view = {};

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
    return m_environment;
}

bool EditorViewport::SetEnvironmentTexture(const std::string& path, std::string& error)
{
    error.clear();

    // Allow clearing an override after opening an empty scene, including when its
    // source file has disappeared and would prevent the next scene from loading.
    const bool valid = m_scene ? m_scene->SetEnvironmentTexture(path, error) : path.empty();

    if (valid)
    {
        m_environment.texturePath = path;
    }
    else if (!m_scene)
    {
        error = "Open a scene with renderable geometry first.";
    }

    return valid;
}

bool EditorViewport::SetEnvironmentLighting(float intensity, float rotationDegrees, bool lighting, bool skybox)
{
    const bool valid = m_scene && m_scene->SetEnvironmentLighting(intensity, rotationDegrees, lighting, skybox);

    if (valid)
    {
        m_environment.intensity       = intensity;

        m_environment.rotationDegrees = rotationDegrees;

        m_environment.lighting        = lighting;

        m_environment.skybox          = skybox;
    }

    return valid;
}

bool EditorViewport::PrepareScene(const LoadedScene& candidate, std::string& error)
{
    DiscardScene();

    error      = rc::GetSceneBindlessCapacityError(m_device.GetGPUInfo().bindlessHeapCapacities,
                                                   candidate.scene->GetComponents<sg::Texture>().size(),
                                                   candidate.scene->GetComponents<sg::Sampler>().size());

    bool valid = error.empty() && m_device.PrepareForResourceReconfiguration();

    if (valid && candidate.scene->GetRenderableCount() > 0)
    {
        rc::SceneData data{};

        data.pScene              = candidate.scene.Get();

        data.pVertices           = candidate.vertices.data();

        data.numVertices         = uint32_t(candidate.vertices.size());

        data.pIndices            = candidate.indices.data();

        data.numIndices          = uint32_t(candidate.indices.size());

        data.pCamera             = &m_camera.GetCamera();

        data.envTextureName      = m_defaultEnvironment;

        data.environmentOverride = m_environment.texturePath;

        m_pendingScene           = MakeUnique<rc::RenderScene>(&m_device, data);

        valid                    = m_pendingScene->Init()
             && m_pendingScene->SetEnvironmentLighting(m_environment.intensity, m_environment.rotationDegrees,
                                                       m_environment.lighting, m_environment.skybox);
    }

    // Finish candidate uploads without resetting the active scene's bindings. The
    // progress dialog can render another frame with the old scene before commit.
    valid = valid && m_device.PrepareForResourceReconfiguration();

    if (!valid)
    {
        DiscardScene();

        error = error.empty() ? "Scene GPU preparation failed. The previous scene remains active." : error;
    }

    m_pendingPrepared = valid;

    return valid;
}

bool EditorViewport::CommitScene(std::string& error)
{
    VERIFY_EXPR_MSG(m_pendingPrepared, "EditorViewport::CommitScene requires a successful PrepareScene");

    // Drain the last old-scene frame and reset its bindings at the actual switch.
    // No rendering may occur between this boundary and publishing the new scene.
    const bool valid = m_device.PrepareForSceneReplacement();

    if (valid)
    {
        m_device.GetRendererServer()->SetRenderScene(m_pendingScene.Get());

        if (m_scene)
        {
            m_scene->Destroy();
        }

        m_scene = std::move(m_pendingScene);

        m_appliedLights.clear();

        m_runtimeLightIds.clear();

        if (m_scene)
        {
            m_scene->SetCameraLight(m_cameraLight);

            for (const rc::LightEntry& entry : m_scene->GetLights().GetEntries())
            {
                m_appliedLights.push_back({entry.id, rc::LightOrigin::eGLTF, entry.light});

                m_runtimeLightIds.push_back(entry.id);
            }
        }

        rc::RendererServer& server = *m_device.GetRendererServer();

        rc::DebugSelection debug   = server.GetDebugSelection();

        debug.lightId              = m_runtimeLightIds.empty() ? 0 : m_runtimeLightIds.front();

        server.SetRenderingSelection(server.GetRequestedRenderOption() == rc::RenderOption::eVoxelGI
                                         ? rc::RenderAlgorithm::eVoxelGI
                                         : rc::RenderAlgorithm::ePBR,
                                     debug);

        m_pendingPrepared = false;

        m_pickRequested   = false;

        error.clear();
    }
    else
    {
        error = "Could not prepare the renderer for the scene switch. The previous scene remains active.";
    }

    return valid;
}

void EditorViewport::DiscardScene()
{
    if (m_pendingScene)
    {
        m_device.PrepareForResourceReconfiguration();

        m_pendingScene->Destroy();

        m_pendingScene.Reset();
    }

    m_pendingPrepared = false;
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

bool EditorViewport::Resize(uint32_t width, uint32_t height)
{
    // Quantize splitter motion to eight physical pixels. The frontend letterboxes
    // the image to its actual aspect ratio and uses that same rectangle for input.
    width      = std::min(8192u, (width + 7u) & ~7u);

    height     = std::min(8192u, (height + 7u) & ~7u);

    bool valid = width > 0 && height > 0;

    if (valid && (m_view.width != width || m_view.height != height))
    {
        rc::TextureFormat format;

        format.width      = width;

        format.height     = height;

        format.depth      = 1;

        format.format     = DataFormat::eR8G8B8A8UNORM;

        RHITexture* color = m_device.CreateTextureColorRT(format, {.copyUsage = true}, "EditorSceneColor");

        format.format     = DataFormat::eD32SFloat;

        RHITexture* depth = m_device.CreateTextureDepthStencilRT(format, {}, "EditorSceneDepth");

        valid             = color != nullptr && depth != nullptr;

        if (valid)
        {
            m_device.DestroyTexture(m_view.color);

            m_device.DestroyTexture(m_view.depth);

            m_view = {color, depth, width, height};

            ++m_targetRevision;
        }
        else
        {
            m_device.DestroyTexture(color);

            m_device.DestroyTexture(depth);
        }
    }

    return valid;
}

const rc::RenderView& EditorViewport::GetRenderView() const
{
    return m_view;
}

bool EditorViewport::HasScene() const
{
    return bool(m_scene);
}

const rc::RenderScene* EditorViewport::GetRenderScene() const
{
    return m_scene.Get();
}

EditorRenderSnapshot EditorViewport::GetSnapshot() const
{
    const rc::RendererServer& server = *m_device.GetRendererServer();

    return {m_device.GetGPUMemoryStats(),
            server.GetVoxelGISettings(),
            server.GetRequestedRenderOption(),
            server.GetRenderOption(),
            server.GetRenderingStatus(),
            server.GetEffectiveVoxelizer(),
            server.GetDebugOutputDescription(),
            server.RequestDeferredLightingRenderer()->GetLightMarkersEnabled(),
            server.RequestDeferredLightingRenderer()->GetLightMarkerSize()};
}

bool EditorViewport::ApplyRenderingSettings(const rc::RenderingSettings& settings, bool retryResources, std::string& error)
{
    rc::RendererServer& server = *m_device.GetRendererServer();

    uint64_t reflectanceBytes  = 0;

    bool valid                 = rc::ValidateRenderingSettings(settings, error);

    if (valid
        && (settings.environment.texturePath != m_environment.texturePath
            || (m_scene
                && !m_scene->ValidateEnvironmentLighting(settings.environment.intensity,
                                                         settings.environment.rotationDegrees))))
    {
        error = "Environment texture must finish loading and its intensity must fit the scene's authored environment.";

        valid = false;
    }

    HeapVector<rc::SceneLight> lights;

    HeapVector<rc::SceneLight> appliedLights;

    HeapVector<rc::LightEntry> replacements;

    bool lightsChanged = settings.lights.size() != m_appliedLights.size();

    for (const rc::RenderingLight& light : m_appliedLights)
    {
        appliedLights.push_back(light.light);
    }

    for (size_t index = 0; index < settings.lights.size(); ++index)
    {
        const rc::RenderingLight& light = settings.lights[index];

        lights.push_back(light.light);

        rc::LightId runtimeId = 0;

        for (size_t old = 0; old < m_appliedLights.size(); ++old)
        {
            if (m_appliedLights[old].id == light.id)
            {
                runtimeId = m_runtimeLightIds[old];
            }
        }

        replacements.push_back({runtimeId, light.light});

        lightsChanged |= index >= m_appliedLights.size() || light.id != m_appliedLights[index].id
                      || !rc::EqualSceneLight(light.light, m_appliedLights[index].light);
    }

    const bool shadows = (settings.algorithm == rc::RenderAlgorithm::eVoxelGI && settings.gi.cone.shadows)
                      || settings.debug.output == rc::DebugOutput::eShadow;

    const uint32_t shadowFaces = rc::CountShadowFaces(lights) + rc::CameraLightShadowFaces(settings.cameraLight);

    if (valid && shadows && !server.ValidateShadowResources(settings.gi.shadowMapResolution, shadowFaces))
    {
        error = "Shadow memory preflight rejected the edit. Reduce resolution or the number of shadow-casting lights.";

        valid = false;
    }

    if (valid && server.ValidateVoxelGIResources(settings.gi, reflectanceBytes) != rc::GIResourceStatus::eSuccess)
    {
        error = "GI resource preflight rejected the settings. Check the reflectance budget and device limits.";

        valid = false;
    }

    // Light, environment and output edits arrive every frame during a drag. They skip the GI
    // apply unless its values change or Apply explicitly retries resources.
    if (valid && (retryResources || settings.gi != server.GetVoxelGISettings()))
    {
        valid = server.ApplyVoxelGISettings(settings.gi, retryResources);

        if (!valid)
        {
            error = "Renderer could not apply the settings at this frame boundary.";
        }
    }

    if (valid)
    {
        if (m_scene)
        {
            if (lightsChanged)
            {
                valid = m_scene->ReplaceLights(replacements);

                if (valid)
                {
                    m_appliedLights = settings.lights;

                    m_runtimeLightIds.clear();

                    for (const rc::LightEntry& entry : replacements)
                    {
                        m_runtimeLightIds.push_back(entry.id);
                    }
                }

                // A different shadow array may now fit, so a latched fallback gets one new attempt.
                if (valid && rc::CountShadowFaces(lights) != rc::CountShadowFaces(appliedLights))
                {
                    server.ResetRenderingFailure();
                }
            }

            valid = valid && m_scene->SetCameraLight(settings.cameraLight);

            if (valid && rc::CameraLightShadowFaces(settings.cameraLight) != rc::CameraLightShadowFaces(m_cameraLight))
            {
                server.ResetRenderingFailure();
            }

            valid = valid
                 && m_scene->SetEnvironmentLighting(settings.environment.intensity, settings.environment.rotationDegrees,
                                                    settings.environment.lighting, settings.environment.skybox);
        }

        if (valid)
        {
            m_environment            = settings.environment;

            m_cameraLight            = settings.cameraLight;

            rc::DebugSelection debug = settings.debug;

            debug.lightId            = 0;

            for (size_t index = 0; index < m_appliedLights.size(); ++index)
            {
                if (m_appliedLights[index].id == settings.debug.lightId)
                {
                    debug.lightId = m_runtimeLightIds[index];
                }
            }

            server.SetRenderingSelection(settings.algorithm, debug);

            server.RequestDeferredLightingRenderer()->SetLightMarkers(
                settings.lightMarkers && debug.output == rc::DebugOutput::eFinal, settings.lightMarkerSize);
        }
    }

    return valid;
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
    return m_targetRevision;
}

void EditorViewport::RequestPick(Vec2 normalized, const PickStamp& stamp)
{
    m_pickPosition   = glm::clamp(normalized, Vec2(0), Vec2(0.999999f));

    m_requestedStamp = stamp;

    m_pickRequested  = true;
}

void EditorViewport::BuildPick(rc::RenderGraph& graph)
{
    if (m_scene && m_pickRequested && !m_pickInFlight)
    {
        HeapVector<PickDraw> draws;

        m_pickNodes.clear();

        m_pickNodes.resize(m_scene->GetRenderableNodes().size() + 1);

        for (const sg::Node* node : m_scene->GetRenderableNodes())
        {
            if (node->selectable)
            {
                m_pickNodes[node->GetRenderableIndex() + 1] = {m_editorScene.GetGeneration(), node->GetIndex()};
            }
        }

        for (const rc::SceneMeshDraw& draw : rc::SnapshotSceneDraws(*m_scene))
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

        pass.SetRenderArea(0, 0, m_view.width, m_view.height);

        pass.AddColorOutput(DataFormat::eR32UInt, m_view.width, m_view.height, "EditorObjectIDs");

        pass.AddDepthStencilOutput(DataFormat::eD32SFloat, m_view.width, m_view.height, "EditorPickDepth",
                                   RHIRenderTargetLoadOp::eClear, RHIRenderTargetStoreOp::eStore);

        pass.BindStorageBuffer("NodeBuffer", m_scene->GetNodesDataSSBO());

        pass.BindStorageBuffer("MaterialBuffer", m_scene->GetMaterialsDataSSBO());

        pass.BindStorageBuffer("UVBuffer", m_scene->GetUVBuffer());

        rc::BindSceneTextureArray(pass, m_materialSampler, m_scene->GetSceneTextures(), m_scene->GetSceneSamplers());

        pass.BindValue("uCameraData", m_scene->GetCameraUniformData(), sizeof(sg::CameraUniformData));

        pass.BindVertexBuffer(m_scene->GetVertexBuffer());

        pass.BindIndexBuffer(m_scene->GetIndexBuffer());

        graph.AddGraphicsPass(std::move(pass)).RecordPassCommands([draws = std::move(draws)](rc::RDGPassCmdEncoder& encoder) {
            RecordPick(encoder, draws);
        });

        rc::RDGComputePassDesc read;

        read.SetShaderProgramName("EditorPickReadSP");

        read.SetPassTag("EditorReadPick");

        read.BindSampledTexture("uIds", m_pickSampler, "EditorObjectIDs");

        read.BindStorageBuffer("PickOutput", m_pickOutput, rc::RDGContentGuarantee::eFullWrite);

        const glm::uvec2 pixel(uint32_t(m_pickPosition.x * m_view.width), uint32_t(m_pickPosition.y * m_view.height));

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

        pass.SetRenderArea(0, 0, m_view.width, m_view.height);

        pass.AddColorOutput(m_view.color, RHIRenderTargetLoadOp::eLoad);

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
        uint8_t* bytes = m_pickReadback->Map();

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
