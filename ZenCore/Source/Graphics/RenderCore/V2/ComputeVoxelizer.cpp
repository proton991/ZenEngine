#include "Graphics/RenderCore/V2/Renderer/ComputeVoxelizer.h"
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
void ComputeVoxelizer::Init()
{
    m_voxelTexResolution = 256;
    m_voxelTexFormat     = DataFormat::eR8G8B8A8UNORM;
    m_voxelCount         = m_voxelTexResolution * m_voxelTexResolution * m_voxelTexResolution;

    PrepareTextures();

    PrepareBuffers();

    BuildGraphicsPasses();

    BuildComputePasses();

    LoadCubeModel();
}

void ComputeVoxelizer::Destroy()
{
    VoxelizerBase::Destroy();
    ZEN_DELETE(m_pCube);
}

void ComputeVoxelizer::LoadCubeModel()
{
    m_pCube = ZEN_NEW()
        RenderObject(m_pRenderDevice, platform::ConfigLoader::GetInstance().GetGLTFModelPath("Box"));
}

void ComputeVoxelizer::PrepareTextures()
{
    VoxelizerBase::PrepareTextures();
    // WA for MoltenVK on MacOS.
#ifdef ZEN_MACOS
    WarmupTextureAllocation();
#endif
}

void ComputeVoxelizer::PrepareBuffers()
{
    ComputeIndirectCommand indirectCommand{};
    indirectCommand.x               = 0;
    indirectCommand.y               = 1;
    indirectCommand.z               = 1;
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

// static glm::mat4 get_model()
// {
//     auto model = glm::mat4(1.0f);
//     model      = glm::scale(model, glm::vec3(1.0f));
//     model      = glm::mat4_cast(rotation) * model;
//     model      = glm::translate(model, position);
//     return model;
// }

void ComputeVoxelizer::BuildRenderGraph()
{
    VERIFY_EXPR(m_pScene != nullptr);
    m_rdg = MakeUnique<RenderGraph>("comp_voxelize_rdg");
    m_rdg->Begin();
    int workgroupCount;
    // voxelization pPass
    if (m_needVoxelization)
    {
        // TextureHandle textures[] = {
        //     // m_voxelTextures.staticFlag,
        //     m_voxelTextures.pAlbedo,
        //     // m_voxelTextures.normal,
        //     // m_voxelTextures.emissive
        // };
        // RHITextureSubResourceRange ranges[] = {
        //     // m_renderDevice->GetTextureSubResourceRange(m_voxelTextures.staticFlag),
        //     m_renderDevice->GetTextureSubResourceRange(m_voxelTextures.pAlbedo),
        //     // m_renderDevice->GetTextureSubResourceRange(m_voxelTextures.normal),
        //     // m_renderDevice->GetTextureSubResourceRange(m_voxelTextures.emissive)
        // };
        // reset voxel texture
        {
            auto* pPass =
                m_rdg->AddComputePassNode(m_computePasses.pResetVoxelTexture, "reset_voxel_texture");
            // m_rdg->DeclareTextureAccessForPass(pPass, 1, textures, RHITextureUsage::eStorage, ranges,
            //                                    RHIAccessMode::eReadWrite);
            workgroupCount = static_cast<int>(m_voxelTexResolution / 8);
            m_rdg->AddComputePassDispatchNode(pPass, workgroupCount, workgroupCount, workgroupCount);
        }
        // reset compute indirect
        {
            auto* pPass = m_rdg->AddComputePassNode(m_computePasses.pResetComputeIndirect,
                                                   "reset_compute_indirect");
            // m_rdg->DeclareBufferAccessForPass(pPass, m_buffers.pComputeIndirectBuffer,
            //                                   RHIBufferUsage::eStorageBuffer, RHIAccessMode::eReadWrite);
            m_rdg->AddComputePassDispatchNode(pPass, 1, 1, 1);
        }
        // voxelize small triangles
        {
            VoxelizationCompSP* pShaderProgram =
                dynamic_cast<VoxelizationCompSP*>(m_computePasses.pVoxelization->pShaderProgram);
            auto* pPass = m_rdg->AddComputePassNode(m_computePasses.pVoxelization,
                                                   "voxelization_compute_small_triangles");

            // m_rdg->DeclareTextureAccessForPass(pPass, 1, textures, RHITextureUsage::eStorage, ranges,
            //                                    RHIAccessMode::eReadWrite);
            // m_rdg->DeclareBufferAccessForPass(pPass, m_buffers.pComputeIndirectBuffer,
            //                                   RHIBufferUsage::eStorageBuffer, RHIAccessMode::eReadWrite);
            // m_rdg->DeclareBufferAccessForPass(pPass, m_buffers.pLargeTriangleBuffer,
            //                                   RHIBufferUsage::eStorageBuffer, RHIAccessMode::eReadWrite);

            pShaderProgram->pushConstantsData.largeTriangleThreshold = 15;

            const int localSize = 32;
            for (auto* node : m_pScene->GetRenderableNodes())
            {
                const int triangleCount = node->GetComponent<sg::Mesh>()->GetNumIndices() / 3;
                ASSERT(triangleCount == m_pScene->GetNumIndices() / 3);
                workgroupCount = ceil(double(triangleCount) / double(localSize));

                pShaderProgram->pushConstantsData.nodeIndex     = node->GetRenderableIndex();
                pShaderProgram->pushConstantsData.triangleCount = triangleCount;
                m_rdg->AddComputePassSetPushConstants(
                    pPass, &pShaderProgram->pushConstantsData,
                    sizeof(VoxelizationCompSP::PushConstantsData));
                m_rdg->AddComputePassDispatchNode(pPass, workgroupCount, 1, 1);
            }
        }
        // voxelize large triangles
        {
            auto* pPass = m_rdg->AddComputePassNode(m_computePasses.pVoxelizationLargeTriangle,
                                                   "voxelization_compute_large_triangle");
            // m_rdg->DeclareTextureAccessForPass(pPass, 1, textures, RHITextureUsage::eStorage, ranges,
            //                                    RHIAccessMode::eReadWrite);
            // m_rdg->DeclareBufferAccessForPass(pPass, m_buffers.pComputeIndirectBuffer,
            //                                   RHIBufferUsage::eIndirectBuffer, RHIAccessMode::eRead);
            // m_rdg->DeclareBufferAccessForPass(pPass, m_buffers.pLargeTriangleBuffer,
            //                                   RHIBufferUsage::eStorageBuffer, RHIAccessMode::eRead);
            m_rdg->AddComputePassDispatchIndirectNode(pPass, m_buffers.pComputeIndirectBuffer, 0);
        }
        // reset draw indirect
        {
            auto* pPass = m_rdg->AddComputePassNode(m_computePasses.pResetDrawIndirect,
                                                   "reset_draw_indirect_compute");
            // m_rdg->DeclareBufferAccessForPass(pPass, m_buffers.pDrawIndirectBuffer,
            //                                   RHIBufferUsage::eStorageBuffer, RHIAccessMode::eReadWrite);
            m_rdg->AddComputePassDispatchNode(pPass, 1, 1, 1);
        }
        // voxel pre-draw pPass
        {
            auto* pPass =
                m_rdg->AddComputePassNode(m_computePasses.pVoxelPreDraw, "voxel_pre_draw_compute");
            // m_rdg->DeclareTextureAccessForPass(
            //     pPass, m_voxelTextures.pAlbedo, RHITextureUsage::eStorage,
            //     m_renderDevice->GetTextureSubResourceRange(m_voxelTextures.pAlbedo),
            //     RHIAccessMode::eRead);
            // m_rdg->DeclareBufferAccessForPass(pPass, m_buffers.pInstancePositionBuffer,
            //                                   RHIBufferUsage::eStorageBuffer, RHIAccessMode::eReadWrite);
            // m_rdg->DeclareBufferAccessForPass(pPass, m_buffers.pInstanceColorBuffer,
            //                                   RHIBufferUsage::eStorageBuffer, RHIAccessMode::eReadWrite);
            // m_rdg->DeclareBufferAccessForPass(pPass, m_buffers.pDrawIndirectBuffer,
            //                                   RHIBufferUsage::eStorageBuffer, RHIAccessMode::eReadWrite);
            workgroupCount = static_cast<int>(m_voxelTexResolution / 8);
            m_rdg->AddComputePassDispatchNode(pPass, workgroupCount, workgroupCount, workgroupCount);
        }

        m_needVoxelization = false;
        m_rebuildRDG       = true;
    }
    else
    {
        m_rebuildRDG = false;
    }

    // voxel draw pPass
    {
        // std::vector<RHIRenderPassClearValue> clearValues(2);
        // clearValues[0].color   = {0.0f, 0.0f, 0.0f, 0.0f};
        // clearValues[1].depth   = 1.0f;
        // clearValues[1].stencil = 0;
        Rect2<int> area(0, static_cast<int>(m_pViewport->GetWidth()), 0,
                        static_cast<int>(m_pViewport->GetHeight()));
        Rect2<float> viewport(static_cast<float>(m_pViewport->GetWidth()),
                              static_cast<float>(m_pViewport->GetHeight()));
        auto* pPass = m_rdg->AddGraphicsPassNode(m_gfxPasses.pVoxelDraw, "voxel_draw2");
        // m_rdg->DeclareBufferAccessForPass(pPass, m_buffers.pInstancePositionBuffer,
        //                                   RHIBufferUsage::eStorageBuffer, RHIAccessMode::eRead);
        // m_rdg->DeclareBufferAccessForPass(pPass, m_buffers.pInstanceColorBuffer,
        //                                   RHIBufferUsage::eStorageBuffer, RHIAccessMode::eRead);
        // m_rdg->DeclareBufferAccessForPass(pPass, m_buffers.pDrawIndirectBuffer,
        //                                   RHIBufferUsage::eIndirectBuffer, RHIAccessMode::eRead);
        // m_rdg->DeclareTextureAccessForPass(
        //     pPass, m_voxelTextures.pAlbedo, RHITextureUsage::eStorage,
        //     m_renderDevice->GetTextureSubResourceRange(m_voxelTextures.pAlbedo), RHIAccessMode::eRead);
        m_rdg->AddGraphicsPassBindVertexBufferNode(pPass, m_pCube->GetVertexBuffer(), {0});
        m_rdg->AddGraphicsPassBindIndexBufferNode(pPass, m_pCube->GetIndexBuffer(),
                                                  DataFormat::eR32UInt);
        m_rdg->AddGraphicsPassSetViewportNode(pPass, viewport);
        m_rdg->AddGraphicsPassSetScissorNode(pPass, area);
        m_rdg->AddGraphicsPassDrawIndexedIndirectNode(pPass, m_buffers.pDrawIndirectBuffer, 0, 1, 0);
    }
    m_rdg->End();
}

void ComputeVoxelizer::BuildGraphicsPasses()
{
    RHIGfxPipelineStates pso{};
    pso.rasterizationState           = {};
    pso.rasterizationState.cullMode  = RHIPolygonCullMode::eDisabled;
    pso.rasterizationState.frontFace = RHIPolygonFrontFace::eCounterClockWise;

    pso.depthStencilState =
        RHIGfxPipelineDepthStencilState::Create(true, true, RHIDepthCompareOperator::eLess);
    pso.multiSampleState = {};
    pso.colorBlendState.AddAttachment();
    pso.dynamicStates.Enable(RHIDynamicState::eScissor, RHIDynamicState::eViewPort);

    rc::GraphicsPassBuilder builder(m_pRenderDevice);
    m_gfxPasses.pVoxelDraw =
        builder
            .SetShaderProgramName("VoxelDrawSP2")
            // .SetNumSamples(SampleCount::e1)
            .SetPipelineState(pso)
            .AddViewportColorRT(m_pViewport, RHIRenderTargetLoadOp::eLoad)
            .SetViewportDepthStencilRT(m_pViewport, RHIRenderTargetLoadOp::eClear,
                                       RHIRenderTargetStoreOp::eStore)
            .SetFramebufferInfo(m_pViewport)
            .SetTag("VoxelDraw2")
            .Build();
}

void ComputeVoxelizer::BuildComputePasses()
{
    {
        // reset draw indirect
        ComputePassBuilder builder(m_pRenderDevice);
        m_computePasses.pResetDrawIndirect = builder.SetShaderProgramName("ResetDrawIndirectSP")
                                                .SetTag("ResetDrawIndirectComp")
                                                .Build();
    }
    {
        // reset voxel texture
        ComputePassBuilder builder(m_pRenderDevice);
        m_computePasses.pResetVoxelTexture = builder.SetShaderProgramName("ResetVoxelTextureSP")
                                                .SetTag("ResetVoxelTextureComp")
                                                .Build();
    }
    {
        // reset compute indirect
        ComputePassBuilder builder(m_pRenderDevice);
        m_computePasses.pResetComputeIndirect =
            builder.SetShaderProgramName("ResetComputeIndirectSP")
                .SetTag("ResetComputeIndirectComp")
                .Build();
    }
    // voxelization pPass
    {
        ComputePassBuilder builder(m_pRenderDevice);
        m_computePasses.pVoxelization =
            builder.SetShaderProgramName("VoxelizationCompSP").SetTag("VoxelizationComp").Build();
    }
    // voxelization large triangle pPass
    {
        ComputePassBuilder builder(m_pRenderDevice);
        m_computePasses.pVoxelizationLargeTriangle =
            builder.SetShaderProgramName("VoxelizationLargeTriangleCompSP")
                .SetTag("VoxelizationLargeTriangleComp")
                .Build();
    }
    // voxel pre-draw pPass, calculate position and color
    {
        ComputePassBuilder builder(m_pRenderDevice);
        m_computePasses.pVoxelPreDraw =
            builder.SetShaderProgramName("VoxelPreDrawSP").SetTag("VoxelPreDrawComp").Build();
    }
}


void ComputeVoxelizer::UpdatePassResources()
{
    // reset draw indirect
    {
        HeapVector<RHIShaderResourceBinding> set0bindings;
        ADD_SHADER_BINDING_SINGLE(set0bindings, 0, RHIShaderResourceType::eStorageBuffer,
                                  m_buffers.pDrawIndirectBuffer);
        ComputePassResourceUpdater updater(m_pRenderDevice, m_computePasses.pResetDrawIndirect);
        updater.SetShaderResourceBinding(0, std::move(set0bindings)).Update();
    }
    // reset voxel texture
    {
        HeapVector<RHIShaderResourceBinding> set0bindings;
        ADD_SHADER_BINDING_SINGLE(set0bindings, 0, RHIShaderResourceType::eImage,
                                  m_voxelTextures.pAlbedo);
        ComputePassResourceUpdater updater(m_pRenderDevice, m_computePasses.pResetVoxelTexture);
        updater.SetShaderResourceBinding(0, std::move(set0bindings)).Update();
    }
    // reset compute indirect
    {
        HeapVector<RHIShaderResourceBinding> set0bindings;
        ADD_SHADER_BINDING_SINGLE(set0bindings, 0, RHIShaderResourceType::eStorageBuffer,
                                  m_buffers.pComputeIndirectBuffer);
        ComputePassResourceUpdater updater(m_pRenderDevice, m_computePasses.pResetComputeIndirect);
        updater.SetShaderResourceBinding(0, std::move(set0bindings)).Update();
    }
    // voxelization pPass - small triangles
    {
        HeapVector<RHIShaderResourceBinding> set0bindings;
        HeapVector<RHIShaderResourceBinding> set1bindings;
        HeapVector<RHIShaderResourceBinding> set2bindings;
        HeapVector<RHIShaderResourceBinding> set3bindings;
        HeapVector<RHIShaderResourceBinding> set4bindings;
        HeapVector<RHIShaderResourceBinding> set5bindings;
        // set-0 bindings
        ADD_SHADER_BINDING_SINGLE(set0bindings, 0, RHIShaderResourceType::eImage,
                                  m_voxelTextures.pAlbedo);
        // set-1 bindings
        ADD_SHADER_BINDING_SINGLE(
            set1bindings, 0, RHIShaderResourceType::eUniformBuffer,
            m_computePasses.pVoxelization->pShaderProgram->GetUniformBufferHandle("uSceneInfo"));
        // set-2 bindings
        ADD_SHADER_BINDING_SINGLE(set2bindings, 0, RHIShaderResourceType::eStorageBuffer,
                                  m_pScene->GetVertexBuffer());
        ADD_SHADER_BINDING_SINGLE(set2bindings, 1, RHIShaderResourceType::eStorageBuffer,
                                  m_pScene->GetIndexBuffer());
        ADD_SHADER_BINDING_SINGLE(set2bindings, 2, RHIShaderResourceType::eStorageBuffer,
                                  m_pScene->GetNodesDataSSBO());
        // set-3 bindings: texture array
        ADD_SHADER_BINDING_TEXTURE_ARRAY(set3bindings, 0,
                                         RHIShaderResourceType::eSamplerWithTexture, m_pColorSampler,
                                         m_pScene->GetSceneTextures())
        // set-4 bindings
        ADD_SHADER_BINDING_SINGLE(set4bindings, 0, RHIShaderResourceType::eStorageBuffer,
                                  m_buffers.pComputeIndirectBuffer);
        ADD_SHADER_BINDING_SINGLE(set4bindings, 1, RHIShaderResourceType::eStorageBuffer,
                                  m_buffers.pLargeTriangleBuffer);

        // set-5 bindings
        ADD_SHADER_BINDING_SINGLE(set5bindings, 0, RHIShaderResourceType::eStorageBuffer,
                                  m_pScene->GetTriangleMapBuffer());

        // small triangle
        ComputePassResourceUpdater updater(m_pRenderDevice, m_computePasses.pVoxelization);
        updater.SetShaderResourceBinding(0, std::move(set0bindings))
            .SetShaderResourceBinding(1, std::move(set1bindings))
            .SetShaderResourceBinding(2, std::move(set2bindings))
            .SetShaderResourceBinding(3, std::move(set3bindings))
            .SetShaderResourceBinding(4, std::move(set4bindings))
            .SetShaderResourceBinding(5, std::move(set5bindings))
            .Update();
    }
    // voxelization pPass - large triangles
    {
        HeapVector<RHIShaderResourceBinding> set0bindings;
        HeapVector<RHIShaderResourceBinding> set1bindings;
        HeapVector<RHIShaderResourceBinding> set2bindings;
        HeapVector<RHIShaderResourceBinding> set3bindings;
        HeapVector<RHIShaderResourceBinding> set4bindings;
        HeapVector<RHIShaderResourceBinding> set5bindings;
        // set-0 bindings
        ADD_SHADER_BINDING_SINGLE(set0bindings, 0, RHIShaderResourceType::eImage,
                                  m_voxelTextures.pAlbedo);
        // set-1 bindings
        ADD_SHADER_BINDING_SINGLE(
            set1bindings, 0, RHIShaderResourceType::eUniformBuffer,
            m_computePasses.pVoxelization->pShaderProgram->GetUniformBufferHandle("uSceneInfo"));
        // set-2 bindings
        ADD_SHADER_BINDING_SINGLE(set2bindings, 0, RHIShaderResourceType::eStorageBuffer,
                                  m_pScene->GetVertexBuffer());
        ADD_SHADER_BINDING_SINGLE(set2bindings, 1, RHIShaderResourceType::eStorageBuffer,
                                  m_pScene->GetIndexBuffer());
        ADD_SHADER_BINDING_SINGLE(set2bindings, 2, RHIShaderResourceType::eStorageBuffer,
                                  m_pScene->GetNodesDataSSBO());
        // set-3 bindings: texture array
        ADD_SHADER_BINDING_TEXTURE_ARRAY(set3bindings, 0,
                                         RHIShaderResourceType::eSamplerWithTexture, m_pColorSampler,
                                         m_pScene->GetSceneTextures())
        // set-4 bindings
        ADD_SHADER_BINDING_SINGLE(set4bindings, 0, RHIShaderResourceType::eStorageBuffer,
                                  m_buffers.pComputeIndirectBuffer);
        ADD_SHADER_BINDING_SINGLE(set4bindings, 1, RHIShaderResourceType::eStorageBuffer,
                                  m_buffers.pLargeTriangleBuffer);

        // set-5 bindings
        ADD_SHADER_BINDING_SINGLE(set5bindings, 0, RHIShaderResourceType::eStorageBuffer,
                                  m_pScene->GetTriangleMapBuffer());

        ComputePassResourceUpdater updater2(m_pRenderDevice,
                                            m_computePasses.pVoxelizationLargeTriangle);
        updater2.SetShaderResourceBinding(0, std::move(set0bindings))
            .SetShaderResourceBinding(1, std::move(set1bindings))
            .SetShaderResourceBinding(2, std::move(set2bindings))
            .SetShaderResourceBinding(3, std::move(set3bindings))
            .SetShaderResourceBinding(4, std::move(set4bindings))
            .SetShaderResourceBinding(5, std::move(set5bindings))
            .Update();
    }
    // voxel pre-draw pPass
    {
        HeapVector<RHIShaderResourceBinding> set0bindings;
        HeapVector<RHIShaderResourceBinding> set1bindings;
        HeapVector<RHIShaderResourceBinding> set2bindings;
        HeapVector<RHIShaderResourceBinding> set3bindings;
        HeapVector<RHIShaderResourceBinding> set4bindings;

        // set-0 bindings
        ADD_SHADER_BINDING_SINGLE(set0bindings, 0, RHIShaderResourceType::eImage,
                                  m_voxelTextures.pAlbedo);
        // set-1 bindings
        ADD_SHADER_BINDING_SINGLE(set1bindings, 0, RHIShaderResourceType::eStorageBuffer,
                                  m_buffers.pInstancePositionBuffer);
        // set-2 bindings
        ADD_SHADER_BINDING_SINGLE(set2bindings, 0, RHIShaderResourceType::eStorageBuffer,
                                  m_buffers.pInstanceColorBuffer);
        // set-3 bindings
        ADD_SHADER_BINDING_SINGLE(set3bindings, 0, RHIShaderResourceType::eStorageBuffer,
                                  m_buffers.pDrawIndirectBuffer);
        // set-4 bindings
        ADD_SHADER_BINDING_SINGLE(
            set4bindings, 0, RHIShaderResourceType::eUniformBuffer,
            m_computePasses.pVoxelPreDraw->pShaderProgram->GetUniformBufferHandle("uSceneInfo"));


        ComputePassResourceUpdater updater(m_pRenderDevice, m_computePasses.pVoxelPreDraw);
        updater.SetShaderResourceBinding(0, std::move(set0bindings))
            .SetShaderResourceBinding(1, std::move(set1bindings))
            .SetShaderResourceBinding(2, std::move(set2bindings))
            .SetShaderResourceBinding(3, std::move(set3bindings))
            .SetShaderResourceBinding(4, std::move(set4bindings))
            .Update();
    }
    // voxel draw pPass
    {
        VoxelDrawSP2* pShaderProgram =
            dynamic_cast<VoxelDrawSP2*>(m_gfxPasses.pVoxelDraw->pShaderProgram);

        HeapVector<RHIShaderResourceBinding> set0bindings;
        HeapVector<RHIShaderResourceBinding> set1bindings;
        HeapVector<RHIShaderResourceBinding> set2bindings;
        // set-0 bindings
        ADD_SHADER_BINDING_SINGLE(set0bindings, 0, RHIShaderResourceType::eUniformBuffer,
                                  pShaderProgram->GetUniformBufferHandle("uTransformData"));
        // set-1 bindings
        ADD_SHADER_BINDING_SINGLE(set1bindings, 0, RHIShaderResourceType::eStorageBuffer,
                                  m_buffers.pInstancePositionBuffer);
        // set-2 bindings
        ADD_SHADER_BINDING_SINGLE(set2bindings, 0, RHIShaderResourceType::eStorageBuffer,
                                  m_buffers.pInstanceColorBuffer);
        rc::GraphicsPassResourceUpdater updater(m_pRenderDevice, m_gfxPasses.pVoxelDraw);
        updater.SetShaderResourceBinding(0, std::move(set0bindings))
            .SetShaderResourceBinding(1, std::move(set1bindings))
            .SetShaderResourceBinding(2, std::move(set2bindings))
            .Update();
    }
}

void ComputeVoxelizer::UpdateUniformData()
{
    {
        VoxelizationCompSP* pShaderProgram =
            dynamic_cast<VoxelizationCompSP*>(m_computePasses.pVoxelization->pShaderProgram);
        pShaderProgram->sceneInfo.aabbMax = Vec4(m_voxelAABB.GetMax(), 1.0f);
        pShaderProgram->sceneInfo.aabbMin = Vec4(m_voxelAABB.GetMin(), 1.0f);
        pShaderProgram->UpdateUniformBuffer("uSceneInfo", pShaderProgram->GetSceneInfoData(), 0);
    }
    {
        VoxelizationLargeTriangleCompSP* pShaderProgram =
            dynamic_cast<VoxelizationLargeTriangleCompSP*>(
                m_computePasses.pVoxelizationLargeTriangle->pShaderProgram);
        pShaderProgram->sceneInfo.aabbMax = Vec4(m_voxelAABB.GetMax(), 1.0f);
        pShaderProgram->sceneInfo.aabbMin = Vec4(m_voxelAABB.GetMin(), 1.0f);
        pShaderProgram->UpdateUniformBuffer("uSceneInfo", pShaderProgram->GetSceneInfoData(), 0);
    }
    {
        VoxelPreDrawSP* pShaderProgram =
            dynamic_cast<VoxelPreDrawSP*>(m_computePasses.pVoxelPreDraw->pShaderProgram);
        pShaderProgram->sceneInfo.aabbMax = Vec4(m_voxelAABB.GetMax(), 1.0f);
        pShaderProgram->sceneInfo.aabbMin = Vec4(m_voxelAABB.GetMin(), 1.0f);
        pShaderProgram->UpdateUniformBuffer("uSceneInfo", pShaderProgram->GetSceneInfoData(), 0);
    }
    {
        VoxelDrawSP2* pShaderProgram =
            dynamic_cast<VoxelDrawSP2*>(m_gfxPasses.pVoxelDraw->pShaderProgram);
        pShaderProgram->transformData.modelMatrix = m_voxelTransform;
        pShaderProgram->transformData.projMatrix  = m_pScene->GetCamera()->GetProjectionMatrix();
        pShaderProgram->transformData.viewMatrix  = m_pScene->GetCamera()->GetViewMatrix();

        pShaderProgram->UpdateUniformBuffer("uTransformData", pShaderProgram->GetTransformData(), 0);
    }
}


void ComputeVoxelizer::PrepareRenderWorkload()
{
    if (m_rebuildRDG)
    {
        BuildRenderGraph();
        //        m_rebuildRDG = false;
    }
    UpdateUniformData();
}


void ComputeVoxelizer::OnResize()
{
    m_rebuildRDG = true;
    m_pRenderDevice->UpdateGraphicsPassOnResize(m_gfxPasses.pVoxelDraw, m_pViewport);
}

void ComputeVoxelizer::SetRenderScene(RenderScene* pScene)
{
    VoxelizerBase::SetRenderScene(pScene);

    const auto center = m_pScene->GetAABB().GetCenter();

    glm::vec3 min = center - glm::vec3(m_sceneExtent / 2, m_sceneExtent / 2, m_sceneExtent / 2);
    glm::vec3 max = center + glm::vec3(m_sceneExtent / 2, m_sceneExtent / 2, m_sceneExtent / 2);
    m_voxelAABB   = {min, max};

    auto cubeExtent = m_pCube->GetAABB().GetExtent3D();
    // make sure cube vertex is (-1, -1, 1) (1, 1, 1) todo: find other solution
    float scaleFactor = 2.0f / (cubeExtent.x);

    m_voxelTransform = glm::scale(Mat4(1.0f), glm::vec3(m_voxelSize) * scaleFactor);
}

#ifdef ZEN_MACOS
void ComputeVoxelizer::WarmupTextureAllocation()
{
    const auto halfDim = m_voxelTexResolution / 2;
    for (uint32_t i = 0; i < NUM_DUMMY_TEXTURES; i++)
    {
        const auto texName = "dummy_texture_" + std::to_string(i);
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
// void ComputeVoxelizer::VoxelizeStaticScene() {}
//
// void ComputeVoxelizer::VoxelizeDynamicScene() {}
} // namespace zen::rc