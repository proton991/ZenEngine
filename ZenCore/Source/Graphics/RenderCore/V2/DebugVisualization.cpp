#include "Graphics/RenderCore/V2/Renderer/DebugVisualization.h"

namespace zen::rc
{
RDGGraphicsPassDesc MakeDebugVisualizationPass(const RenderView& view, NameID shader, const DebugVisualizationData& data)
{
    RDGGraphicsPassDesc pass;

    pass.SetPassTag("RenderDebugVisualization");

    pass.SetShaderProgramName(shader);

    RHIGfxPipelineStates states{};

    states.rasterizationState.cullMode = RHIPolygonCullMode::eDisabled;

    states.depthStencilState = RHIGfxPipelineDepthStencilState::Create(false, false, RHIDepthCompareOperator::eAlways);

    states.colorBlendState.AddAttachment();

    states.dynamicStates.Enable(RHIDynamicState::eScissor, RHIDynamicState::eViewPort);

    pass.SetPipelineStates(states);

    pass.SetRenderArea(0, 0, view.width, view.height);

    pass.AddColorOutput(view.color, RHIRenderTargetLoadOp::eClear);

    pass.BindValue("uDebugData", data);

    return pass;
}

void AddDebugVisualizationPass(RenderGraph& graph, RDGGraphicsPassDesc pass)
{
    graph.AddGraphicsPass(std::move(pass)).RecordPassCommands([](RDGPassCmdEncoder& encoder) { encoder.Draw(3, 1, 0, 0); });
}
} // namespace zen::rc
