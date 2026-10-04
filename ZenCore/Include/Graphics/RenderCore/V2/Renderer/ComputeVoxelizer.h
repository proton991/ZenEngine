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
    explicit ComputeVoxelizer(RenderDevice* pRenderDevice) : VoxelizerBase(pRenderDevice) {}

    void Init() final;

    void BuildVoxelizationGraph() final;

    void BuildVisualizationGraph(const RenderView& view) final;

    void OnRenderGraphExecuted(bool succeeded) final;

    void Destroy() final;

protected:
#ifdef ZEN_MACOS
    void WarmupTextureAllocation();
#endif

    void LoadCubeModel();

    void PrepareTextures() final;

    void PrepareBuffers() final;

    struct
    {
        RHIBuffer* pInstancePositionBuffer;
        RHIBuffer* pInstanceColorBuffer;
        RHIBuffer* pDrawIndirectBuffer;
    } m_buffers{};

    RenderObject* m_pCube{nullptr};
    uint64_t      m_visualizationRevision{0};
};
} // namespace zen::rc
