#pragma once
#include "VoxelizerBase.h"
#include "../RenderDevice.h"
#include "../RenderGraph.h"

namespace zen::rc
{
class RenderScene;
class RenderObject;

// todo: For voxel albedo, color output only occupies upper half of the texture. Maybe related to flickering issue?
class ComputeVoxelizer : public VoxelizerBase
{
public:
    ComputeVoxelizer(RenderDevice* renderDevice, rhi::RHIViewport* viewport) :
        VoxelizerBase(renderDevice, viewport)
    {}

    void Init() final;

    void Destroy() final;

    void PrepareRenderWorkload() final;

    void OnResize() final;

protected:
    void LoadCubeModel();

    void PrepareTextures() final;

    void PrepareBuffers() final;

    void BuildRenderGraph() final;

    void BuildGraphicsPasses() final;

    void BuildComputePasses() final;

    void UpdatePassResources() final;

    void UpdateUniformData() final;

    struct
    {
        // voxelization pass
        rhi::BufferHandle computeIndirectBuffer;
        rhi::BufferHandle largeTriangleBuffer;
        // voxel pre-draw pass
        rhi::BufferHandle instancePositionBuffer;
        rhi::BufferHandle instanceColorBuffer;
        rhi::BufferHandle drawIndirectBuffer;
    } m_buffers;

    struct
    {
        ComputePass resetVoxelTexture;
        ComputePass resetDrawIndirect;
        ComputePass voxelization;
        ComputePass voxelPreDraw; // calculate position and color for voxel draw pass
    } m_computePasses;

    struct
    {
        GraphicsPass voxelDraw;
    } m_gfxPasses;

    RenderObject* m_cube;
};
} // namespace zen::rc