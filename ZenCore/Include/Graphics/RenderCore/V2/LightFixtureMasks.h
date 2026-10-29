#pragma once
#include "Graphics/RenderCore/V2/RenderGraph/RenderGraph.h"
#include "Graphics/RenderCore/V2/RenderScene.h"
#include "Graphics/RenderCore/V2/SceneLighting.h"
#include <array>

namespace zen::rc
{
class RenderDevice;
class SceneRayQuery;
struct VoxelGIUniformData;

// Per shadowed point or spot light, a cube of directions in which every sampled ray from the light
// meets geometry within a small shell, with the distance within which it does. A ray-traced shadow
// test of a receiver beyond that distance is occluded without a ray (scene_shadows.glsl): lamps
// inside fixtures otherwise trace every pixel's ray to the same glass. A light's mask is rebuilt
// when it moves, becomes shadowed, or a shadow caster changes within its shell.
class LightFixtureMasks
{
public:
    explicit LightFixtureMasks(RenderDevice* device) : m_device(device) {}

    // Records rebuilds of changed masks; call after the frame's ray-query build, before its users.
    bool BuildRenderGraph(RenderScene& scene, const SceneRayQuery& rayQuery, const VoxelGIUniformData& gi);

    void BindInputs(RDGPassDescBase& pass) const;

    void OnRenderGraphExecuted(RenderFrameResult result);

    void Destroy();

private:
    struct LightState
    {
        Vec4 positionShell{}; // Position and shell radius.
        bool masked{false};
    };

    struct BuildUniforms
    {
        glm::uvec4 lights[MaxSceneLights / 4]{};
        Vec4       build{}; // Queued light count, shell radius.
    };

    RenderDevice*                          m_device;
    RHITexture*                            m_texels{nullptr}; // Undilated per-texel distances.
    RHITexture*                            m_mask{nullptr};
    RHISampler*                            m_sampler{nullptr};
    const RenderScene*                     m_scene{nullptr};
    std::array<LightState, MaxSceneLights> m_committed{};
    std::array<LightState, MaxSceneLights> m_recorded{};
    HeapVector<RenderScene::ShadowCaster>  m_committedCasters;
    HeapVector<RenderScene::ShadowCaster>  m_recordedCasters;
    bool                                   m_cleared{false};
    bool                                   m_recordedClear{false};
    bool                                   m_recordedFrame{false};
};
} // namespace zen::rc
