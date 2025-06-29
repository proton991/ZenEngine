#pragma once
#include "VoxelizerBase.h"
#include "../RenderDevice.h"
#include "../RenderGraph.h"
#include "Common/UniquePtr.h"

namespace zen::rc
{
class RenderScene;

class GeometryVoxelizer : public VoxelizerBase
{
public:
    GeometryVoxelizer(RenderDevice* renderDevice, rhi::RHIViewport* viewport) :
        VoxelizerBase(renderDevice, viewport)
    {}

    void Init() final;

    void Destroy() final;

    void PrepareRenderWorkload() final;

    void OnResize() final;

protected:
    void PrepareTextures() final;

    void PrepareBuffers() final;

    void BuildRenderGraph() final;

    void BuildGraphicsPasses() final;

    void UpdatePassResources() final;

    void UpdateUniformData() final;

    rhi::BufferHandle m_voxelVBO;
    struct
    {
        GraphicsPass voxelization;
        GraphicsPass voxelDraw;
    } m_gfxPasses;
};
} // namespace zen::rc