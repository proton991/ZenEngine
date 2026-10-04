#pragma once
#include "Graphics/RenderCore/V2/RenderingSettings.h"
#include "Graphics/RenderCore/V2/RenderView.h"
#include "Graphics/RenderCore/V2/RenderGraph/RenderGraph.h"

namespace zen::rc
{
struct DebugOutputDescription
{
    DebugOutput output{DebugOutput::eFinal};
    bool        available{false};
    uint32_t    width{0};
    uint32_t    height{0};
    uint32_t    depth{1};
    uint32_t    mipCount{1};
    uint32_t    faceCount{1};
    DataFormat  format{DataFormat::eUndefined};
    std::string interpretation;
    std::string reason{"No rendered scene."};
};

// Bindings refer only to resources in this frame. The persistent view color
// receives the visualization; no transient image is exposed to a frontend.
struct DebugVisualizationData
{
    Mat4       inverseProjection{1.0f};
    Vec4       range{0.0f, 1.0f, 0.0f, 0.0f}; // min, max, linear depth, reserved
    glm::uvec4 selection{0};                  // interpretation, layer, axis, slice
};

RDGGraphicsPassDesc MakeDebugVisualizationPass(const RenderView& view, NameID shader, const DebugVisualizationData& data);

void AddDebugVisualizationPass(RenderGraph& graph, RDGGraphicsPassDesc pass);
} // namespace zen::rc
