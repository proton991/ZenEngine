#pragma once
#include "VoxelizerBase.h"
#include "../RenderDevice.h"
#include "../RenderGraph/RenderGraph.h"

namespace zen::rc
{
class RenderScene;

class GeometryVoxelizer : public VoxelizerBase
{
public:
    explicit GeometryVoxelizer(RenderDevice* pRenderDevice) : VoxelizerBase(pRenderDevice) {}

    void Init() final;

    void BuildVoxelizationGraph() final;

    void BuildVisualizationGraph(const RenderView& view) final;

    void Destroy() final;
};
} // namespace zen::rc
