#include "Graphics/RenderCore/V2/Renderer/SceneShadowRenderer.h"
#include "Graphics/RenderCore/V2/Renderer/RendererUtils.h"
#include "Graphics/RenderCore/V2/RenderDevice.h"
#include "Graphics/RenderCore/V2/ShaderProgram.h"
#include "Platform/ConfigLoader.h"
#include <cmath>
#include <cstring>

namespace zen::rc
{
SceneShadowRenderer::SceneShadowRenderer(RenderDevice* device) : m_device(device)
{
    uint32_t resolution = m_resolution;
    if (platform::ConfigLoader::GetInstance().ReadNumber("shadow_map_resolution", resolution) &&
        resolution >= 128 && resolution <= 2048)
    {
        m_resolution = resolution;
    }
    else
    {
        LOGW("Invalid shadow_map_resolution; using 1024");
    }
}

void SceneShadowRenderer::PrepareLight(const GPULight& light,
                                       uint32_t index,
                                       const sg::AABB& bounds)
{
    const SceneLightType type = static_cast<SceneLightType>(light.directionType.w);
    const Vec3 position(light.positionRange);
    const Vec3 direction(light.directionType);
    const float radius    = std::max(glm::length(bounds.GetMax() - bounds.GetMin()) * 0.5f, 0.001f);
    const float nearPlane = std::min(radius * 0.001f, light.positionRange.w * 0.01f);
    const float farPlane  = std::max(
        nearPlane * 2.0f,
        std::min(light.positionRange.w, glm::length(position - bounds.GetCenter()) + radius));
    const uint32_t firstFace = static_cast<uint32_t>(m_faces.size());
    const uint32_t faceCount = type == SceneLightType::ePoint ? 6 : 1;
    const float halfAngle    = type == SceneLightType::eSpot ?
        std::max(std::acos(std::clamp(light.coneShadow.y, 0.0f, 1.0f)), 0.001f) :
        glm::radians(45.0f);
    const float depthRange   = type == SceneLightType::eDirectional ? radius * 2.0f : farPlane;
    const float texelWidth   = 2.0f *
        (type == SceneLightType::eDirectional ? radius : std::tan(halfAngle)) /
        static_cast<float>(m_resolution);
    m_uniforms.lights[index] = Vec4(static_cast<float>(firstFace), static_cast<float>(faceCount),
                                    1.0f / depthRange, texelWidth);
    static const Vec3 cubeDirections[6] = {{1, 0, 0},  {-1, 0, 0}, {0, 1, 0},
                                           {0, -1, 0}, {0, 0, 1},  {0, 0, -1}};
    for (uint32_t face = 0; face < faceCount; ++face)
    {
        const Vec3 forward = type == SceneLightType::ePoint ? cubeDirections[face] : direction;
        const Vec3 up      = std::abs(forward.y) < 0.99f ? Vec3(0, 1, 0) : Vec3(0, 0, 1);
        const Vec3 eye     = type == SceneLightType::eDirectional ?
            bounds.GetCenter() - direction * (radius + nearPlane) :
            position;
        Mat4 projection    = type == SceneLightType::eDirectional ?
            glm::ortho(-radius, radius, -radius, radius, nearPlane, nearPlane + depthRange) :
            glm::perspective(2.0f * halfAngle, 1.0f, nearPlane, farPlane);
        projection[1][1] *= -1.0f;
        FaceData data;
        data.viewProjection = projection * glm::lookAt(eye, eye + forward, up);
        data.lightPositionInvRange =
            Vec4(position, type == SceneLightType::eDirectional ? 0.0f : 1.0f / farPlane);
        m_uniforms.viewProjection[firstFace + face] = data.viewProjection;
        m_faces.push_back(data);
    }
}

bool SceneShadowRenderer::Prepare(const RenderScene& scene,
                                  bool enabled,
                                  bool includeInactiveLights)
{
    m_uniforms = {};
    m_faces.clear();
    m_uniforms.settings = Vec4(std::max(scene.GetAABB().GetMaxExtent(), 0.001f) * 0.0003f,
                               static_cast<float>(m_resolution), 0, 0);
    const SceneUniformData& sceneData =
        *reinterpret_cast<const SceneUniformData*>(scene.GetSceneUniformData());
    for (uint32_t index = 0; index < static_cast<uint32_t>(sceneData.lightInfo.x); ++index)
    {
        const GPULight& light = sceneData.lights[index];
        if (enabled && light.coneShadow.z > 0.0f &&
            (includeInactiveLights || light.colorIntensity.w > 0.0f))
        {
            PrepareLight(light, index, scene.GetAABB());
        }
    }

    // At least two layers keep the native default view a 2D array, even with one/no light.
    const uint32_t layers = std::max(2u, static_cast<uint32_t>(m_faces.size()));
    if (m_maps != nullptr && m_maps->GetArrayLayers() != layers)
    {
        m_device->DestroyTexture(m_maps);
        m_maps          = nullptr;
        m_validContents = false;
    }
    TextureFormat format;
    format.format    = DataFormat::eD32SFloat;
    format.dimension = TextureDimension::e2D;
    format.width     = m_resolution;
    format.height    = m_resolution;
    format.depth     = 1;
    if (m_depth == nullptr)
    {
        m_depth = m_device->CreateTextureDepthStencilRT(format, {.copyUsage = true},
                                                        "scene_shadow_depth");
    }
    if (m_depth != nullptr && m_maps == nullptr)
    {
        format.arrayLayers = layers;
        m_maps = m_device->CreateTextureSampled(format, {.copyUsage = true}, "scene_shadow_maps");
    }
    if (m_depthSampler == nullptr)
    {
        m_depthSampler = m_device->CreateSampler({});
    }
    if (m_materialSampler == nullptr)
    {
        m_materialSampler = m_device->CreateSampler(RHISamplerCreateInfo::CreateLinearRepeat());
    }
    const bool valid = m_depth != nullptr && m_maps != nullptr && m_depthSampler != nullptr &&
        m_materialSampler != nullptr;
    if (!valid)
    {
        LOGE("Scene shadow resource creation failed");
        Destroy();
    }
    return valid;
}

void SceneShadowRenderer::BuildFace(const RenderScene& scene, uint32_t layer, bool drawGeometry)
{
    RenderGraph* graph = m_device->GetCurrentFrameRDG();
    RHIGfxPipelineStates pso{};
    pso.rasterizationState.cullMode = RHIPolygonCullMode::eDisabled;
    pso.depthStencilState =
        RHIGfxPipelineDepthStencilState::Create(true, true, RHIDepthCompareOperator::eLess);
    pso.dynamicStates.Enable(RHIDynamicState::eScissor, RHIDynamicState::eViewPort);

    RDGGraphicsPassDesc pass;
    pass.SetShaderProgramName("SceneShadowSP");
    pass.SetPassTag(NameID(fmt::format("SceneShadowFace_{}", layer)));
    pass.SetPipelineStates(pso);
    pass.SetRenderArea(0, 0, m_resolution, m_resolution);
    pass.AddDepthStencilOutput(m_depth);
    const FaceData face = drawGeometry ? m_faces[layer] : FaceData{};
    pass.BindValue("uShadowFace", face);
    pass.BindStorageBuffer("NodeBuffer", scene.GetNodesDataSSBO());
    pass.BindStorageBuffer("MaterialBuffer", scene.GetMaterialsDataSSBO());
    BindSceneTextureArray(pass, m_materialSampler, scene.GetSceneTextures());
    pass.BindVertexBuffer(scene.GetVertexBuffer());
    pass.BindIndexBuffer(scene.GetIndexBuffer());
    HeapVector<SceneMeshDraw> draws =
        drawGeometry ? SnapshotSceneDraws(scene) : HeapVector<SceneMeshDraw>{};
    graph->AddGraphicsPass(std::move(pass))
        .RecordPassCommands([draws = std::move(draws)](RDGPassCmdEncoder& encoder) {
            for (const SceneMeshDraw& draw : draws)
            {
                const GBufferSP::PushConstantsData constants{draw.nodeIndex, draw.materialIndex};
                encoder.SetPushConstants(constants);
                encoder.DrawIndexed(draw.indexCount, 1, draw.firstIndex, 0, 0);
            }
        });

    // The graph's depth-attachment API renders one whole 2D texture. Copy each face into
    // its sampled array layer, using the same declared transfer path as the EVSM renderer.
    RHITextureCopyRegion region{};
    region.srcSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eDepth);
    region.dstSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eDepth);
    region.dstSubresources.baseArrayLayer = layer;
    region.size = {static_cast<int32_t>(m_resolution), static_cast<int32_t>(m_resolution), 1};
    graph->AddTransferPass(NameID(fmt::format("SceneShadowCopy_{}", layer)))
        .CopyTexture(m_depth, m_maps, MakeVecView(&region, 1));
}

void SceneShadowRenderer::BuildRenderGraph(const RenderScene& scene, uint64_t geometryRevision)
{
    m_recordedGeometry = geometryRevision;
    for (uint32_t layer = 0; layer < m_maps->GetArrayLayers(); ++layer)
    {
        const bool hasFace = layer < m_faces.size();
        const bool changed = hasFace &&
            (layer >= m_committedFaces.size() ||
             std::memcmp(&m_faces[layer], &m_committedFaces[layer], sizeof(FaceData)) != 0);
        if (!m_validContents || m_geometryRevision != geometryRevision || changed)
        {
            BuildFace(scene, layer, hasFace);
        }
    }
}

void SceneShadowRenderer::BindLightingInputs(RDGPassDescBase& pass) const
{
    pass.BindValue("uSceneShadows", m_uniforms);
    pass.BindSampledTexture("sceneShadowMaps", m_depthSampler, m_maps->GetDefaultView());
}

void SceneShadowRenderer::OnRenderGraphExecuted(bool succeeded)
{
    m_validContents    = succeeded;
    m_geometryRevision = succeeded ? m_recordedGeometry : 0;
    if (succeeded)
    {
        m_committedFaces = m_faces;
    }
}

void SceneShadowRenderer::Destroy()
{
    m_device->DestroyTexture(m_depth);
    m_device->DestroyTexture(m_maps);
    m_depth         = nullptr;
    m_maps          = nullptr;
    m_validContents = false;
    m_committedFaces.clear();
}
} // namespace zen::rc
