#include "Graphics/RenderCore/V2/ScenePresentation.h"
#include "Graphics/RenderCore/V2/RenderDevice.h"
#include "Graphics/RenderCore/V2/ShaderProgram.h"
#include <algorithm>

namespace zen::rc
{
bool SceneRenderTarget::Resize(uint32_t width, uint32_t height)
{
    const bool changed = width != m_view.width || height != m_view.height;
    const bool valid   = ResizeSceneView(m_device, m_view, width, height);
    if (valid && changed)
    {
        ++m_revision;
        m_completed = false;
    }
    return valid;
}

void SceneRenderTarget::Destroy()
{
    DestroySceneView(m_device, m_view);
    m_completed = false;
    ++m_revision;
}

void DestroySceneView(RenderDevice& device, RenderView& view)
{
    device.DestroyTexture(view.color);
    device.DestroyTexture(view.depth);
    view = {};
}

bool ResizeSceneView(RenderDevice& device, RenderView& view, uint32_t width, uint32_t height)
{
    bool valid = width > 0 && height > 0 && width <= 8192 && height <= 8192;
    if (valid && (view.width != width || view.height != height))
    {
        TextureFormat format;
        format.width      = width;
        format.height     = height;
        format.depth      = 1;
        format.format     = DataFormat::eR8G8B8A8UNORM;
        RHITexture* color = device.CreateTextureColorRT(format, {.copyUsage = true}, "SceneColor");
        format.format     = DataFormat::eD32SFloat;
        RHITexture* depth = device.CreateTextureDepthStencilRT(format, {}, "SceneDepth");
        valid             = color != nullptr && depth != nullptr;
        if (valid)
        {
            const uint64_t history = view.historyId;
            DestroySceneView(device, view);
            view = {color, depth, width, height, history};
        }
        else
        {
            device.DestroyTexture(color);
            device.DestroyTexture(depth);
        }
    }
    return valid;
}

bool SceneCompositor::Init(RenderDevice& device)
{
    ShaderProgramManager& programs = ShaderProgramManager::GetInstance();
    ShaderProgram*        program  = programs.RequestShaderProgram("SceneCompositorSP");
    if (program == nullptr)
    {
        program = ZEN_NEW()
            HybridGraphicsSP(&device, "SceneCompositorSP", "SceneRenderer/deferred.vert.spv", "Runtime/composite.frag.spv");
        programs.StoreProgram(program);
    }
    // Clamp so scaled presentation never blends the opposite edge into border pixels.
    RHISamplerCreateInfo sampler{};
    sampler.minFilter = RHISamplerFilter::eLinear;
    sampler.magFilter = RHISamplerFilter::eLinear;
    m_sampler         = device.CreateSampler(sampler);
    return program->GetShader() != nullptr && m_sampler != nullptr;
}

bool SceneCompositor::BuildRenderGraph(RenderGraph& graph, const RenderView& destination, const RenderView& scene)
{
    const bool valid = m_sampler != nullptr && scene.color != nullptr && scene.width > 0 && scene.height > 0
                    && destination.color != nullptr && destination.width > 0 && destination.height > 0;
    if (valid)
    {
        const float         width  = static_cast<float>(destination.width);
        const float         height = static_cast<float>(destination.height);
        const float         scale  = std::min(width / scene.width, height / scene.height);
        const Vec2          size(scale * scene.width / width, scale * scene.height / height);
        const Vec4          rectangle((Vec2(1.0f) - size) * 0.5f, size);
        RDGGraphicsPassDesc pass;
        pass.SetPassTag("ScenePresentation");
        pass.SetShaderProgramName("SceneCompositorSP");
        RHIGfxPipelineStates states{};
        states.rasterizationState.cullMode = RHIPolygonCullMode::eDisabled;
        states.depthStencilState = RHIGfxPipelineDepthStencilState::Create(false, false, RHIDepthCompareOperator::eAlways);
        states.colorBlendState.AddAttachment();
        states.dynamicStates.Enable(RHIDynamicState::eScissor, RHIDynamicState::eViewPort);
        pass.SetPipelineStates(states);
        pass.SetRenderArea(0, 0, destination.width, destination.height);
        pass.AddColorOutput(destination.color, RHIRenderTargetLoadOp::eClear);
        pass.BindValue("uPresentation", rectangle);
        pass.BindSampledTexture("sceneColor", m_sampler, scene.color->GetDefaultView());
        graph.AddGraphicsPass(std::move(pass)).RecordPassCommands([](RDGPassCmdEncoder& encoder) { encoder.Draw(3, 1, 0, 0); });
    }
    return valid;
}
} // namespace zen::rc
