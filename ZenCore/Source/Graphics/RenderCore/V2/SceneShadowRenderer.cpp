#include "Graphics/RenderCore/V2/Renderer/SceneShadowRenderer.h"
#include "Graphics/RenderCore/V2/Renderer/RendererUtils.h"
#include "Graphics/RenderCore/V2/RenderDevice.h"
#include "Graphics/RenderCore/V2/ShaderProgram.h"
#include "Platform/ConfigLoader.h"
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <glm/gtc/matrix_access.hpp>

namespace zen::rc
{
namespace
{
using ShadowPlanes = std::array<Vec4, 6>;

ShadowPlanes GetShadowPlanes(const Mat4& viewProjection)
{
    const Vec4 x = glm::row(viewProjection, 0);
    const Vec4 y = glm::row(viewProjection, 1);
    const Vec4 z = glm::row(viewProjection, 2);
    const Vec4 w = glm::row(viewProjection, 3);
    // Shadow rasterization uses Vulkan's [0,w] clip-depth interval.
    return {w + x, w - x, w + y, w - y, z, w - z};
}

bool IntersectsShadowFace(const sg::AABB& bounds, const ShadowPlanes& planes)
{
    bool visible = glm::all(glm::lessThanEqual(bounds.GetMin(), bounds.GetMax()));
    if (visible)
    {
        const Vec3 center = bounds.GetCenter();
        const Vec3 extent = (bounds.GetMax() - bounds.GetMin()) * 0.5f;
        for (const Vec4& plane : planes)
        {
            const float distance = glm::dot(Vec3(plane), center) + plane.w;
            const float radius   = glm::dot(glm::abs(Vec3(plane)), extent);
            // Retain boxes touching a face edge, including planar/thin casters and cube seams.
            const float tolerance = 32.0f * std::numeric_limits<float>::epsilon()
                                  * (glm::dot(glm::abs(Vec3(plane)), glm::abs(center)) + std::abs(plane.w) + radius + 1.0f);
            visible               = visible && distance + radius >= -tolerance;
        }
    }
    return visible;
}
} // namespace

SceneShadowRenderer::SceneShadowRenderer(RenderDevice* device, uint32_t initialResolution) : m_device(device)
{
    uint32_t resolution = m_resolution;

    if (initialResolution != 0)
    {
        VERIFY_EXPR(initialResolution >= 128 && initialResolution <= 2048);
        m_resolution = initialResolution;
    }
    else if (platform::ConfigLoader::GetInstance().ReadNumber("shadow_map_resolution", resolution) && resolution >= 128
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

    const float halfAngle  = type == SceneLightType::eSpot
                               ? std::clamp(std::acos(std::clamp(light.coneShadow.y, 0.0f, 1.0f)), 0.001f, glm::radians(89.9f))
                               : glm::radians(45.0f);

    const float depthRange = type == SceneLightType::eDirectional ? radius * 2.0f : farPlane;

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

        data.viewProjection                         = projection * glm::lookAt(eye, eye + forward, up);

        data.lightPositionInvRange                  = Vec4(type == SceneLightType::eDirectional ? -direction : position,
                                                           type == SceneLightType::eDirectional ? 0.0f : 1.0f / farPlane);

        m_uniforms.viewProjection[firstFace + face] = data.viewProjection;

        m_faces.push_back(data);
    }
}

bool SceneShadowRenderer::Prepare(const RenderScene& scene, bool enabled, bool includeInactiveLights)
{
    if (m_scene != &scene)
    {
        m_validContents = false;
        m_committedFaces.clear();
        m_committedCasters.clear();
        m_recordedCasters.clear();
        m_scene = &scene;
    }

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

        m_device->DestroyTexture(m_maxDepth);

        m_maps          = nullptr;

        m_maxDepth      = nullptr;

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

    if (fits && m_maps != nullptr && m_maxDepth == nullptr)
    {
        RHITextureCreateInfo reduced;
        reduced.type        = RHITextureType::e2D;
        reduced.format      = DataFormat::eR32SFloat;
        reduced.width       = m_resolution / 4;
        reduced.height      = m_resolution / 4;
        reduced.arrayLayers = layers;
        reduced.tag         = "scene_shadow_max_depth";
        reduced.usageFlags.SetFlags(RHITextureUsageFlagBits::eStorage, RHITextureUsageFlagBits::eSampled);
        m_maxDepth = m_device->CreateTexture(reduced);
    }

    if (m_depthSampler == nullptr)
    {
        m_depthSampler = m_device->CreateSampler({});
    }

    if (m_materialSampler == nullptr)
    {
        m_materialSampler = m_device->CreateSampler(RHISamplerCreateInfo::CreateLinearRepeat());
    }

    const bool valid = fits && m_depth != nullptr && m_maps != nullptr && m_maxDepth != nullptr && m_depthSampler != nullptr
                    && m_materialSampler != nullptr;

    if (!valid)
    {
        LOGE("Scene shadow resource creation failed");

        Destroy();
    }

    return valid;
}

void SceneShadowRenderer::BuildFace(const RenderScene& scene, uint32_t layer, const HeapVector<SceneMeshDraw>& sceneDraws)
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

    const bool     hasFace = layer < m_faces.size();
    const FaceData face    = hasFace ? m_faces[layer] : FaceData{};

    pass.BindValue("uShadowFace", face);

    pass.BindStorageBuffer("NodeBuffer", scene.GetNodesDataSSBO());

    pass.BindStorageBuffer("MaterialBuffer", scene.GetMaterialsDataSSBO());

    pass.BindStorageBuffer("UVBuffer", scene.GetUVBuffer());

    BindSceneTextureArray(pass, m_materialSampler, scene.GetSceneTextureViews(), scene.GetSceneSamplers());

    pass.BindVertexBuffer(scene.GetVertexBuffer());

    pass.BindIndexBuffer(scene.GetIndexBuffer());

    HeapVector<SceneMeshDraw> draws;
    if (hasFace)
    {
        const ShadowPlanes planes = GetShadowPlanes(face.viewProjection);
        for (const SceneMeshDraw& draw : sceneDraws)
        {
            if (draw.nodeIndex >= m_recordedCasters.size()
                || IntersectsShadowFace(m_recordedCasters[draw.nodeIndex].bounds, planes))
            {
                draws.push_back(draw);
            }
        }
    }

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

void SceneShadowRenderer::BuildRenderGraph(const RenderScene& scene)
{
    m_recordedCasters = scene.GetShadowCasters();
    HeapVector<sg::AABB> changedBounds;
    for (size_t index = 0; index < std::max(m_recordedCasters.size(), m_committedCasters.size()); ++index)
    {
        const bool current  = index < m_recordedCasters.size();
        const bool previous = index < m_committedCasters.size();
        if (!current || !previous || m_recordedCasters[index].revision != m_committedCasters[index].revision
            || m_recordedCasters[index].bounds != m_committedCasters[index].bounds)
        {
            if (previous)
            {
                changedBounds.push_back(m_committedCasters[index].bounds);
            }
            if (current && (!previous || m_recordedCasters[index].bounds != m_committedCasters[index].bounds))
            {
                changedBounds.push_back(m_recordedCasters[index].bounds);
            }
        }
    }

    HeapVector<SceneMeshDraw> draws;
    bool                      snapshotDraws = false;
    HeapVector<uint32_t>      rendered;

    for (uint32_t layer = 0; layer < m_maps->GetArrayLayers(); ++layer)
    {
        const bool hasFace = layer < m_faces.size();

        bool changed       = hasFace
                          && (layer >= m_committedFaces.size()
                              || std::memcmp(&m_faces[layer], &m_committedFaces[layer], sizeof(FaceData)) != 0);

        if (hasFace && !changed && m_validContents && !changedBounds.empty())
        {
            const ShadowPlanes planes = GetShadowPlanes(m_faces[layer].viewProjection);
            for (size_t index = 0; index < changedBounds.size() && !changed; ++index)
            {
                changed = IntersectsShadowFace(changedBounds[index], planes);
            }
        }

        if (!m_validContents || changed)
        {
            if (hasFace && !snapshotDraws)
            {
                draws         = SnapshotSceneDraws(scene, GI_ALL, true, false);
                snapshotDraws = true;
            }
            BuildFace(scene, layer, draws);
            rendered.push_back(layer);
        }
    }

    if (!rendered.empty())
    {
        // Lighting skips a local light's filter beyond the deepest depth under its taps (scene_shadows.glsl).
        // Reduce after every copy, so the pass reads initialized layers. Reducing all layers defines
        // the whole texture; later frames reduce only re-rendered layers.
        struct ReductionUniforms
        {
            glm::uvec4 layers[(MaxSceneShadowFaces + 3) / 4]{};
        } uniforms;

        for (uint32_t index = 0; index < rendered.size(); ++index)
        {
            uniforms.layers[index / 4][index % 4] = rendered[index];
        }

        const bool complete = rendered.size() == m_maps->GetArrayLayers();

        RDGComputePassDesc reduce;

        reduce.SetShaderProgramName("SceneShadowMaxDepthSP");

        reduce.SetPassTag("SceneShadowMaxDepth");

        reduce.BindSampledTexture("sceneShadowMaps", m_depthSampler, m_maps->GetDefaultView());

        reduce.BindStorageImage("sceneShadowMaxDepth", m_maxDepth->GetDefaultView(),
                                complete ? RDGContentGuarantee::eFullWrite : RDGContentGuarantee::eNone);

        reduce.BindValue("uShadowReduction", uniforms);

        const uint32_t groups = (m_resolution / 4 + 7) / 8;

        const uint32_t layers = static_cast<uint32_t>(rendered.size());

        m_device->GetCurrentFrameRDG()
            ->AddComputePass(std::move(reduce))
            .RecordPassCommands([groups, layers](RDGPassCmdEncoder& encoder) { encoder.Dispatch(groups, groups, layers); });
    }
}

void SceneShadowRenderer::BindLightingInputs(RDGPassDescBase& pass) const
{
    pass.BindValue("uSceneShadows", m_uniforms);

    pass.BindSampledTexture("sceneShadowMaps", m_depthSampler, m_maps->GetDefaultView());

    pass.BindSampledTexture("sceneShadowMaxDepth", m_depthSampler, m_maxDepth->GetDefaultView());
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

void SceneShadowRenderer::OnRenderGraphExecuted(RenderFrameResult result)
{
    const bool succeeded = result == RenderFrameResult::eSubmitted;

    if (result != RenderFrameResult::eDeclined)
    {
        m_validContents = succeeded;

        if (succeeded)
        {
            m_committedFaces   = m_faces;
            m_committedCasters = m_recordedCasters;
        }
    }
}

void SceneShadowRenderer::Destroy()
{
    m_device->DestroyTexture(m_depth);

    m_device->DestroyTexture(m_maps);

    m_device->DestroyTexture(m_maxDepth);

    m_depth         = nullptr;

    m_maps          = nullptr;

    m_maxDepth      = nullptr;

    m_validContents = false;

    m_committedFaces.clear();
    m_committedCasters.clear();
    m_recordedCasters.clear();
    m_scene = nullptr;
}
} // namespace zen::rc
