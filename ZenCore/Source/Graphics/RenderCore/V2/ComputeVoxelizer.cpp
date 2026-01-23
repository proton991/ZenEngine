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
    ZEN_DELETE(m_cube);
}

void ComputeVoxelizer::LoadCubeModel()
{
    m_cube = ZEN_NEW()
        RenderObject(m_renderDevice, platform::ConfigLoader::GetInstance().GetGLTFModelPath("Box"));
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
    m_buffers.computeIndirectBuffer = m_renderDevice->CreateIndirectBuffer(
        sizeof(ComputeIndirectCommand), reinterpret_cast<const uint8_t*>(&indirectCommand),
        "voxel_comp_indirect_buffer");

    m_buffers.largeTriangleBuffer = m_renderDevice->CreateStorageBuffer(
        sizeof(LargeTriangle) * 200000, nullptr, "large_triangle_buffer");

    m_buffers.instancePositionBuffer = m_renderDevice->CreateStorageBuffer(
        sizeof(Vec4) * 3000000, nullptr, "instance_position_buffer");

    m_buffers.instanceColorBuffer = m_renderDevice->CreateStorageBuffer(
        sizeof(Vec4) * 3000000, nullptr, "instance_color_buffer");

    DrawIndexedIndirectCommand drawIndirectCmd{};
    drawIndirectCmd.instanceCount = 1;
    drawIndirectCmd.firstInstance = 0;
    drawIndirectCmd.indexCount    = 36;
    drawIndirectCmd.firstIndex    = 0;
    drawIndirectCmd.vertexOffset  = 0;

    m_buffers.drawIndirectBuffer = m_renderDevice->CreateIndirectBuffer(
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
    VERIFY_EXPR(m_scene != nullptr);
    m_rdg = MakeUnique<RenderGraph>("comp_voxelize_rdg");
    m_rdg->Begin();
    int workgroupCount;
    // voxelization pass
    if (m_needVoxelization)
    {
        // TextureHandle textures[] = {
        //     // m_voxelTextures.staticFlag,
        //     m_voxelTextures.albedo,
        //     // m_voxelTextures.normal,
        //     // m_voxelTextures.emissive
        // };
        // RHITextureSubResourceRange ranges[] = {
        //     // m_renderDevice->GetTextureSubResourceRange(m_voxelTextures.staticFlag),
        //     m_renderDevice->GetTextureSubResourceRange(m_voxelTextures.albedo),
        //     // m_renderDevice->GetTextureSubResourceRange(m_voxelTextures.normal),
        //     // m_renderDevice->GetTextureSubResourceRange(m_voxelTextures.emissive)
        // };
        // reset voxel texture
        {
            auto* pass =
                m_rdg->AddComputePassNode(m_computePasses.resetVoxelTexture, "reset_voxel_texture");
            // m_rdg->DeclareTextureAccessForPass(pass, 1, textures, RHITextureUsage::eStorage, ranges,
            //                                    RHIAccessMode::eReadWrite);
            workgroupCount = static_cast<int>(m_voxelTexResolution / 8);
            m_rdg->AddComputePassDispatchNode(pass, workgroupCount, workgroupCount, workgroupCount);
        }
        // reset compute indirect
        {
            auto* pass = m_rdg->AddComputePassNode(m_computePasses.resetComputeIndirect,
                                                   "reset_compute_indirect");
            // m_rdg->DeclareBufferAccessForPass(pass, m_buffers.computeIndirectBuffer,
            //                                   RHIBufferUsage::eStorageBuffer, RHIAccessMode::eReadWrite);
            m_rdg->AddComputePassDispatchNode(pass, 1, 1, 1);
        }
        // voxelize small triangles
        {
            VoxelizationCompSP* shaderProgram =
                dynamic_cast<VoxelizationCompSP*>(m_computePasses.voxelization->shaderProgram);
            auto* pass = m_rdg->AddComputePassNode(m_computePasses.voxelization,
                                                   "voxelization_compute_small_triangles");

            // m_rdg->DeclareTextureAccessForPass(pass, 1, textures, RHITextureUsage::eStorage, ranges,
            //                                    RHIAccessMode::eReadWrite);
            // m_rdg->DeclareBufferAccessForPass(pass, m_buffers.computeIndirectBuffer,
            //                                   RHIBufferUsage::eStorageBuffer, RHIAccessMode::eReadWrite);
            // m_rdg->DeclareBufferAccessForPass(pass, m_buffers.largeTriangleBuffer,
            //                                   RHIBufferUsage::eStorageBuffer, RHIAccessMode::eReadWrite);

            shaderProgram->pushConstantsData.largeTriangleThreshold = 15;

            const int localSize = 32;
            for (auto* node : m_scene->GetRenderableNodes())
            {
                const int triangleCount = node->GetComponent<sg::Mesh>()->GetNumIndices() / 3;
                ASSERT(triangleCount == m_scene->GetNumIndices() / 3);
                workgroupCount = ceil(double(triangleCount) / double(localSize));

                shaderProgram->pushConstantsData.nodeIndex     = node->GetRenderableIndex();
                shaderProgram->pushConstantsData.triangleCount = triangleCount;
                m_rdg->AddComputePassSetPushConstants(
                    pass, &shaderProgram->pushConstantsData,
                    sizeof(VoxelizationCompSP::PushConstantsData));
                m_rdg->AddComputePassDispatchNode(pass, workgroupCount, 1, 1);
            }
        }
        // voxelize large triangles
        {
            auto* pass = m_rdg->AddComputePassNode(m_computePasses.voxelizationLargeTriangle,
                                                   "voxelization_compute_large_triangle");
            // m_rdg->DeclareTextureAccessForPass(pass, 1, textures, RHITextureUsage::eStorage, ranges,
            //                                    RHIAccessMode::eReadWrite);
            // m_rdg->DeclareBufferAccessForPass(pass, m_buffers.computeIndirectBuffer,
            //                                   RHIBufferUsage::eIndirectBuffer, RHIAccessMode::eRead);
            // m_rdg->DeclareBufferAccessForPass(pass, m_buffers.largeTriangleBuffer,
            //                                   RHIBufferUsage::eStorageBuffer, RHIAccessMode::eRead);
            m_rdg->AddComputePassDispatchIndirectNode(pass, m_buffers.computeIndirectBuffer, 0);
        }
        // reset draw indirect
        {
            auto* pass = m_rdg->AddComputePassNode(m_computePasses.resetDrawIndirect,
                                                   "reset_draw_indirect_compute");
            // m_rdg->DeclareBufferAccessForPass(pass, m_buffers.drawIndirectBuffer,
            //                                   RHIBufferUsage::eStorageBuffer, RHIAccessMode::eReadWrite);
            m_rdg->AddComputePassDispatchNode(pass, 1, 1, 1);
        }
        // voxel pre-draw pass
        {
            auto* pass =
                m_rdg->AddComputePassNode(m_computePasses.voxelPreDraw, "voxel_pre_draw_compute");
            // m_rdg->DeclareTextureAccessForPass(
            //     pass, m_voxelTextures.albedo, RHITextureUsage::eStorage,
            //     m_renderDevice->GetTextureSubResourceRange(m_voxelTextures.albedo),
            //     RHIAccessMode::eRead);
            // m_rdg->DeclareBufferAccessForPass(pass, m_buffers.instancePositionBuffer,
            //                                   RHIBufferUsage::eStorageBuffer, RHIAccessMode::eReadWrite);
            // m_rdg->DeclareBufferAccessForPass(pass, m_buffers.instanceColorBuffer,
            //                                   RHIBufferUsage::eStorageBuffer, RHIAccessMode::eReadWrite);
            // m_rdg->DeclareBufferAccessForPass(pass, m_buffers.drawIndirectBuffer,
            //                                   RHIBufferUsage::eStorageBuffer, RHIAccessMode::eReadWrite);
            workgroupCount = static_cast<int>(m_voxelTexResolution / 8);
            m_rdg->AddComputePassDispatchNode(pass, workgroupCount, workgroupCount, workgroupCount);
        }

        m_needVoxelization = false;
        m_rebuildRDG       = true;
    }
    else
    {
        m_rebuildRDG = false;
    }

    // voxel draw pass
    {
        // std::vector<RHIRenderPassClearValue> clearValues(2);
        // clearValues[0].color   = {0.0f, 0.0f, 0.0f, 0.0f};
        // clearValues[1].depth   = 1.0f;
        // clearValues[1].stencil = 0;
        Rect2<int> area(0, static_cast<int>(m_viewport->GetWidth()), 0,
                        static_cast<int>(m_viewport->GetHeight()));
        Rect2<float> viewport(static_cast<float>(m_viewport->GetWidth()),
                              static_cast<float>(m_viewport->GetHeight()));
        auto* pass = m_rdg->AddGraphicsPassNode(m_gfxPasses.voxelDraw, "voxel_draw2");
        // m_rdg->DeclareBufferAccessForPass(pass, m_buffers.instancePositionBuffer,
        //                                   RHIBufferUsage::eStorageBuffer, RHIAccessMode::eRead);
        // m_rdg->DeclareBufferAccessForPass(pass, m_buffers.instanceColorBuffer,
        //                                   RHIBufferUsage::eStorageBuffer, RHIAccessMode::eRead);
        // m_rdg->DeclareBufferAccessForPass(pass, m_buffers.drawIndirectBuffer,
        //                                   RHIBufferUsage::eIndirectBuffer, RHIAccessMode::eRead);
        // m_rdg->DeclareTextureAccessForPass(
        //     pass, m_voxelTextures.albedo, RHITextureUsage::eStorage,
        //     m_renderDevice->GetTextureSubResourceRange(m_voxelTextures.albedo), RHIAccessMode::eRead);
        m_rdg->AddGraphicsPassBindVertexBufferNode(pass, m_cube->GetVertexBuffer(), {0});
        m_rdg->AddGraphicsPassBindIndexBufferNode(pass, m_cube->GetIndexBuffer(),
                                                  DataFormat::eR32UInt);
        m_rdg->AddGraphicsPassSetViewportNode(pass, viewport);
        m_rdg->AddGraphicsPassSetScissorNode(pass, area);
        m_rdg->AddGraphicsPassDrawIndexedIndirectNode(pass, m_buffers.drawIndirectBuffer, 0, 1, 0);
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

    rc::GraphicsPassBuilder builder(m_renderDevice);
    m_gfxPasses.voxelDraw =
        builder
            .SetShaderProgramName("VoxelDrawSP2")
            // .SetNumSamples(SampleCount::e1)
            .SetPipelineState(pso)
            .AddViewportColorRT(m_viewport, RHIRenderTargetLoadOp::eLoad)
            .SetViewportDepthStencilRT(m_viewport, RHIRenderTargetLoadOp::eClear,
                                       RHIRenderTargetStoreOp::eStore)
            .SetFramebufferInfo(m_viewport)
            .SetTag("VoxelDraw2")
            .Build();
}

void ComputeVoxelizer::BuildComputePasses()
{
    {
        // reset draw indirect
        ComputePassBuilder builder(m_renderDevice);
        m_computePasses.resetDrawIndirect = builder.SetShaderProgramName("ResetDrawIndirectSP")
                                                .SetTag("ResetDrawIndirectComp")
                                                .Build();
    }
    {
        // reset voxel texture
        ComputePassBuilder builder(m_renderDevice);
        m_computePasses.resetVoxelTexture = builder.SetShaderProgramName("ResetVoxelTextureSP")
                                                .SetTag("ResetVoxelTextureComp")
                                                .Build();
    }
    {
        // reset compute indirect
        ComputePassBuilder builder(m_renderDevice);
        m_computePasses.resetComputeIndirect =
            builder.SetShaderProgramName("ResetComputeIndirectSP")
                .SetTag("ResetComputeIndirectComp")
                .Build();
    }
    // voxelization pass
    {
        ComputePassBuilder builder(m_renderDevice);
        m_computePasses.voxelization =
            builder.SetShaderProgramName("VoxelizationCompSP").SetTag("VoxelizationComp").Build();
    }
    // voxelization large triangle pass
    {
        ComputePassBuilder builder(m_renderDevice);
        m_computePasses.voxelizationLargeTriangle =
            builder.SetShaderProgramName("VoxelizationLargeTriangleCompSP")
                .SetTag("VoxelizationLargeTriangleComp")
                .Build();
    }
    // voxel pre-draw pass, calculate position and color
    {
        ComputePassBuilder builder(m_renderDevice);
        m_computePasses.voxelPreDraw =
            builder.SetShaderProgramName("VoxelPreDrawSP").SetTag("VoxelPreDrawComp").Build();
    }
}


void ComputeVoxelizer::UpdatePassResources()
{
    // reset draw indirect
    {
        HeapVector<RHIShaderResourceBinding> set0bindings;
        ADD_SHADER_BINDING_SINGLE(set0bindings, 0, RHIShaderResourceType::eStorageBuffer,
                                  m_buffers.drawIndirectBuffer);
        ComputePassResourceUpdater updater(m_renderDevice, m_computePasses.resetDrawIndirect);
        updater.SetShaderResourceBinding(0, std::move(set0bindings)).Update();
    }
    // reset voxel texture
    {
        HeapVector<RHIShaderResourceBinding> set0bindings;
        ADD_SHADER_BINDING_SINGLE(set0bindings, 0, RHIShaderResourceType::eImage,
                                  m_voxelTextures.albedo);
        ComputePassResourceUpdater updater(m_renderDevice, m_computePasses.resetVoxelTexture);
        updater.SetShaderResourceBinding(0, std::move(set0bindings)).Update();
    }
    // reset compute indirect
    {
        HeapVector<RHIShaderResourceBinding> set0bindings;
        ADD_SHADER_BINDING_SINGLE(set0bindings, 0, RHIShaderResourceType::eStorageBuffer,
                                  m_buffers.computeIndirectBuffer);
        ComputePassResourceUpdater updater(m_renderDevice, m_computePasses.resetComputeIndirect);
        updater.SetShaderResourceBinding(0, std::move(set0bindings)).Update();
    }
    // voxelization pass - small triangles
    {
        HeapVector<RHIShaderResourceBinding> set0bindings;
        HeapVector<RHIShaderResourceBinding> set1bindings;
        HeapVector<RHIShaderResourceBinding> set2bindings;
        HeapVector<RHIShaderResourceBinding> set3bindings;
        HeapVector<RHIShaderResourceBinding> set4bindings;
        HeapVector<RHIShaderResourceBinding> set5bindings;
        // set-0 bindings
        ADD_SHADER_BINDING_SINGLE(set0bindings, 0, RHIShaderResourceType::eImage,
                                  m_voxelTextures.albedo);
        // set-1 bindings
        ADD_SHADER_BINDING_SINGLE(
            set1bindings, 0, RHIShaderResourceType::eUniformBuffer,
            m_computePasses.voxelization->shaderProgram->GetUniformBufferHandle("uSceneInfo"));
        // set-2 bindings
        ADD_SHADER_BINDING_SINGLE(set2bindings, 0, RHIShaderResourceType::eStorageBuffer,
                                  m_scene->GetVertexBuffer());
        ADD_SHADER_BINDING_SINGLE(set2bindings, 1, RHIShaderResourceType::eStorageBuffer,
                                  m_scene->GetIndexBuffer());
        ADD_SHADER_BINDING_SINGLE(set2bindings, 2, RHIShaderResourceType::eStorageBuffer,
                                  m_scene->GetNodesDataSSBO());
        // set-3 bindings: texture array
        ADD_SHADER_BINDING_TEXTURE_ARRAY(set3bindings, 0,
                                         RHIShaderResourceType::eSamplerWithTexture, m_colorSampler,
                                         m_scene->GetSceneTextures())
        // set-4 bindings
        ADD_SHADER_BINDING_SINGLE(set4bindings, 0, RHIShaderResourceType::eStorageBuffer,
                                  m_buffers.computeIndirectBuffer);
        ADD_SHADER_BINDING_SINGLE(set4bindings, 1, RHIShaderResourceType::eStorageBuffer,
                                  m_buffers.largeTriangleBuffer);

        // set-5 bindings
        ADD_SHADER_BINDING_SINGLE(set5bindings, 0, RHIShaderResourceType::eStorageBuffer,
                                  m_scene->GetTriangleMapBuffer());

        // small triangle
        ComputePassResourceUpdater updater(m_renderDevice, m_computePasses.voxelization);
        updater.SetShaderResourceBinding(0, std::move(set0bindings))
            .SetShaderResourceBinding(1, std::move(set1bindings))
            .SetShaderResourceBinding(2, std::move(set2bindings))
            .SetShaderResourceBinding(3, std::move(set3bindings))
            .SetShaderResourceBinding(4, std::move(set4bindings))
            .SetShaderResourceBinding(5, std::move(set5bindings))
            .Update();
    }
    // voxelization pass - large triangles
    {
        HeapVector<RHIShaderResourceBinding> set0bindings;
        HeapVector<RHIShaderResourceBinding> set1bindings;
        HeapVector<RHIShaderResourceBinding> set2bindings;
        HeapVector<RHIShaderResourceBinding> set3bindings;
        HeapVector<RHIShaderResourceBinding> set4bindings;
        HeapVector<RHIShaderResourceBinding> set5bindings;
        // set-0 bindings
        ADD_SHADER_BINDING_SINGLE(set0bindings, 0, RHIShaderResourceType::eImage,
                                  m_voxelTextures.albedo);
        // set-1 bindings
        ADD_SHADER_BINDING_SINGLE(
            set1bindings, 0, RHIShaderResourceType::eUniformBuffer,
            m_computePasses.voxelization->shaderProgram->GetUniformBufferHandle("uSceneInfo"));
        // set-2 bindings
        ADD_SHADER_BINDING_SINGLE(set2bindings, 0, RHIShaderResourceType::eStorageBuffer,
                                  m_scene->GetVertexBuffer());
        ADD_SHADER_BINDING_SINGLE(set2bindings, 1, RHIShaderResourceType::eStorageBuffer,
                                  m_scene->GetIndexBuffer());
        ADD_SHADER_BINDING_SINGLE(set2bindings, 2, RHIShaderResourceType::eStorageBuffer,
                                  m_scene->GetNodesDataSSBO());
        // set-3 bindings: texture array
        ADD_SHADER_BINDING_TEXTURE_ARRAY(set3bindings, 0,
                                         RHIShaderResourceType::eSamplerWithTexture, m_colorSampler,
                                         m_scene->GetSceneTextures())
        // set-4 bindings
        ADD_SHADER_BINDING_SINGLE(set4bindings, 0, RHIShaderResourceType::eStorageBuffer,
                                  m_buffers.computeIndirectBuffer);
        ADD_SHADER_BINDING_SINGLE(set4bindings, 1, RHIShaderResourceType::eStorageBuffer,
                                  m_buffers.largeTriangleBuffer);

        // set-5 bindings
        ADD_SHADER_BINDING_SINGLE(set5bindings, 0, RHIShaderResourceType::eStorageBuffer,
                                  m_scene->GetTriangleMapBuffer());

        ComputePassResourceUpdater updater2(m_renderDevice,
                                            m_computePasses.voxelizationLargeTriangle);
        updater2.SetShaderResourceBinding(0, std::move(set0bindings))
            .SetShaderResourceBinding(1, std::move(set1bindings))
            .SetShaderResourceBinding(2, std::move(set2bindings))
            .SetShaderResourceBinding(3, std::move(set3bindings))
            .SetShaderResourceBinding(4, std::move(set4bindings))
            .SetShaderResourceBinding(5, std::move(set5bindings))
            .Update();
    }
    // voxel pre-draw pass
    {
        HeapVector<RHIShaderResourceBinding> set0bindings;
        HeapVector<RHIShaderResourceBinding> set1bindings;
        HeapVector<RHIShaderResourceBinding> set2bindings;
        HeapVector<RHIShaderResourceBinding> set3bindings;
        HeapVector<RHIShaderResourceBinding> set4bindings;

        // set-0 bindings
        ADD_SHADER_BINDING_SINGLE(set0bindings, 0, RHIShaderResourceType::eImage,
                                  m_voxelTextures.albedo);
        // set-1 bindings
        ADD_SHADER_BINDING_SINGLE(set1bindings, 0, RHIShaderResourceType::eStorageBuffer,
                                  m_buffers.instancePositionBuffer);
        // set-2 bindings
        ADD_SHADER_BINDING_SINGLE(set2bindings, 0, RHIShaderResourceType::eStorageBuffer,
                                  m_buffers.instanceColorBuffer);
        // set-3 bindings
        ADD_SHADER_BINDING_SINGLE(set3bindings, 0, RHIShaderResourceType::eStorageBuffer,
                                  m_buffers.drawIndirectBuffer);
        // set-4 bindings
        ADD_SHADER_BINDING_SINGLE(
            set4bindings, 0, RHIShaderResourceType::eUniformBuffer,
            m_computePasses.voxelPreDraw->shaderProgram->GetUniformBufferHandle("uSceneInfo"));


        ComputePassResourceUpdater updater(m_renderDevice, m_computePasses.voxelPreDraw);
        updater.SetShaderResourceBinding(0, std::move(set0bindings))
            .SetShaderResourceBinding(1, std::move(set1bindings))
            .SetShaderResourceBinding(2, std::move(set2bindings))
            .SetShaderResourceBinding(3, std::move(set3bindings))
            .SetShaderResourceBinding(4, std::move(set4bindings))
            .Update();
    }
    // voxel draw pass
    {
        VoxelDrawSP2* shaderProgram =
            dynamic_cast<VoxelDrawSP2*>(m_gfxPasses.voxelDraw->shaderProgram);

        HeapVector<RHIShaderResourceBinding> set0bindings;
        HeapVector<RHIShaderResourceBinding> set1bindings;
        HeapVector<RHIShaderResourceBinding> set2bindings;
        // set-0 bindings
        ADD_SHADER_BINDING_SINGLE(set0bindings, 0, RHIShaderResourceType::eUniformBuffer,
                                  shaderProgram->GetUniformBufferHandle("uTransformData"));
        // set-1 bindings
        ADD_SHADER_BINDING_SINGLE(set1bindings, 0, RHIShaderResourceType::eStorageBuffer,
                                  m_buffers.instancePositionBuffer);
        // set-2 bindings
        ADD_SHADER_BINDING_SINGLE(set2bindings, 0, RHIShaderResourceType::eStorageBuffer,
                                  m_buffers.instanceColorBuffer);
        rc::GraphicsPassResourceUpdater updater(m_renderDevice, m_gfxPasses.voxelDraw);
        updater.SetShaderResourceBinding(0, std::move(set0bindings))
            .SetShaderResourceBinding(1, std::move(set1bindings))
            .SetShaderResourceBinding(2, std::move(set2bindings))
            .Update();
    }
}

void ComputeVoxelizer::UpdateUniformData()
{
    {
        VoxelizationCompSP* shaderProgram =
            dynamic_cast<VoxelizationCompSP*>(m_computePasses.voxelization->shaderProgram);
        shaderProgram->sceneInfo.aabbMax = Vec4(m_voxelAABB.GetMax(), 1.0f);
        shaderProgram->sceneInfo.aabbMin = Vec4(m_voxelAABB.GetMin(), 1.0f);
        shaderProgram->UpdateUniformBuffer("uSceneInfo", shaderProgram->GetSceneInfoData(), 0);
    }
    {
        VoxelizationLargeTriangleCompSP* shaderProgram =
            dynamic_cast<VoxelizationLargeTriangleCompSP*>(
                m_computePasses.voxelizationLargeTriangle->shaderProgram);
        shaderProgram->sceneInfo.aabbMax = Vec4(m_voxelAABB.GetMax(), 1.0f);
        shaderProgram->sceneInfo.aabbMin = Vec4(m_voxelAABB.GetMin(), 1.0f);
        shaderProgram->UpdateUniformBuffer("uSceneInfo", shaderProgram->GetSceneInfoData(), 0);
    }
    {
        VoxelPreDrawSP* shaderProgram =
            dynamic_cast<VoxelPreDrawSP*>(m_computePasses.voxelPreDraw->shaderProgram);
        shaderProgram->sceneInfo.aabbMax = Vec4(m_voxelAABB.GetMax(), 1.0f);
        shaderProgram->sceneInfo.aabbMin = Vec4(m_voxelAABB.GetMin(), 1.0f);
        shaderProgram->UpdateUniformBuffer("uSceneInfo", shaderProgram->GetSceneInfoData(), 0);
    }
    {
        VoxelDrawSP2* shaderProgram =
            dynamic_cast<VoxelDrawSP2*>(m_gfxPasses.voxelDraw->shaderProgram);
        shaderProgram->transformData.modelMatrix = m_voxelTransform;
        shaderProgram->transformData.projMatrix  = m_scene->GetCamera()->GetProjectionMatrix();
        shaderProgram->transformData.viewMatrix  = m_scene->GetCamera()->GetViewMatrix();

        shaderProgram->UpdateUniformBuffer("uTransformData", shaderProgram->GetTransformData(), 0);
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
    m_renderDevice->UpdateGraphicsPassOnResize(m_gfxPasses.voxelDraw, m_viewport);
}

void ComputeVoxelizer::SetRenderScene(RenderScene* scene)
{
    VoxelizerBase::SetRenderScene(scene);

    const auto center = m_scene->GetAABB().GetCenter();

    glm::vec3 min = center - glm::vec3(m_sceneExtent / 2, m_sceneExtent / 2, m_sceneExtent / 2);
    glm::vec3 max = center + glm::vec3(m_sceneExtent / 2, m_sceneExtent / 2, m_sceneExtent / 2);
    m_voxelAABB   = {min, max};

    auto cubeExtent = m_cube->GetAABB().GetExtent3D();
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

        RHITexture* dummyTex =
            m_renderDevice->CreateTextureDummy(texFormat, {.copyUsage = false}, texName);
        m_renderDevice->DestroyTexture(dummyTex);
    }
}
#endif
// void ComputeVoxelizer::VoxelizeStaticScene() {}
//
// void ComputeVoxelizer::VoxelizeDynamicScene() {}
} // namespace zen::rc