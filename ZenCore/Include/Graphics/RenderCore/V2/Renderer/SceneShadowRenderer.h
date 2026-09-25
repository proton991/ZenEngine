#pragma once
#include "Graphics/RenderCore/V2/RenderGraph/RenderGraph.h"
#include "Graphics/RenderCore/V2/SceneLighting.h"
#include "SceneGraph/AABB.h"

namespace zen::rc
{
class RenderDevice;
class RenderScene;

constexpr uint32_t MaxSceneShadowFaces = MaxSceneLights * 6;

struct SceneShadowUniformData
{
    Mat4 viewProjection[MaxSceneShadowFaces]{};
    // First layer, face count, inverse depth range, texel width (at unit distance for perspective).
    Vec4 lights[MaxSceneLights]{};
    Vec4 settings{}; // Minimum world-space bias, resolution, reserved, reserved.
};
static_assert(sizeof(SceneShadowUniformData) ==
              MaxSceneShadowFaces * 64 + MaxSceneLights * 16 + 16);

// Mesh visibility shared by direct lighting and directional-GI sender lighting.
class SceneShadowRenderer
{
public:
    explicit SceneShadowRenderer(RenderDevice* device);
    bool Prepare(const RenderScene& scene, bool enabled, bool includeInactiveLights = false);
    void BuildRenderGraph(const RenderScene& scene, uint64_t geometryRevision);
    void BindLightingInputs(RDGPassDescBase& pass) const;
    void OnRenderGraphExecuted(bool succeeded);
    void Destroy();

private:
    struct FaceData
    {
        Mat4 viewProjection{1.0f};
        Vec4 lightPositionInvRange{};
    };

    void PrepareLight(const GPULight& light, uint32_t index, const sg::AABB& bounds);
    void BuildFace(const RenderScene& scene, uint32_t layer, bool drawGeometry);

    RenderDevice* m_device;
    RHITexture* m_depth{nullptr};
    RHITexture* m_maps{nullptr};
    RHISampler* m_depthSampler{nullptr};
    RHISampler* m_materialSampler{nullptr};
    uint32_t m_resolution{1024};
    HeapVector<FaceData> m_faces;
    HeapVector<FaceData> m_committedFaces;
    SceneShadowUniformData m_uniforms{};
    uint64_t m_geometryRevision{0};
    uint64_t m_recordedGeometry{0};
    bool m_validContents{false};
};
} // namespace zen::rc
