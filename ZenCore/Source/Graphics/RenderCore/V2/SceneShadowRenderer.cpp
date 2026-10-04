#include "Graphics/RenderCore/V2/Renderer/SceneShadowRenderer.h"
#include "Graphics/RenderCore/V2/Renderer/RendererUtils.h"
#include "Graphics/RenderCore/V2/RenderDevice.h"
#include "Graphics/RenderCore/V2/ShaderProgram.h"
#include "Platform/ConfigLoader.h"
#include <cmath>
#include <cstring>
#include <limits>

namespace zen::rc
{
SceneShadowRenderer::SceneShadowRenderer(RenderDevice* device) : m_device(device)
{
    uint32_t resolution = m_resolution;

    if (platform::ConfigLoader::GetInstance().ReadNumber("shadow_map_resolution", resolution) && resolution >= 128
        && resolution <= 2048)
    {
        m_resolution = resolution;
    }
    else
    {
        LOGW("Invalid shadow_map_resolution; using 1024");
    }
}

bool SceneShadowRenderer::SetResolution(uint32_t resolution)
{
    const bool valid = m_device->CanReconfigureResources() && resolution >= 128 && resolution <= 2048;

    if (valid && resolution != m_resolution)
    {
        Destroy();

        m_resolution = resolution;
    }

    return valid;
}

bool SceneShadowRenderer::Preflight(uint32_t resolution, uint32_t faces) const
{
    const bool retained = m_maps != nullptr && resolution == m_resolution && m_maps->GetArrayLayers() == std::max(2u, faces);

    const RHIGPUMemoryStats memory = m_device->GetGPUMemoryStats();

    const uint64_t capacity        = m_device->GetGPUInfo().deviceLocalMemoryBytes;

    // ApplyVoxelGISettings drains GPU work, destroys resized maps, and collects them before
    // allocating replacements. Use the same D32 byte estimate as ValidateShadowMemory.
    const uint64_t reclaimed = resolution != m_resolution
                                 ? uint64_t(m_resolution) * m_resolution * 4
                                       * ((m_depth != nullptr ? 1u : 0u) + (m_maps != nullptr ? m_maps->GetArrayLayers() : 0u))
                                 : 0;

    const uint64_t used      = memory.deviceLocalBytes - std::min(memory.deviceLocalBytes, reclaimed);

    // A backend which reports no capacity has an unknown budget, not zero bytes.
    // Keep dimensional validation and let allocation report failure in that case.
    uint64_t available = capacity == 0 ? std::numeric_limits<uint64_t>::max() : capacity > used ? capacity - used : 0;

    if (memory.budgetAvailable)
    {
        uint64_t budgetAvailable = 0;

        for (uint32_t index = 0; index < memory.heapCount; ++index)
        {
            const RHIGPUMemoryStats::Heap& heap = memory.heaps[index];

            const uint64_t heapUsed             = heap.usageBytes - std::min(heap.usageBytes, reclaimed);

            if (heap.deviceLocal && heap.budgetBytes > heapUsed)
            {
                budgetAvailable = std::max(budgetAvailable, heap.budgetBytes - heapUsed);
            }
        }

        available = std::min(available, budgetAvailable);
    }

    return retained || ValidateShadowMemory(resolution, faces, available);
}

void SceneShadowRenderer::PrepareLight(const GPULight& light, uint32_t index, const sg::AABB& bounds)
{
    const SceneLightType type = static_cast<SceneLightType>(light.directionType.w);

    const Vec3 position(light.positionRange);

    const Vec3 direction(light.directionType);

    const float radius        = std::max(glm::length(bounds.GetMax() - bounds.GetMin()) * 0.5f, 0.001f);

    const float sceneDistance = glm::length(position - bounds.GetCenter()) + radius;

    const float range         = light.positionRange.w > 0.0f ? light.positionRange.w : sceneDistance;

    const float nearPlane     = std::max(std::min(radius * 0.001f, range * 0.01f), 1e-6f);

    const float farPlane      = std::max(nearPlane * 2.0f, std::min(range, sceneDistance));

    const uint32_t firstFace  = static_cast<uint32_t>(m_faces.size());

    const uint32_t faceCount  = type == SceneLightType::ePoint ? 6 : 1;

    const float halfAngle     = type == SceneLightType::eSpot
                                  ? std::clamp(std::acos(std::clamp(light.coneShadow.y, 0.0f, 1.0f)), 0.001f, glm::radians(89.9f))
                                  : glm::radians(45.0f);

    const float depthRange    = type == SceneLightType::eDirectional ? radius * 2.0f : farPlane;

    const float texelWidth =
        2.0f * (type == SceneLightType::eDirectional ? radius : std::tan(halfAngle)) / static_cast<float>(m_resolution);

    m_uniforms.lights[index] =
        Vec4(static_cast<float>(firstFace), static_cast<float>(faceCount), 1.0f / depthRange, texelWidth);

    static const Vec3 cubeDirections[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};

    for (uint32_t face = 0; face < faceCount; ++face)
    {
        const Vec3 forward = type == SceneLightType::ePoint ? cubeDirections[face] : direction;

        const Vec3 up      = std::abs(forward.y) < 0.99f ? Vec3(0, 1, 0) : Vec3(0, 0, 1);

        const Vec3 eye =
            type == SceneLightType::eDirectional ? bounds.GetCenter() - direction * (radius + nearPlane) : position;

        Mat4 projection   = type == SceneLightType::eDirectional
                              ? glm::ortho(-radius, radius, -radius, radius, nearPlane, nearPlane + depthRange)
                              : glm::perspective(2.0f * halfAngle, 1.0f, nearPlane, farPlane);

        projection[1][1] *= -1.0f;

        FaceData data;

        data.viewProjection        = projection * glm::lookAt(eye, eye + forward, up);

        data.lightPositionInvRange = Vec4(type == SceneLightType::eDirectional ? -direction : position,
                                          type == SceneLightType::eDirectional ? 0.0f : 1.0f / farPlane);

        m_uniforms.viewProjection[firstFace + face] = data.viewProjection;

        m_faces.push_back(data);
    }
}

bool SceneShadowRenderer::Prepare(const RenderScene& scene, bool enabled, bool includeInactiveLights)
{
    m_uniforms = {};

    m_faces.clear();

    m_uniforms.settings =
        Vec4(std::max(scene.GetAABB().GetMaxExtent(), 0.001f) * 0.0003f, static_cast<float>(m_resolution), 0, 0);

    const SceneUniformData& sceneData = *reinterpret_cast<const SceneUniformData*>(scene.GetSceneUniformData());

    for (uint32_t index = 0; index < static_cast<uint32_t>(sceneData.lightInfo.x); ++index)
    {
        const GPULight& light = sceneData.lights[index];

        if (enabled && light.coneShadow.z > 0.0f && (includeInactiveLights || light.colorIntensity.w > 0.0f))
        {
            PrepareLight(light, index, scene.GetAABB());
        }
    }

    if (enabled && sceneData.cameraLight.colorIntensity.w > 0.0f)
    {
        PrepareLight(sceneData.cameraLight, MaxSceneLights, scene.GetAABB());
    }

    // At least two layers keep the native default view a 2D array, even with one/no light.
    const uint32_t layers = std::max(2u, static_cast<uint32_t>(m_faces.size()));

    const bool fits       = Preflight(m_resolution, static_cast<uint32_t>(m_faces.size()));

    if (fits && m_maps != nullptr && m_maps->GetArrayLayers() != layers)
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

    if (fits && m_depth == nullptr)
    {
        m_depth = m_device->CreateTextureDepthStencilRT(format, {.copyUsage = true}, "scene_shadow_depth");
    }

    if (fits && m_depth != nullptr && m_maps == nullptr)
    {
        format.arrayLayers = layers;

        m_maps             = m_device->CreateTextureSampled(format, {.copyUsage = true}, "scene_shadow_maps");
    }

    if (m_depthSampler == nullptr)
    {
        m_depthSampler = m_device->CreateSampler({});
    }

    if (m_materialSampler == nullptr)
    {
        m_materialSampler = m_device->CreateSampler(RHISamplerCreateInfo::CreateLinearRepeat());
    }

    const bool valid =
        fits && m_depth != nullptr && m_maps != nullptr && m_depthSampler != nullptr && m_materialSampler != nullptr;

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

    pso.depthStencilState           = RHIGfxPipelineDepthStencilState::Create(true, true, RHIDepthCompareOperator::eLess);

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

    pass.BindStorageBuffer("UVBuffer", scene.GetUVBuffer());

    BindSceneTextureArray(pass, m_materialSampler, scene.GetSceneTextures(), scene.GetSceneSamplers());

    pass.BindVertexBuffer(scene.GetVertexBuffer());

    pass.BindIndexBuffer(scene.GetIndexBuffer());

    HeapVector<SceneMeshDraw> draws =
        drawGeometry ? SnapshotSceneDraws(scene, GI_ALL, true, false) : HeapVector<SceneMeshDraw>{};

    graph->AddGraphicsPass(std::move(pass)).RecordPassCommands([draws = std::move(draws)](RDGPassCmdEncoder& encoder) {
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

    region.size                           = {static_cast<int32_t>(m_resolution), static_cast<int32_t>(m_resolution), 1};

    graph->AddTransferPass(NameID(fmt::format("SceneShadowCopy_{}", layer)))
        .CopyTexture(m_depth, m_maps, MakeVecView(&region, 1));
}

void SceneShadowRenderer::BuildRenderGraph(const RenderScene& scene, uint64_t geometryRevision)
{
    m_recordedGeometry = geometryRevision;

    for (uint32_t layer = 0; layer < m_maps->GetArrayLayers(); ++layer)
    {
        const bool hasFace = layer < m_faces.size();

        const bool changed = hasFace
                          && (layer >= m_committedFaces.size()
                              || std::memcmp(&m_faces[layer], &m_committedFaces[layer], sizeof(FaceData)) != 0);

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

DebugOutputDescription SceneShadowRenderer::BuildDebugView(const RenderScene&    scene,
                                                           const RenderView&     view,
                                                           const DebugSelection& selection)
{
    DebugOutputDescription description;

    description.output = DebugOutput::eShadow;

    description.width = description.height = m_resolution;

    description.format                     = DataFormat::eD32SFloat;

    description.interpretation =
        "Normalized radial distance for point/spot lights; normalized orthographic distance for directional lights.";

    description.reason    = "Select an enabled, non-zero shadow-casting light and a valid face.";

    uint32_t enabledIndex = 0;

    for (const LightEntry& entry : scene.GetLights().GetEntries())
    {
        if (entry.light.enabled)
        {
            if (entry.id == selection.lightId)
            {
                const Vec4 layers     = m_uniforms.lights[enabledIndex];

                description.faceCount = static_cast<uint32_t>(layers.y);

                description.available = m_maps != nullptr && selection.face < description.faceCount;

                if (description.available)
                {
                    DebugVisualizationData data;

                    data.range               = Vec4(selection.minimum, selection.maximum, 0, 0);

                    data.selection.y         = static_cast<uint32_t>(layers.x) + selection.face;

                    RDGGraphicsPassDesc pass = MakeDebugVisualizationPass(view, "RenderDebugArraySP", data);

                    pass.BindSampledTexture("sourceImage", m_depthSampler, m_maps->GetDefaultView());

                    AddDebugVisualizationPass(*m_device->GetCurrentFrameRDG(), std::move(pass));

                    description.reason.clear();
                }
            }

            ++enabledIndex;
        }
    }

    return description;
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
