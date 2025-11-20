#pragma once
#include "VoxelizerBase.h"
#include "../RenderDevice.h"
#include "../RenderGraph.h"
#include "SceneGraph/AABB.h"

#ifdef ZEN_MACOS
#    define NUM_DUMMY_TEXTURES 6
#endif

namespace zen::rc
{
class RenderScene;
class RenderObject;

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

    void SetRenderScene(RenderScene* scene) override;

protected:
#ifdef ZEN_MACOS
    void WarmupTextureAllocation();
#endif

    void LoadCubeModel();

    void PrepareTextures() final;

    void PrepareBuffers() final;

    void BuildRenderGraph() final;

    void BuildGraphicsPasses() final;

    void BuildComputePasses() final;

    void UpdatePassResources() final;

    void UpdateUniformData() final;

    struct LargeTriangle
    {
        uint32_t triangleIndex{0};
        uint32_t innerTriangleIndex{0};
        Mat4 modelMatrix{1.0f};
    };

    struct
    {
        // voxelization pass
        rhi::RHIBuffer* computeIndirectBuffer;
        rhi::RHIBuffer* largeTriangleBuffer;
        // voxel pre-draw pass
        rhi::RHIBuffer* instancePositionBuffer;
        rhi::RHIBuffer* instanceColorBuffer;
        rhi::RHIBuffer* drawIndirectBuffer;
    } m_buffers;

    struct
    {
        ComputePass resetVoxelTexture;
        ComputePass resetComputeIndirect;
        ComputePass resetDrawIndirect;
        ComputePass voxelization;
        ComputePass voxelizationLargeTriangle;
        ComputePass voxelPreDraw; // calculate position and color for voxel draw pass
    } m_computePasses;

    struct
    {
        GraphicsPass voxelDraw;
    } m_gfxPasses;

    RenderObject* m_cube;

    Mat4 m_voxelTransform;
    sg::AABB m_voxelAABB;
};
} // namespace zen::rc