#include "Graphics/RenderCore/V2/LightFixtureMasks.h"
#include "Graphics/RenderCore/V2/RenderDevice.h"
#include "Graphics/RenderCore/V2/SceneRayQuery.h"
#include "Graphics/RenderCore/V2/Renderer/VoxelGIRenderer.h"

namespace zen::rc
{
namespace
{
// LIGHT_FIXTURE_MASK_SIZE and LIGHT_FIXTURE_MASK_OPEN (light_fixture_mask.glsl).
constexpr uint32_t kMaskSize = 64;
constexpr float    kOpen     = 1e30f;
// Shell radius as a share of the scene's largest extent: New Sponza's lanterns fit well inside.
constexpr float kShellShare = 0.025f;

bool TouchesShell(const sg::AABB& bounds, const Vec4& positionShell)
{
    const Vec3 center(positionShell);
    const Vec3 offset = glm::clamp(center, bounds.GetMin(), bounds.GetMax()) - center;
    return glm::dot(offset, offset) <= positionShell.w * positionShell.w;
}

RHITexture* CreateMaskTexture(RenderDevice& device, const char* tag)
{
    RHITextureCreateInfo info;
    info.type        = RHITextureType::e2D;
    info.format      = DataFormat::eR32SFloat;
    info.width       = kMaskSize;
    info.height      = kMaskSize;
    info.arrayLayers = MaxSceneLights * 6;
    info.tag         = tag;
    info.usageFlags.SetFlags(RHITextureUsageFlagBits::eStorage, RHITextureUsageFlagBits::eSampled,
                             RHITextureUsageFlagBits::eTransferDst);
    return device.CreateTexture(info);
}
} // namespace

bool LightFixtureMasks::BuildRenderGraph(RenderScene& scene, const SceneRayQuery& rayQuery, const VoxelGIUniformData& gi)
{
    if (m_mask == nullptr)
    {
        m_texels  = CreateMaskTexture(*m_device, "light_fixture_texels");
        m_mask    = CreateMaskTexture(*m_device, "light_fixture_mask");
        m_sampler = m_device->CreateSampler({});
        m_cleared = false;
    }
    const bool valid = m_texels != nullptr && m_mask != nullptr && m_sampler != nullptr;
    m_recordedFrame  = false;
    m_recordedClear  = false;
    if (valid)
    {
        RenderGraph& graph = *m_device->GetCurrentFrameRDG();
        if (m_scene != &scene || !m_cleared)
        {
            m_scene     = &scene;
            m_committed = {};
            m_committedCasters.clear();
        }
        if (!m_cleared)
        {
            // Unbuilt lights read open texels: every receiver is traced. Builds then rewrite only
            // their own lights' layers of both textures.
            graph.AddTransferPass("ClearLightFixtureMask").ClearTexture(m_mask, Color(kOpen));
            graph.AddTransferPass("ClearLightFixtureTexels").ClearTexture(m_texels, Color(kOpen));
            m_recordedClear = true;
        }

        // Casters added, removed, moved or deformed since the committed masks were built.
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
                if (current)
                {
                    changedBounds.push_back(m_recordedCasters[index].bounds);
                }
            }
        }

        const SceneUniformData& data  = *reinterpret_cast<const SceneUniformData*>(scene.GetSceneUniformData());
        const float             shell = std::max(scene.GetAABB().GetMaxExtent(), 0.001f) * kShellShare;
        BuildUniforms           uniforms;
        uint32_t                count = 0;
        for (uint32_t index = 0; index < MaxSceneLights; ++index)
        {
            const GPULight& light = data.lights[index];
            LightState      state;
            state.masked        = index < static_cast<uint32_t>(data.lightInfo.x)
                               && static_cast<SceneLightType>(light.directionType.w) != SceneLightType::eDirectional
                               && light.coneShadow.z > 0.0f;
            state.positionShell = Vec4(Vec3(light.positionRange), shell);
            bool rebuild =
                state.masked && (!m_committed[index].masked || m_committed[index].positionShell != state.positionShell);
            for (size_t bounds = 0; state.masked && !rebuild && bounds < changedBounds.size(); ++bounds)
            {
                rebuild = TouchesShell(changedBounds[bounds], state.positionShell);
            }
            if (rebuild)
            {
                uniforms.lights[count / 4][count % 4] = index;
                ++count;
            }
            m_recorded[index] = state;
        }

        if (count != 0)
        {
            uniforms.build        = Vec4(static_cast<float>(count), shell, 0.0f, 0.0f);
            const uint32_t groups = kMaskSize / 8;

            RDGComputePassDesc trace;
            trace.SetShaderProgramName("LightFixtureMaskBuildSP");
            trace.SetPassTag("LightFixtureMaskBuild");
            trace.BindValue("uGISettings", gi);
            trace.BindValue("uSceneData", scene.GetSceneUniformData(), sizeof(SceneUniformData));
            rayQuery.BindInputs(trace, scene);
            trace.BindStorageImage("lightFixtureTexels", m_texels->GetDefaultView());
            trace.BindValue("uLightFixtureBuild", uniforms);
            graph.AddComputePass(std::move(trace)).RecordPassCommands([groups, count](RDGPassCmdEncoder& encoder) {
                encoder.Dispatch(groups, groups, 6 * count);
            });

            RDGComputePassDesc dilate;
            dilate.SetShaderProgramName("LightFixtureMaskDilateSP");
            dilate.SetPassTag("LightFixtureMaskDilate");
            dilate.BindSampledTexture("lightFixtureTexels", m_sampler, m_texels->GetDefaultView());
            dilate.BindStorageImage("lightFixtureMask", m_mask->GetDefaultView());
            dilate.BindValue("uLightFixtureBuild", uniforms);
            graph.AddComputePass(std::move(dilate)).RecordPassCommands([groups, count](RDGPassCmdEncoder& encoder) {
                encoder.Dispatch(groups, groups, 6 * count);
            });
        }
        m_recordedFrame = true;
    }
    return valid;
}

void LightFixtureMasks::BindInputs(RDGPassDescBase& pass) const
{
    VERIFY_EXPR(m_mask != nullptr);
    pass.BindSampledTexture("lightFixtureMask", m_sampler, m_mask->GetDefaultView());
}

void LightFixtureMasks::OnRenderGraphExecuted(RenderFrameResult result)
{
    if (m_recordedFrame && result == RenderFrameResult::eSubmitted)
    {
        m_committed        = m_recorded;
        m_committedCasters = m_recordedCasters;
        m_cleared          = m_cleared || m_recordedClear;
    }
    else if (m_recordedFrame && result != RenderFrameResult::eDeclined)
    {
        // Contents of a failed frame are unknown: clear and rebuild every mask.
        m_cleared = false;
    }
    m_recordedFrame = false;
}

void LightFixtureMasks::Destroy()
{
    m_device->DestroyTexture(m_texels);
    m_device->DestroyTexture(m_mask);
    m_texels    = nullptr;
    m_mask      = nullptr;
    m_cleared   = false;
    m_scene     = nullptr;
    m_committed = {};
    m_committedCasters.clear();
}
} // namespace zen::rc
