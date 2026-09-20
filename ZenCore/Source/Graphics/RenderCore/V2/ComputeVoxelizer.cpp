#include "Graphics/RenderCore/V2/Renderer/ComputeVoxelizer.h"
#include "Graphics/RenderCore/V2/Renderer/RendererUtils.h"
#include "AssetLib/FastGLTFLoader.h"
#include "Graphics/RenderCore/V2/RenderObject.h"
#include "Graphics/RenderCore/V2/RenderResource.h"
#include "Graphics/RenderCore/V2/RenderScene.h"
#include "Graphics/RenderCore/V2/ShaderProgram.h"
#include "Graphics/Val/CommandBuffer.h"
#include "Platform/ConfigLoader.h"
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
    m_voxelTexResolution = 256;
    m_voxelTexFormat     = DataFormat::eR8G8B8A8UNORM;
    m_voxelCount         = m_voxelTexResolution * m_voxelTexResolution * m_voxelTexResolution;

    PrepareTextures();

    PrepareBuffers();

    LoadCubeModel();
}

void ComputeVoxelizer::Destroy()
{
    VoxelizerBase::Destroy();
    ZEN_DELETE(m_pCube);
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
    ComputeIndirectCommand indirectCommand{};
    indirectCommand.x                = 0;
    indirectCommand.y                = 1;
    indirectCommand.z                = 1;
    m_buffers.pComputeIndirectBuffer = m_pRenderDevice->CreateIndirectBuffer(
        sizeof(ComputeIndirectCommand), reinterpret_cast<const uint8_t*>(&indirectCommand),
        "voxel_comp_indirect_buffer");

    m_buffers.pLargeTriangleBuffer = m_pRenderDevice->CreateStorageBuffer(
        sizeof(LargeTriangle) * 200000, nullptr, "large_triangle_buffer");

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

void ComputeVoxelizer::BuildRenderGraph()
{
    RenderGraph* pRDG = m_pRenderDevice->GetCurrentFrameRDG();
    VERIFY_EXPR(pRDG != nullptr && m_pScene != nullptr);

    const Vec3 center  = m_pScene->GetAABB().GetCenter();
    const float extent = m_pScene->GetAABB().GetMaxExtent();
    const Vec3 halfExtent(extent / 2.0f);
    const sg::AABB voxelAABB{center - halfExtent, center + halfExtent};
    const float scaleFactor = 2.0f / m_pCube->GetAABB().GetExtent3D().x;
    const Mat4 voxelTransform =
        glm::scale(Mat4(1.0f), Vec3(extent / m_voxelTexResolution * scaleFactor));

    if (BeginVoxelization(*pRDG, RDGQueuePreference::ePreferAsyncCompute))
    {
        const uint32_t groups = (m_voxelTexResolution + 7) / 8;
        const VoxelizationCompSP::SceneInfo sceneInfo{Vec4(voxelAABB.GetMin(), 1.0f),
                                                      Vec4(voxelAABB.GetMax(), 1.0f)};

        RDGComputePassDesc resetCompute =
            VoxelComputePass("ResetComputeIndirectSP", "ResetComputeIndirectComp");

        resetCompute.BindStorageBuffer("IndirectBuffer", m_buffers.pComputeIndirectBuffer);

        pRDG->AddComputePass(std::move(resetCompute))
            .RecordPassCommands([](RDGPassCmdEncoder& encoder) { encoder.Dispatch(1, 1, 1); });

        RDGComputePassDesc voxelization =
            VoxelComputePass("VoxelizationCompSP", "VoxelizationComp");

        RDGComputePassDesc largeTriangles =
            VoxelComputePass("VoxelizationLargeTriangleCompSP", "VoxelizationLargeTriangleComp");

        for (RDGComputePassDesc* desc : {&voxelization, &largeTriangles})
        {
            desc->BindStorageImage("voxelTexture", m_voxelTextures.pAlbedo->GetDefaultView());
            desc->BindStorageBuffer("VertexBuffer", m_pScene->GetVertexBuffer());
            desc->BindStorageBuffer("IndexBuffer", m_pScene->GetIndexBuffer());
            desc->BindStorageBuffer("NodeBuffer", m_pScene->GetNodesDataSSBO());
            desc->BindStorageBuffer("TriangleMap", m_pScene->GetTriangleMapBuffer());
            BindSceneTextureArray(*desc, m_pColorSampler, m_pScene->GetSceneTextures());
            desc->BindValue("uSceneInfo", sceneInfo);
        }

        // The producer emits exactly the records counted by command.x; indirect
        // dispatch indexes only those records, not the allocation's unused capacity.
        voxelization.BindStorageBuffer("LargeTriangleArray", m_buffers.pLargeTriangleBuffer,
                                       RDGContentGuarantee::eProducedElements);
        largeTriangles.BindStorageBuffer("LargeTriangleArray", m_buffers.pLargeTriangleBuffer,
                                         RDGContentGuarantee::eConsumeProducedElements);
        voxelization.BindStorageBuffer("IndirectBuffer", m_buffers.pComputeIndirectBuffer);

        for (sg::Node* node : m_pScene->GetRenderableNodes())
        {
            const uint32_t triangleCount = node->GetComponent<sg::Mesh>()->GetNumIndices() / 3;
            VERIFY_EXPR(triangleCount == m_pScene->GetNumIndices() / 3);

            const VoxelizationCompSP::PushConstantsData constants{node->GetRenderableIndex(),
                                                                  triangleCount, 15};

            // Each node owns a pass so shared writes remain visible to RDG.
            pRDG->AddComputePass(voxelization)
                .RecordPassCommands([constants](RDGPassCmdEncoder& encoder) {
                    encoder.SetPushConstants(constants);
                    encoder.Dispatch((constants.triangleCount + 31) / 32, 1, 1);
                });
        }

        largeTriangles.UseIndirectBuffer(m_buffers.pComputeIndirectBuffer);

        pRDG->AddComputePass(std::move(largeTriangles))
            .RecordPassCommands(
                [indirect = m_buffers.pComputeIndirectBuffer](RDGPassCmdEncoder& encoder) {
                    encoder.DispatchIndirect(indirect, 0);
                });

        RDGComputePassDesc resetDraw =
            VoxelComputePass("ResetDrawIndirectSP", "ResetDrawIndirectComp");

        resetDraw.BindStorageBuffer("IndirectBuffer", m_buffers.pDrawIndirectBuffer);

        pRDG->AddComputePass(std::move(resetDraw))
            .RecordPassCommands([](RDGPassCmdEncoder& encoder) { encoder.Dispatch(1, 1, 1); });

        RDGComputePassDesc preDraw = VoxelComputePass("VoxelPreDrawSP", "VoxelPreDrawComp");

        preDraw.BindStorageImage("voxelTexture", m_voxelTextures.pAlbedo->GetDefaultView());
        preDraw.BindStorageBuffer("InstancePositionBuffer", m_buffers.pInstancePositionBuffer,
                                  RDGContentGuarantee::eProducedElements);
        preDraw.BindStorageBuffer("InstanceColorBuffer", m_buffers.pInstanceColorBuffer,
                                  RDGContentGuarantee::eProducedElements);
        preDraw.BindStorageBuffer("IndirectBuffer", m_buffers.pDrawIndirectBuffer);
        preDraw.BindValue("uSceneInfo", sceneInfo);

        pRDG->AddComputePass(std::move(preDraw))
            .RecordPassCommands(
                [groups](RDGPassCmdEncoder& encoder) { encoder.Dispatch(groups, groups, groups); });
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
