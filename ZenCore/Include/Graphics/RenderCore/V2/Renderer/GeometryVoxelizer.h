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
    GeometryVoxelizer(RenderDevice* pRenderDevice, RHIViewport* pViewport) :
        VoxelizerBase(pRenderDevice, pViewport)
    {}

    void Init() final;

    void BuildRenderGraph() final;

    void Destroy() final;
};
} // namespace zen::rc
