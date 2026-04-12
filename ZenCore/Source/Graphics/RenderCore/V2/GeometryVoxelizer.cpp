#include "Graphics/RenderCore/V2/Renderer/GeometryVoxelizer.h"

#include "Graphics/RenderCore/V2/RenderResource.h"
#include "Graphics/RenderCore/V2/RenderScene.h"
#include "Graphics/RenderCore/V2/ShaderProgram.h"
#include "SceneGraph/Scene.h"
#include "SceneGraph/Camera.h"



namespace zen::rc
{
void GeometryVoxelizer::Init()
{
    m_voxelTexResolution = 256;
    m_voxelTexFormat     = DataFormat::eR32UInt;
    m_voxelCount         = m_voxelTexResolution * m_voxelTexResolution * m_voxelTexResolution;

    PrepareTextures();

    PrepareBuffers();

    BuildGraphicsPasses();
}

void GeometryVoxelizer::Destroy()
{
    VoxelizerBase::Destroy();
}

void GeometryVoxelizer::PrepareTextures()
{
    VoxelizerBase::PrepareTextures();
}

void GeometryVoxelizer::PrepareBuffers()
{
    std::vector<SimpleVertex> vertices;
    vertices.resize(m_voxelCount);

    m_pVoxelVBO = m_pRenderDevice->CreateVertexBuffer(
        m_voxelCount * sizeof(Vec4), reinterpret_cast<const uint8_t*>(vertices.data()));
}

void GeometryVoxelizer::BuildRenderGraph()
{
    m_rdg = MakeUnique<RenderGraph>("geom_voxelize_rdg");
    m_rdg->Begin();
    // voxelization pass
    if (m_needVoxelization)
    {
        VoxelizationSP* pShaderProgram =
            dynamic_cast<VoxelizationSP*>(m_gfxPasses.pVoxelization->pShaderProgram);
        const uint32_t cFbSize = m_voxelTexResolution;
        // std::vector<RHIRenderPassClearValue> clearValues(0);
        // clearValues[0].color = {0.0f, 0.0f, 0.0f, 0.0f};
        Rect2<int> area(0, static_cast<int>(cFbSize), 0, static_cast<int>(cFbSize));
        Rect2<float> viewport(static_cast<float>(cFbSize), static_cast<float>(cFbSize));

        auto* pPass = m_rdg->AddGraphicsPassNode(m_gfxPasses.pVoxelization, "geom_voxelization");

        // TextureHandle textures[]         = {m_voxelTextures.pStaticFlag, m_voxelTextures.pAlbedo,
        //                                          m_voxelTextures.pNormal, m_voxelTextures.pEmissive};
        // RHITextureSubResourceRange ranges[] = {
        //     m_renderDevice->GetTextureSubResourceRange(m_voxelTextures.pStaticFlag),
        //     m_renderDevice->GetTextureSubResourceRange(m_voxelTextures.pAlbedo),
        //     m_renderDevice->GetTextureSubResourceRange(m_voxelTextures.pNormal),
        //     m_renderDevice->GetTextureSubResourceRange(m_voxelTextures.pEmissive)};

        // m_rdg->DeclareTextureAccessForPass(pPass, 4, textures, RHITextureUsage::eStorage, ranges,
        //                                    RHIAccessMode::eReadWrite);
        m_rdg->AddGraphicsPassBindVertexBufferNode(pPass, m_pScene->GetVertexBuffer(), {0});
        m_rdg->AddGraphicsPassBindIndexBufferNode(pPass, m_pScene->GetIndexBuffer(),
                                                  DataFormat::eR32UInt);
        m_rdg->AddGraphicsPassSetViewportNode(pPass, viewport);
        m_rdg->AddGraphicsPassSetScissorNode(pPass, area);
        pShaderProgram->pushConstantsData.flagStaticVoxels = 1;
        pShaderProgram->pushConstantsData.volumeDimension  = m_voxelTexResolution;
        for (auto* node : m_pScene->GetRenderableNodes())
        {
            pShaderProgram->pushConstantsData.nodeIndex = node->GetRenderableIndex();
            for (auto* subMesh : node->GetComponent<sg::Mesh>()->GetSubMeshes())
            {
                pShaderProgram->pushConstantsData.materialIndex = subMesh->GetMaterial()->index;
                m_rdg->AddGraphicsPassSetPushConstants(pPass, &pShaderProgram->pushConstantsData,
                                                       sizeof(VoxelizationSP::PushConstantsData));
                m_rdg->AddGraphicsPassDrawIndexedNode(pPass, subMesh->GetIndexCount(), 1,
                                                      subMesh->GetFirstIndex(), 0, 0);
            }
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
        VoxelDrawSP* pShaderProgram =
            dynamic_cast<VoxelDrawSP*>(m_gfxPasses.pVoxelDraw->pShaderProgram);

        // std::vector<RHIRenderPassClearValue> clearValues(2);
        // clearValues[0].color   = {0.0f, 0.0f, 0.0f, 0.0f};
        // clearValues[1].depth   = 1.0f;
        // clearValues[1].stencil = 0;

        Rect2<int> area(0, static_cast<int>(m_pViewport->GetWidth()), 0,
                        static_cast<int>(m_pViewport->GetHeight()));
        Rect2<float> viewport(static_cast<float>(m_pViewport->GetWidth()),
                              static_cast<float>(m_pViewport->GetHeight()));

        auto* pPass = m_rdg->AddGraphicsPassNode(m_gfxPasses.pVoxelDraw, "geom_voxel_draw");

        // m_rdg->DeclareTextureAccessForPass(
        //     pPass, m_voxelTextures.pAlbedo, RHITextureUsage::eStorage,
        //     m_renderDevice->GetTextureSubResourceRange(m_voxelTextures.pAlbedo), RHIAccessMode::eRead);
        // m_rdg->AddGraphicsPassBindVertexBufferNode(pPass, m_voxelVBO, {0});
        m_rdg->AddGraphicsPassSetViewportNode(pPass, viewport);
        m_rdg->AddGraphicsPassSetScissorNode(pPass, area);
        //        pShaderProgram->pushConstantsData.colorChannels   = m_config.drawColorChannels;
        pShaderProgram->pushConstantsData.volumeDimension = m_voxelTexResolution;
        m_rdg->AddGraphicsPassSetPushConstants(pPass, &pShaderProgram->pushConstantsData,
                                               sizeof(VoxelDrawSP::PushConstantsData));
        m_rdg->AddGraphicsPassDrawNode(pPass, m_voxelCount, 1);
    }
    m_rdg->End();
}

void GeometryVoxelizer::BuildGraphicsPasses()
{
    // voxelization graphics pass, set static flag
    {
        // disable depth cull, color write, depth stencil test and write.
        RHIGfxPipelineStates pso{};
        pso.rasterizationState = {};
        pso.depthStencilState =
            RHIGfxPipelineDepthStencilState::Create(false, false, RHIDepthCompareOperator::eNever);
        pso.multiSampleState = {};
        pso.colorBlendState.AddAttachment();
        pso.dynamicStates.Enable(RHIDynamicState::eScissor, RHIDynamicState::eViewPort);

        rc::GraphicsPassBuilder builder(m_pRenderDevice);
        m_gfxPasses.pVoxelization =
            builder
                .SetShaderProgramName("VoxelizationSP")
                // .SetNumSamples(SampleCount::e1)
                .SetPipelineState(pso)
                //.AddColorRenderTarget(DataFormat::eR8G8B8A8SRGB, RHITextureUsage::eColorAttachment,
                //                      m_voxelTextures.offscreen1)
                .SetFramebufferInfo(m_pViewport, m_voxelTexResolution, m_voxelTexResolution)
                .SetTag("Voxelization")
                .Build();
    }
    // voxel draw graphics pPass
    {
        RHIGfxPipelineStates pso{};
        pso.primitiveType                = RHIDrawPrimitiveType::ePointList;
        pso.rasterizationState           = {};
        pso.rasterizationState.cullMode  = RHIPolygonCullMode::eBack;
        pso.rasterizationState.frontFace = RHIPolygonFrontFace::eCounterClockWise;

        pso.depthStencilState =
            RHIGfxPipelineDepthStencilState::Create(true, true, RHIDepthCompareOperator::eLess);
        pso.multiSampleState = {};
        pso.colorBlendState.AddAttachment();
        pso.dynamicStates.Enable(RHIDynamicState::eScissor, RHIDynamicState::eViewPort);


        rc::GraphicsPassBuilder builder(m_pRenderDevice);
        m_gfxPasses.pVoxelDraw =
            builder
                .SetShaderProgramName("VoxelDrawSP")
                // .SetNumSamples(SampleCount::e1)
                .SetPipelineState(pso)
                .AddViewportColorRT(m_pViewport, RHIRenderTargetLoadOp::eLoad)
                .SetViewportDepthStencilRT(m_pViewport, RHIRenderTargetLoadOp::eClear,
                                           RHIRenderTargetStoreOp::eStore)
                .SetFramebufferInfo(m_pViewport)
                .SetTag("VoxelDraw")
                .Build();
    }
}

void GeometryVoxelizer::UpdatePassResources()
{
    // voxelization pass
    {
        HeapVector<RHIShaderResourceBinding> set0bindings;
        HeapVector<RHIShaderResourceBinding> set1bindings;
        HeapVector<RHIShaderResourceBinding> set2bindings;
        // set-0 bindings
        ADD_SHADER_BINDING_SINGLE(set0bindings, 0, RHIShaderResourceType::eStorageBuffer,
                                  m_pScene->GetNodesDataSSBO());
        ADD_SHADER_BINDING_SINGLE(
            set0bindings, 1, RHIShaderResourceType::eUniformBuffer,
            m_gfxPasses.pVoxelization->pShaderProgram->GetUniformBufferHandle("uVoxelConfig"));
        ADD_SHADER_BINDING_SINGLE(set0bindings, 2, RHIShaderResourceType::eStorageBuffer,
                                  m_pScene->GetMaterialsDataSSBO());

        // set-1 bindings
        ADD_SHADER_BINDING_SINGLE(set1bindings, 0, RHIShaderResourceType::eImage,
                                  m_voxelTextures.pAlbedo);
        ADD_SHADER_BINDING_SINGLE(set1bindings, 1, RHIShaderResourceType::eImage,
                                  m_voxelTextures.pNormal);
        ADD_SHADER_BINDING_SINGLE(set1bindings, 2, RHIShaderResourceType::eImage,
                                  m_voxelTextures.pEmissive);
        ADD_SHADER_BINDING_SINGLE(set1bindings, 3, RHIShaderResourceType::eImage,
                                  m_voxelTextures.pStaticFlag);
        // set-2 bindings: texture array
        ADD_SHADER_BINDING_TEXTURE_ARRAY(set2bindings, 0,
                                         RHIShaderResourceType::eSamplerWithTexture, m_pColorSampler,
                                         m_pScene->GetSceneTextures())

        rc::GraphicsPassResourceUpdater updater(m_pRenderDevice, m_gfxPasses.pVoxelization);
        updater.SetShaderResourceBinding(0, std::move(set0bindings))
            .SetShaderResourceBinding(1, std::move(set1bindings))
            .SetShaderResourceBinding(2, std::move(set2bindings))
            .Update();
    }
    // voxel draw pass
    {
        HeapVector<RHIShaderResourceBinding> set0bindings;
        ADD_SHADER_BINDING_SINGLE(set0bindings, 0, RHIShaderResourceType::eImage,
                                  m_voxelTextures.pAlbedoProxy);
        ADD_SHADER_BINDING_SINGLE(
            set0bindings, 1, RHIShaderResourceType::eUniformBuffer,
            m_gfxPasses.pVoxelDraw->pShaderProgram->GetUniformBufferHandle("uVoxelInfo"));

        rc::GraphicsPassResourceUpdater updater(m_pRenderDevice, m_gfxPasses.pVoxelDraw);
        updater.SetShaderResourceBinding(0, std::move(set0bindings)).Update();
    }
}

void GeometryVoxelizer::UpdateUniformData()
{
    const sg::AABB& sceneAABB = m_pScene->GetAABB();

    auto center = sceneAABB.GetCenter();
    {
        auto halfSize = m_sceneExtent / 2.0f;
        // projection matrices
        auto projection = glm::ortho(-halfSize, halfSize, -halfSize, halfSize, 0.0f, m_sceneExtent);
        projection[1][1] *= -1;

        // view matrices
        VoxelizationSP* pShaderProgram =
            dynamic_cast<VoxelizationSP*>(m_gfxPasses.pVoxelization->pShaderProgram);
        pShaderProgram->voxelConfigData.viewProjectionMatrices[0] =
            glm::lookAt(center - Vec3(halfSize, 0.0f, 0.0f), center, Vec3(0.0f, 1.0f, 0.0f));
        pShaderProgram->voxelConfigData.viewProjectionMatrices[1] =
            glm::lookAt(center - Vec3(0.0f, halfSize, 0.0f), center, Vec3(-1.0f, 0.0f, 0.0f));
        pShaderProgram->voxelConfigData.viewProjectionMatrices[2] =
            glm::lookAt(center - Vec3(0.0f, 0.0f, halfSize), center, Vec3(0.0f, 1.0f, 0.0f));

        pShaderProgram->voxelConfigData.worldMinPointScale =
            Vec4(m_pScene->GetAABB().GetMin(), 1.0f / m_sceneExtent);

        for (int i = 0; i < 3; ++i)
        {
            pShaderProgram->voxelConfigData.viewProjectionMatrices[i] =
                projection * pShaderProgram->voxelConfigData.viewProjectionMatrices[i];
            pShaderProgram->voxelConfigData.viewProjectionMatricesI[i] =
                glm::inverse(pShaderProgram->voxelConfigData.viewProjectionMatrices[i]);
        }

        pShaderProgram->UpdateUniformBuffer("uVoxelConfig", pShaderProgram->GetVoxelConfigData(), 0);
    }

    {
        const auto* pCameraUniformData =
            reinterpret_cast<const sg::CameraUniformData*>(m_pScene->GetCameraUniformData());
        VoxelDrawSP* pShaderProgram =
            dynamic_cast<VoxelDrawSP*>(m_gfxPasses.pVoxelDraw->pShaderProgram);
        uint32_t drawMipLevel = 0;
        auto vDimension  = static_cast<unsigned>(m_voxelTexResolution / pow(2.0f, drawMipLevel));
        auto vSize       = m_sceneExtent / vDimension;
        auto modelMatrix = glm::translate(Mat4(1.0f), sceneAABB.GetMin()) *
            glm::scale(Mat4(1.0f), glm::vec3(vSize));
        pShaderProgram->voxelInfo.modelViewProjection =
            pCameraUniformData->projViewMatrix * modelMatrix;

        auto& planes = m_pScene->GetCamera()->GetFrustum().GetPlanes();
        for (auto i = 0; i < 6; i++)
        {
            pShaderProgram->voxelInfo.frustumPlanes[i] = planes[i];
        }
        pShaderProgram->voxelInfo.worldMinPointVoxelSize = Vec4(sceneAABB.GetMin(), m_voxelSize);

        pShaderProgram->UpdateUniformBuffer("uVoxelInfo", pShaderProgram->GetVoxelInfoData(), 0);
    }
}


void GeometryVoxelizer::PrepareRenderWorkload()
{
    if (m_rebuildRDG)
    {
        BuildRenderGraph();
        //        m_rebuildRDG = false;
    }
    UpdateUniformData();
    // VoxelizationProgram* voxelizationSP =
    //     dynamic_cast<VoxelizationProgram*>(m_gfxPasses.pVoxelization.pShaderProgram);
    // voxelizationSP->UpdateUniformBuffer("uVoxelConfig", voxelizationSP->GetVoxelConfigData(), 0);
    //
    // VoxelDrawShaderProgram* voxelDrawSP =
    //     dynamic_cast<VoxelDrawShaderProgram*>(m_gfxPasses.pVoxelDraw.pShaderProgram);
    // voxelDrawSP->UpdateUniformBuffer("uVoxelInfo", voxelDrawSP->GetVoxelInfoData(), 0);
}


void GeometryVoxelizer::OnResize()
{
    m_rebuildRDG = true;
    m_pRenderDevice->UpdateGraphicsPassOnResize(m_gfxPasses.pVoxelDraw, m_pViewport);
}

// void GeometryVoxelizer::VoxelizeStaticScene() {}
//
// void GeometryVoxelizer::VoxelizeDynamicScene() {}
} // namespace zen::rc