#include "Graphics/RenderCore/V2/Renderer/DeferredLightingRenderer.h"
#include "Graphics/RenderCore/V2/Renderer/VoxelGIRenderer.h"
#include "Graphics/RenderCore/V2/Renderer/SceneShadowRenderer.h"
#include "Graphics/RenderCore/V2/Renderer/RendererUtils.h"
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

namespace zen::rc
{
namespace
{
void RecordGBufferDraws(RDGPassCmdEncoder& encoder, const HeapVector<SceneMeshDraw>& draws)
{
    GBufferSP::PushConstantsData constants{};

    for (SceneMeshDraw const& draw : draws)
    {
        constants.nodeIndex     = draw.nodeIndex;

        constants.materialIndex = draw.materialIndex;

        encoder.SetPushConstants(constants);

        encoder.DrawIndexed(draw.indexCount, 1, draw.firstIndex, 0, 0);
    }
}

struct ForwardPushConstants
{
    uint32_t nodeIndex;
    uint32_t materialIndex;
    uint32_t topology;
};

void RecordForwardDraws(RDGPassCmdEncoder& encoder, const HeapVector<SceneMeshDraw>& draws, bool displayLinear = false)
{
    for (const SceneMeshDraw& draw : draws)
    {
        const ForwardPushConstants constants{draw.nodeIndex, draw.materialIndex,
                                             static_cast<uint32_t>(draw.topology) | (displayLinear ? 4u : 0u)};

        encoder.SetPushConstants(constants);

        encoder.DrawIndexed(draw.indexCount, 1, draw.firstIndex, 0, 0);
    }
}

struct ForwardDrawDepth
{
    SceneMeshDraw draw;
    float         depth;
};

bool SortForwardDepth(const ForwardDrawDepth& left, const ForwardDrawDepth& right)
{
    return left.depth < right.depth;
}

bool IsTranslucent(const sg::MaterialData& material)
{
    return material.surfaceProperties.y == static_cast<float>(sg::AlphaMode::Blend) || material.sheenColorTransmission.w > 0.0f;
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

DeferredLightingRenderer::DeferredLightingRenderer(RenderDevice* pRenderDevice, RHIViewport* pViewport) :
    m_pRenderDevice(pRenderDevice), m_pViewport(pViewport)
{}

void DeferredLightingRenderer::Init()
{
    PrepareSamplers();

    const platform::ConfigLoader& config = platform::ConfigLoader::GetInstance();

    bool markersEnabled                  = false;

    float markerSize                     = 0.02f;

    const bool valid                     = config.ReadBool("light_markers.enabled", markersEnabled)
                    && config.ReadNumber("light_markers.size", markerSize) && std::isfinite(markerSize) && markerSize > 0.0f;

    SetLightMarkers(valid && markersEnabled, valid ? markerSize : 0.02f);

    if (!valid)
    {
        LOGW("Invalid light_markers configuration; markers disabled");
    }
}

void DeferredLightingRenderer::Destroy() {}

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

bool DeferredLightingRenderer::BuildLightingCaptureClear()
{
    const uint64_t pixels = uint64_t(m_pViewport->GetWidth()) * m_pViewport->GetHeight();

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

void DeferredLightingRenderer::BuildRenderGraph(VoxelGIRenderer* voxelGI, SceneShadowRenderer* shadows)
{
    BuildGBufferGraph();

    BuildCompositionGraph(voxelGI, shadows);
}

void DeferredLightingRenderer::BuildGBufferGraph()
{
    RenderGraph* pRDG = m_pRenderDevice->GetCurrentFrameRDG();

    VERIFY_EXPR(pRDG != nullptr && m_pScene != nullptr);

    const uint32_t width  = m_pViewport->GetWidth();

    const uint32_t height = m_pViewport->GetHeight();

    // One texel per screen pixel: the lighting pass reads the G-buffer at its own fragment
    // coordinate. A suspended (zero-sized) viewport renders nothing and declares no G-buffer.
    const bool declared = width != 0 && height != 0 && !UsesForwardMaterials();

    m_gbufferExtent     = declared ? glm::uvec2(width, height) : glm::uvec2(0, 0);

    if (declared)
    {
        RHIGfxPipelineStates pso{};

        pso.rasterizationState          = {};

        pso.rasterizationState.cullMode = RHIPolygonCullMode::eDisabled;

        pso.depthStencilState           = RHIGfxPipelineDepthStencilState::Create(true, true, RHIDepthCompareOperator::eLess);

        pso.multiSampleState            = {};

        pso.colorBlendState.AddAttachments(5);

        pso.dynamicStates.Enable(RHIDynamicState::eScissor, RHIDynamicState::eViewPort);

        RDGGraphicsPassDesc offscreen{};

        offscreen.SetShaderProgramName("GBufferSP");

        offscreen.AddColorOutput(DataFormat::eR16G16B16A16SFloat, width, height, "offscreen_position");

        offscreen.AddColorOutput(DataFormat::eR16G16B16A16SFloat, width, height, "offscreen_normal");

        offscreen.AddColorOutput(DataFormat::eR8G8B8A8UNORM, width, height, "offscreen_albedo");

        offscreen.AddColorOutput(DataFormat::eR8G8B8A8UNORM, width, height, "offscreen_roughness");

        offscreen.AddColorOutput(DataFormat::eR16G16B16A16SFloat, width, height, "offscreen_emissive_occlusion");

        offscreen.AddDepthStencilOutput(m_pViewport->GetDepthStencilFormat(), width, height, "offscreen_depth",
                                        RHIRenderTargetLoadOp::eClear, RHIRenderTargetStoreOp::eStore);

        offscreen.SetPipelineStates(pso);

        offscreen.SetRenderArea(0, 0, width, height);

        offscreen.SetPassTag("OffScreen");

        offscreen.BindStorageBuffer("NodeBuffer", m_pScene->GetNodesDataSSBO());

        offscreen.BindStorageBuffer("MaterialBuffer", m_pScene->GetMaterialsDataSSBO());

        offscreen.BindStorageBuffer("UVBuffer", m_pScene->GetUVBuffer());

        BindSceneTextureArray(offscreen, m_pColorSampler, m_pScene->GetSceneTextures(), m_pScene->GetSceneSamplers());

        offscreen.BindValue("uCameraData", m_pScene->GetCameraUniformData(), sizeof(sg::CameraUniformData));

        offscreen.BindVertexBuffer(m_pScene->GetVertexBuffer());

        offscreen.BindIndexBuffer(m_pScene->GetIndexBuffer());

        pRDG->AddGraphicsPass(std::move(offscreen))
            .RecordPassCommands(
                [draws = SnapshotSceneDraws(*m_pScene)](RDGPassCmdEncoder& encoder) { RecordGBufferDraws(encoder, draws); });
    }
}

void DeferredLightingRenderer::BuildCompositionGraph(VoxelGIRenderer* voxelGI, SceneShadowRenderer* shadows)
{
    if (UsesForwardMaterials())
    {
        BuildForwardGraph(voxelGI, shadows);
    }
    else
    {
        RenderGraph* pRDG = m_pRenderDevice->GetCurrentFrameRDG();

        VERIFY_EXPR(pRDG != nullptr && m_pScene != nullptr);

        m_captureRecorded       = false;

        const bool capture      = m_captureOutput != nullptr;

        const bool captureValid = !capture || BuildLightingCaptureClear();

        const glm::uvec2 captureExtent(m_pViewport->GetWidth(), m_pViewport->GetHeight());

        {
            RHIGfxPipelineStates pso{};

            pso.primitiveType      = RHIDrawPrimitiveType::eTriangleList;

            pso.rasterizationState = {};

            pso.multiSampleState   = {};

            pso.dynamicStates.Enable(RHIDynamicState::eScissor, RHIDynamicState::eViewPort);

            pso.colorBlendState.AddAttachment();

            pso.depthStencilState = RHIGfxPipelineDepthStencilState::Create(true, true, RHIDepthCompareOperator::eLessOrEqual);

            RDGGraphicsPassDesc lighting{};

            lighting.SetShaderProgramName(voxelGI == nullptr ? (capture ? "DeferredLightingCaptureSP" : "DeferredLightingSP")
                                                             : (capture ? "DeferredVoxelGICaptureSP" : "DeferredVoxelGISP"));

            lighting.AddColorOutput(m_pViewport->GetColorBackBuffer(), RHIRenderTargetLoadOp::eLoad);

            lighting.AddDepthStencilOutput(m_pViewport->GetDepthStencilBackBuffer(), RHIRenderTargetLoadOp::eClear,
                                           RHIRenderTargetStoreOp::eStore);

            lighting.SetPipelineStates(pso);

            lighting.SetRenderArea(0, 0, m_pViewport->GetWidth(), m_pViewport->GetHeight());

            lighting.SetPassTag("SceneLighting");

            const EnvTexture& env = m_pScene->GetEnvTexture();

            lighting.BindSampledTexture("positionMap", m_pColorSampler, "offscreen_position");

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

                VERIFY_EXPR(shadows != nullptr);

                shadows->BindLightingInputs(lighting);
            }

            pRDG->AddGraphicsPass(std::move(lighting))
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

        if (capture && captureValid)
        {
            pRDG->AddTransferPass("ReadLightingCapture")
                .CopyBuffer(m_captureOutput, m_captureReadback, {0, 0, m_captureOutput->GetRequiredSize()})
                .NeverCull();

            m_captureRecorded = true;
        }

        BuildLightMarkers();
    }
}

bool DeferredLightingRenderer::UsesForwardMaterials() const
{
    bool result = false;

    for (const sg::MaterialData& material : m_pScene->GetMaterialsData())
    {
        result = result || material.materialProperties.w > 0.5f || IsTranslucent(material);
    }

    for (const SceneMeshDraw& draw : SnapshotSceneDraws(*m_pScene, GI_ALL, false))
    {
        result = result || draw.topology != sg::MeshTopology::Triangles;
    }

    return result;
}

void DeferredLightingRenderer::BuildForwardGraph(VoxelGIRenderer* voxelGI, SceneShadowRenderer* shadows)
{
    RenderGraph* graph            = m_pRenderDevice->GetCurrentFrameRDG();

    RDGResourceManager* resources = graph->GetResourceManager();

    const uint32_t width          = m_pViewport->GetWidth();

    const uint32_t height         = m_pViewport->GetHeight();

    const EnvTexture& env         = m_pScene->GetEnvTexture();

    bool displayTransparency      = false;

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

    RHIGfxPipelineStates fullscreenState{};

    fullscreenState.colorBlendState.AddAttachment();

    fullscreenState.dynamicStates.Enable(RHIDynamicState::eScissor, RHIDynamicState::eViewPort);

    fullscreenState.depthStencilState = RHIGfxPipelineDepthStencilState::Create(false, false, RHIDepthCompareOperator::eAlways);

    RDGGraphicsPassDesc background;

    background.SetShaderProgramName("ForwardBackgroundSP");

    background.SetPassTag("ForwardBackground");

    background.SetPipelineStates(fullscreenState);

    background.SetRenderArea(0, 0, width, height);

    background.AddColorOutput(color, RHIRenderTargetLoadOp::eClear, RHIRenderTargetStoreOp::eStore, true);

    background.AddDepthStencilOutput(m_pViewport->GetDepthStencilBackBuffer(), RHIRenderTargetLoadOp::eClear,
                                     RHIRenderTargetStoreOp::eStore);

    background.BindValue("uCameraData", m_pScene->GetCameraUniformData(), sizeof(sg::CameraUniformData));

    background.BindValue("uSceneData", m_pScene->GetSceneUniformData(), sizeof(SceneUniformData));

    background.BindSampledTexture("envSourceMap", env.pPrefilteredSampler, env.pSkybox->GetDefaultView());

    graph->AddGraphicsPass(std::move(background)).RecordPassCommands([](RDGPassCmdEncoder& encoder) { encoder.Draw(3, 1); });

    HeapVector<SceneMeshDraw> opaqueDraws[3];

    HeapVector<ForwardDrawDepth> translucentDepths;

    const Mat4 view = reinterpret_cast<const sg::CameraUniformData*>(m_pScene->GetCameraUniformData())->view;

    for (sg::Node* node : m_pScene->GetRenderableNodes())
    {
        if (m_pScene->GetInstanceMask(node->GetRenderableIndex()) != 0)
        {
            for (sg::SubMesh* mesh : node->GetComponent<sg::Mesh>()->GetSubMeshes())
            {
                const SceneMeshDraw draw{node->GetRenderableIndex(),
                                         mesh->GetMaterial()->index,
                                         mesh->GetIndexCount(),
                                         mesh->GetFirstIndex(),
                                         0,
                                         mesh->topology};

                const sg::MaterialData& material = m_pScene->GetMaterialsData()[draw.materialIndex];

                if (IsTranslucent(material))
                {
                    const Mat4 modelView = view * m_pScene->GetInstanceTransform(draw.nodeIndex);

                    if (material.surfaceProperties.y == static_cast<float>(sg::AlphaMode::Blend)
                        && draw.topology == sg::MeshTopology::Triangles)
                    {
                        const HeapVector<uint32_t>& indices       = m_pScene->GetIndices();

                        const HeapVector<asset::Vertex>& vertices = m_pScene->GetVertices();

                        for (uint32_t index = 0; index + 2 < draw.indexCount; index += 3)
                        {
                            Vec3 center(0.0f);

                            for (uint32_t corner = 0; corner < 3; ++corner)
                            {
                                center += Vec3(vertices[indices[draw.firstIndex + index + corner]].pos);
                            }

                            const Vec4 viewCenter   = modelView * Vec4(center / 3.0f, 1.0f);

                            SceneMeshDraw triangle  = draw;

                            triangle.firstIndex    += index;

                            triangle.indexCount     = 3;

                            translucentDepths.push_back({triangle, viewCenter.z});
                        }
                    }
                    else
                    {
                        const Vec4 center = modelView * Vec4(mesh->GetAABB().GetCenter(), 1.0f);

                        translucentDepths.push_back({draw, center.z});
                    }
                }
                else
                {
                    opaqueDraws[static_cast<uint32_t>(draw.topology)].push_back(draw);
                }
            }
        }
    }

    std::stable_sort(translucentDepths.begin(), translucentDepths.end(), SortForwardDepth);

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
            if (!opaqueDraws[topology].empty())
            {
                RHIGfxPipelineStates state{};

                state.primitiveType               = ForwardTopology(static_cast<sg::MeshTopology>(topology));

                state.rasterizationState.cullMode = RHIPolygonCullMode::eDisabled;

                state.depthStencilState =
                    RHIGfxPipelineDepthStencilState::Create(true, true, RHIDepthCompareOperator::eLessOrEqual);

                state.colorBlendState.AddAttachments(2);

                state.dynamicStates.Enable(RHIDynamicState::eScissor, RHIDynamicState::eViewPort);

                RDGGraphicsPassDesc source;

                source.SetShaderProgramName(voxelGI == nullptr ? "ForwardScatterSP" : "ForwardScatterVoxelGISP");

                source.SetPassTag("ForwardScatterSource");

                source.SetPipelineStates(state);

                source.SetRenderArea(0, 0, width, height);

                source.AddColorOutput(scatterLighting, RHIRenderTargetLoadOp::eLoad);

                source.AddColorOutput(scatterPosition, RHIRenderTargetLoadOp::eLoad);

                source.AddDepthStencilOutput(m_pViewport->GetDepthStencilBackBuffer(), RHIRenderTargetLoadOp::eLoad,
                                             RHIRenderTargetStoreOp::eStore);

                source.BindStorageBuffer("NodeBuffer", m_pScene->GetNodesDataSSBO());

                source.BindStorageBuffer("MaterialBuffer", m_pScene->GetMaterialsDataSSBO());

                source.BindStorageBuffer("UVBuffer", m_pScene->GetUVBuffer());

                source.BindValue("uCameraData", m_pScene->GetCameraUniformData(), sizeof(sg::CameraUniformData));

                source.BindValue("uSceneData", m_pScene->GetSceneUniformData(), sizeof(SceneUniformData));

                source.BindSampledTexture("envIrradianceMap", env.pIrradianceSampler, env.pIrradiance->GetDefaultView());

                BindSceneTextureArray(source, m_pColorSampler, m_pScene->GetSceneTextures(), m_pScene->GetSceneSamplers());

                source.BindVertexBuffer(m_pScene->GetVertexBuffer());

                source.BindIndexBuffer(m_pScene->GetIndexBuffer());

                if (voxelGI != nullptr)
                {
                    voxelGI->BindLightingInputs(source);

                    VERIFY_EXPR(shadows != nullptr);

                    shadows->BindLightingInputs(source);
                }

                graph->AddGraphicsPass(std::move(source))
                    .RecordPassCommands(
                        [draws = opaqueDraws[topology]](RDGPassCmdEncoder& encoder) { RecordForwardDraws(encoder, draws); });
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

        uint32_t first = 0;

        bool pending   = stage < 3 ? !opaqueDraws[stage].empty() : !translucentDepths.empty();

        while (pending)
        {
            HeapVector<SceneMeshDraw> draws;

            const sg::MeshTopology topology =
                stage < 3 ? static_cast<sg::MeshTopology>(stage) : translucentDepths[first].draw.topology;

            const bool writeDepth =
                stage < 3
                || m_pScene->GetMaterialsData()[translucentDepths[first].draw.materialIndex].surfaceProperties.y
                       != static_cast<float>(sg::AlphaMode::Blend);

            if (stage < 3)
            {
                draws   = opaqueDraws[stage];

                pending = false;
            }
            else
            {
                while (first < translucentDepths.size() && translucentDepths[first].draw.topology == topology
                       && (m_pScene->GetMaterialsData()[translucentDepths[first].draw.materialIndex].surfaceProperties.y
                           != static_cast<float>(sg::AlphaMode::Blend))
                              == writeDepth)
                {
                    draws.push_back(translucentDepths[first].draw);

                    ++first;
                }

                pending = first < translucentDepths.size();
            }

            RHIGfxPipelineStates state{};

            state.primitiveType               = ForwardTopology(topology);

            state.rasterizationState.cullMode = RHIPolygonCullMode::eDisabled;

            state.depthStencilState =
                RHIGfxPipelineDepthStencilState::Create(true, writeDepth, RHIDepthCompareOperator::eLessOrEqual);

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

            forward.SetShaderProgramName(voxelGI == nullptr ? "ForwardMaterialSP" : "ForwardMaterialVoxelGISP");

            forward.SetPassTag(stage < 3 ? "ForwardOpaque" : "ForwardTranslucent");

            forward.SetPipelineStates(state);

            forward.SetRenderArea(0, 0, width, height);

            forward.AddColorOutput(stage == 3 ? displayColor : color, RHIRenderTargetLoadOp::eLoad);

            forward.AddDepthStencilOutput(m_pViewport->GetDepthStencilBackBuffer(), RHIRenderTargetLoadOp::eLoad,
                                          RHIRenderTargetStoreOp::eStore);

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

            BindSceneTextureArray(forward, m_pColorSampler, m_pScene->GetSceneTextures(), m_pScene->GetSceneSamplers());

            forward.BindVertexBuffer(m_pScene->GetVertexBuffer());

            forward.BindIndexBuffer(m_pScene->GetIndexBuffer());

            if (voxelGI != nullptr)
            {
                voxelGI->BindLightingInputs(forward);

                VERIFY_EXPR(shadows != nullptr);

                shadows->BindLightingInputs(forward);
            }

            graph->AddGraphicsPass(std::move(forward))
                .RecordPassCommands([draws, displayLinear = stage == 3 && displayTransparency](RDGPassCmdEncoder& encoder) {
                    RecordForwardDraws(encoder, draws, displayLinear);
                });
        }
    }

    RDGGraphicsPassDesc toneMap;

    toneMap.SetShaderProgramName("ForwardToneMapSP");

    toneMap.SetPassTag("ForwardToneMap");

    toneMap.SetPipelineStates(fullscreenState);

    toneMap.SetRenderArea(0, 0, width, height);

    toneMap.AddColorOutput(m_pViewport->GetColorBackBuffer(), RHIRenderTargetLoadOp::eLoad);

    toneMap.BindSampledTexture("sceneColorMap", m_pTransmissionSampler, displayColor);

    graph->AddGraphicsPass(std::move(toneMap)).RecordPassCommands([displayTransparency](RDGPassCmdEncoder& encoder) {
        encoder.SetPushConstants(displayTransparency ? 2u : 0u);

        encoder.Draw(3, 1);
    });

    m_captureRecorded = false;

    BuildLightMarkers();
}

void DeferredLightingRenderer::BuildLightMarkers()
{
    const SceneUniformData& sceneData = *reinterpret_cast<const SceneUniformData*>(m_pScene->GetSceneUniformData());

    const uint32_t lightCount         = static_cast<uint32_t>(sceneData.lightInfo.x);

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

        markers.SetRenderArea(0, 0, m_pViewport->GetWidth(), m_pViewport->GetHeight());

        markers.AddColorOutput(m_pViewport->GetColorBackBuffer(), RHIRenderTargetLoadOp::eLoad);

        markers.AddDepthStencilOutput(m_pViewport->GetDepthStencilBackBuffer(), RHIRenderTargetLoadOp::eLoad,
                                      RHIRenderTargetStoreOp::eStore);

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
