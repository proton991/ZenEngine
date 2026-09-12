#pragma once
#include "VoxelizerBase.h"
#include "../RenderDevice.h"
#include "../RenderGraph/RenderGraph.h"
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
    ComputeVoxelizer(RenderDevice* pRenderDevice, RHIViewport* pViewport) :
        VoxelizerBase(pRenderDevice, pViewport)
    {}

    void Init() final;

    void BuildRenderGraph() final;

    void Destroy() final;

protected:
#ifdef ZEN_MACOS
    void WarmupTextureAllocation();
#endif

    void LoadCubeModel();

    void PrepareTextures() final;

    void PrepareBuffers() final;

    struct LargeTriangle
    {
        uint32_t triangleIndex{0};
        uint32_t innerTriangleIndex{0};
        Mat4 modelMatrix{1.0f};
    };

    struct
    {
        RHIBuffer* pComputeIndirectBuffer;
        RHIBuffer* pLargeTriangleBuffer;
        RHIBuffer* pInstancePositionBuffer;
        RHIBuffer* pInstanceColorBuffer;
        RHIBuffer* pDrawIndirectBuffer;
    } m_buffers;

    RenderObject* m_pCube;
};
} // namespace zen::rc
