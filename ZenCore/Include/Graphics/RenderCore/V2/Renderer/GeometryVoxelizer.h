#pragma once
#include "VoxelizerBase.h"
#include "../RenderDevice.h"
#include "../RenderGraph.h"

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

    RHIBuffer* m_pVoxelVBO;
    struct
    {
        GraphicsPass* pVoxelization;
        GraphicsPass* pVoxelDraw;
    } m_gfxPasses;
};
} // namespace zen::rc