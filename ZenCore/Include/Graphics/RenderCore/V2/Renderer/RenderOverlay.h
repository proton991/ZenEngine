#pragma once

namespace zen
{
class RHIViewport;
}

namespace zen::rc
{
class RenderGraph;

// Application-owned composition appended after scene passes, before submission.
// Implementations must copy transient UI data into graph-owned command packets.
class RenderOverlay
{
public:
    virtual ~RenderOverlay() = default;

    virtual bool BuildRenderGraph(RenderGraph& graph, RHIViewport& viewport) = 0;
};
} // namespace zen::rc
