#include "Graphics/RenderCore/V2/Renderer/ComputeVoxelizer.h"
#include "Graphics/RenderCore/V2/Renderer/RendererUtils.h"
#include "AssetLib/FastGLTFLoader.h"
#include "Graphics/RenderCore/V2/RenderObject.h"
#include "Graphics/RenderCore/V2/RenderResource.h"
#include "Graphics/RenderCore/V2/RenderScene.h"
#include "Graphics/RenderCore/V2/ShaderProgram.h"
#include "Graphics/Val/CommandBuffer.h"
#include "Platform/ConfigLoader.h"
#include "Graphics/RenderCore/V2/RenderConfig.h"
#include "SceneGraph/Scene.h"
#include "SceneGraph/Camera.h"

namespace zen::rc
{
namespace
{
RDGComputePassDesc VoxelComputePass(NameID shader, NameID tag)
{
    RDGComputePassDesc pass;
    pass.SetShaderProgramName(shader);
    pass.SetPassTag(tag);
    pass.SetQueuePreference(RDGQueuePreference::ePreferAsyncCompute);
    return pass;
}
} // namespace

void ComputeVoxelizer::Init()
{
    m_voxelTexResolution = platform::ConfigLoader::GetInstance().GetVoxelResolution();
    m_voxelTexFormat     = DataFormat::eR8G8B8A8UNORM;
    m_voxelCount         = m_voxelTexResolution * m_voxelTexResolution * m_voxelTexResolution;
}

void ComputeVoxelizer::Destroy()
{
    VoxelizerBase::Destroy();
    if (m_pCube != nullptr)
    {
        ZEN_DELETE(m_pCube);
    }
}

void ComputeVoxelizer::OnRenderGraphExecuted(bool succeeded)
{
    VoxelizerBase::OnRenderGraphExecuted(succeeded);
    if (!succeeded)
    {
        m_visualizationRevision = 0;
    }
}

void ComputeVoxelizer::LoadCubeModel()
{
    m_pCube = ZEN_NEW() RenderObject(m_pRenderDevice,
                                     platform::ConfigLoader::GetInstance().GetGLTFModelPath("Box"));
}

void ComputeVoxelizer::PrepareTextures()
{
    VoxelizerBase::PrepareTextures();
#ifdef ZEN_MACOS
    WarmupTextureAllocation();
#endif
}

void ComputeVoxelizer::PrepareBuffers()
{
    m_buffers.pInstancePositionBuffer = m_pRenderDevice->CreateStorageBuffer(
        sizeof(Vec4) * 3000000, nullptr, "instance_position_buffer");

    m_buffers.pInstanceColorBuffer = m_pRenderDevice->CreateStorageBuffer(
        sizeof(Vec4) * 3000000, nullptr, "instance_color_buffer");

    DrawIndexedIndirectCommand drawIndirectCmd{};
    drawIndirectCmd.instanceCount = 1;
    drawIndirectCmd.firstInstance = 0;
    drawIndirectCmd.indexCount    = 36;
    drawIndirectCmd.firstIndex    = 0;
    drawIndirectCmd.vertexOffset  = 0;

    m_buffers.pDrawIndirectBuffer = m_pRenderDevice->CreateIndirectBuffer(
        sizeof(DrawIndexedIndirectCommand), reinterpret_cast<const uint8_t*>(&drawIndirectCmd),
        "voxel_draw_indirect_buffer");
}

void ComputeVoxelizer::BuildVoxelizationGraph()
{
    RenderGraph* graph = m_pRenderDevice->GetCurrentFrameRDG();
    if (BeginVoxelization(*graph, RDGQueuePreference::ePreferAsyncCompute))
    {
        if (m_pScene->GetVoxelTriangleCount() != 0)
        {
            RDGComputePassDesc pass = VoxelComputePass(
                UsesAveragedReflectance() ? "VoxelizationCompAveragedSP" : "VoxelizationCompSP",
                "VoxelizationComp");
            BindVoxelScene(pass);
            BindReflectanceSums(pass);
            pass.BindStorageImage("voxelOwner", m_voxelTextures.pOwner->GetDefaultView());
            const uint32_t count = m_pScene->GetVoxelTriangleCount();
            // Dispatch count is bounded by Vulkan's minimum supported X workgroup limit.
            for (uint32_t first = 0; first < count; first += 65535)
            {
                const glm::uvec2 constants(first, count);
                const uint32_t groups = std::min(65535u, count - first);
                graph->AddComputePass(pass).RecordPassCommands(
                    [constants, groups](RDGPassCmdEncoder& encoder) {
                        encoder.SetPushConstants(constants);
                        encoder.Dispatch(groups, 1, 1);
                    });
            }
        }
        ResolveSurface(RDGQueuePreference::ePreferAsyncCompute);
        BuildCompaction(RDGQueuePreference::ePreferAsyncCompute);
    }
}

void ComputeVoxelizer::BuildVisualizationGraph()
{
    if (m_pCube == nullptr)
    {
        PrepareBuffers();
        LoadCubeModel();
    }
    RenderGraph* pRDG         = m_pRenderDevice->GetCurrentFrameRDG();
    const sg::AABB voxelAABB  = GetVoxelBounds();
    const float scaleFactor   = 1.0f / m_pCube->GetAABB().GetExtent3D().x;
    const Mat4 voxelTransform = glm::scale(Mat4(1.0f), Vec3(GetVoxelSize() * scaleFactor));
    if (m_visualizationRevision != GetRecordedGeometryRevision())
    {
        m_visualizationRevision = GetRecordedGeometryRevision();
        const glm::uvec3 groups =
            GetVoxelVolumeDispatchGroups(m_voxelTexResolution, m_pRenderDevice->GetGPUInfo());
        const VoxelizationCompSP::SceneInfo sceneInfo{Vec4(voxelAABB.GetMin(), 1.0f),
                                                      Vec4(voxelAABB.GetMax(), 1.0f)};
        RDGComputePassDesc resetDraw =
            VoxelComputePass("ResetDrawIndirectSP", "ResetDrawIndirectComp");

        resetDraw.BindStorageBuffer("IndirectBuffer", m_buffers.pDrawIndirectBuffer);

        pRDG->AddComputePass(std::move(resetDraw))
            .RecordPassCommands([](RDGPassCmdEncoder& encoder) { encoder.Dispatch(1, 1, 1); });

        RDGComputePassDesc preDraw = VoxelComputePass("VoxelPreDrawSP", "VoxelPreDrawComp");

        preDraw.BindStorageImage("voxelTexture", m_voxelTextures.pAlbedoView);
        preDraw.BindStorageBuffer("InstancePositionBuffer", m_buffers.pInstancePositionBuffer,
                                  RDGContentGuarantee::eProducedElements);
        preDraw.BindStorageBuffer("InstanceColorBuffer", m_buffers.pInstanceColorBuffer,
                                  RDGContentGuarantee::eProducedElements);
        preDraw.BindStorageBuffer("IndirectBuffer", m_buffers.pDrawIndirectBuffer);
        preDraw.BindValue("uSceneInfo", sceneInfo);

        pRDG->AddComputePass(std::move(preDraw))
            .RecordPassCommands([groups](RDGPassCmdEncoder& encoder) {
                encoder.Dispatch(groups.x, groups.y, groups.z);
            });
    }

    RHIGfxPipelineStates pso{};
    pso.rasterizationState           = {};
    pso.rasterizationState.cullMode  = RHIPolygonCullMode::eDisabled;
    pso.rasterizationState.frontFace = RHIPolygonFrontFace::eCounterClockWise;

    pso.depthStencilState =
        RHIGfxPipelineDepthStencilState::Create(true, true, RHIDepthCompareOperator::eLess);
    pso.multiSampleState = {};
    pso.colorBlendState.AddAttachment();
    pso.dynamicStates.Enable(RHIDynamicState::eScissor, RHIDynamicState::eViewPort);

    RDGGraphicsPassDesc draw{};
    draw.SetShaderProgramName("VoxelDrawSP2");
    draw.SetPipelineStates(pso);
    draw.AddColorOutput(m_pViewport->GetColorBackBuffer(), RHIRenderTargetLoadOp::eLoad);
    draw.AddDepthStencilOutput(m_pViewport->GetDepthStencilBackBuffer(),
                               RHIRenderTargetLoadOp::eClear, RHIRenderTargetStoreOp::eStore);
    draw.SetRenderArea(0, 0, m_pViewport->GetWidth(), m_pViewport->GetHeight());
    draw.SetPassTag("VoxelDraw2");

    draw.BindVertexBuffer(m_pCube->GetVertexBuffer());
    draw.BindIndexBuffer(m_pCube->GetIndexBuffer());

    // Draw instanceCount is emitted by preDraw alongside both instance streams.
    draw.BindStorageBuffer("InstanceBuffer", m_buffers.pInstancePositionBuffer,
                           RDGContentGuarantee::eConsumeProducedElements);
    draw.BindStorageBuffer("InstanceColorBuffer", m_buffers.pInstanceColorBuffer,
                           RDGContentGuarantee::eConsumeProducedElements);
    draw.UseIndirectBuffer(m_buffers.pDrawIndirectBuffer);

    const VoxelDrawSP2::TransformData transform{voxelTransform,
                                                m_pScene->GetCamera()->GetViewMatrix(),
                                                m_pScene->GetCamera()->GetProjectionMatrix()};
    draw.BindValue("uTransformData", transform);

    pRDG->AddGraphicsPass(std::move(draw))
        .RecordPassCommands([indirect = m_buffers.pDrawIndirectBuffer](RDGPassCmdEncoder& encoder) {
            encoder.DrawIndexedIndirect(indirect, 0, 1, sizeof(DrawIndexedIndirectCommand));
        });
}

#ifdef ZEN_MACOS
void ComputeVoxelizer::WarmupTextureAllocation()
{
    const uint32_t halfDim = m_voxelTexResolution / 2;

    for (uint32_t i = 0; i < NUM_DUMMY_TEXTURES; i++)
    {
        const NameID texName(fmt::format("dummy_texture_{}", i));
        TextureFormat texFormat{};
        texFormat.format      = m_voxelTexFormat;
        texFormat.dimension   = TextureDimension::e3D;
        texFormat.width       = halfDim;
        texFormat.height      = halfDim;
        texFormat.depth       = halfDim;
        texFormat.arrayLayers = 1;
        texFormat.mipmaps     = 1;

        RHITexture* pDummyTex =
            m_pRenderDevice->CreateTextureDummy(texFormat, {.copyUsage = false}, texName);
        m_pRenderDevice->DestroyTexture(pDummyTex);
    }
}
#endif
} // namespace zen::rc
