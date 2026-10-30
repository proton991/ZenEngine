#pragma once
#include "Graphics/RenderCore/V2/RenderGraph/RenderGraph.h"
#include "Graphics/RenderCore/V2/SceneLighting.h"
#include "Graphics/RenderCore/V2/RenderScene.h"
#include "SceneGraph/AABB.h"
#include "Graphics/RenderCore/V2/Renderer/DebugVisualization.h"

namespace zen::rc
{
class RenderDevice;
class RenderScene;
struct SceneMeshDraw;

constexpr uint32_t MaxSceneShadowFaces = (MaxSceneLights + 1) * 6;

struct SceneShadowUniformData
{
    Mat4 viewProjection[MaxSceneShadowFaces]{};
    // First layer, face count, inverse depth range, texel width (at unit distance for perspective).
    Vec4 lights[MaxSceneLights + 1]{}; // Last slot belongs to the camera light ball.
    Vec4 settings{};                   // Minimum world-space bias, resolution, reserved, reserved.
};
static_assert(sizeof(SceneShadowUniformData) == MaxSceneShadowFaces * 64 + (MaxSceneLights + 1) * 16 + 16);

// Mesh visibility shared by direct lighting and voxel radiance injection.
class SceneShadowRenderer
{
public:
    explicit SceneShadowRenderer(RenderDevice* device, uint32_t resolution = 0);

    // Call between frames; old maps retire through RenderDevice.
    bool SetResolution(uint32_t resolution);

    uint32_t GetResolution() const
    {
        return m_resolution;
    }

    bool Prepare(const RenderScene& scene, bool enabled, bool includeInactiveLights = false);

    // Resolution changes assume Apply drains GPU work and collects the old maps before allocation.
    // At the current resolution, face-count changes must fit alongside any in-flight maps.
    bool Preflight(uint32_t resolution, uint32_t faces) const;

    DebugOutputDescription BuildDebugView(const RenderScene& scene, const RenderView& view, const DebugSelection& selection);

    void BuildRenderGraph(const RenderScene& scene);

    void BindLightingInputs(RDGPassDescBase& pass) const;

    void OnRenderGraphExecuted(RenderFrameResult result);

    void Destroy();

private:
    struct FaceData
    {
        Mat4 viewProjection{1.0f};
        Vec4 lightPositionInvRange{};
    };

    void PrepareLight(const GPULight& light, uint32_t index, const sg::AABB& bounds);

    void BuildFace(const RenderScene& scene, uint32_t layer, const HeapVector<SceneMeshDraw>& draws);

    RenderDevice*                         m_device;
    RHITexture*                           m_depth{nullptr};
    RHITexture*                           m_maps{nullptr};
    RHITexture*                           m_maxDepth{nullptr}; // Deepest depth per 4x4 block of each layer.
    RHISampler*                           m_depthSampler{nullptr};
    RHISampler*                           m_materialSampler{nullptr};
    uint32_t                              m_resolution{1024};
    HeapVector<FaceData>                  m_faces;
    HeapVector<FaceData>                  m_committedFaces;
    SceneShadowUniformData                m_uniforms{};
    const RenderScene*                    m_scene{nullptr};
    HeapVector<RenderScene::ShadowCaster> m_committedCasters;
    HeapVector<RenderScene::ShadowCaster> m_recordedCasters;
    bool                                  m_validContents{false};
};
} // namespace zen::rc
