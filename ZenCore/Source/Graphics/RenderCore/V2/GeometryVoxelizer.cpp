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

    m_voxelVBO = m_renderDevice->CreateVertexBuffer(
        m_voxelCount * sizeof(Vec4), reinterpret_cast<const uint8_t*>(vertices.data()));
}

void GeometryVoxelizer::BuildRenderGraph()
{
    m_rdg = MakeUnique<RenderGraph>("geom_voxelize_rdg");
    m_rdg->Begin();
    // voxelization pass
    if (m_needVoxelization)
    {
        VoxelizationSP* shaderProgram =
            dynamic_cast<VoxelizationSP*>(m_gfxPasses.voxelization->shaderProgram);
        const uint32_t cFbSize = m_voxelTexResolution;
        std::vector<RHIRenderPassClearValue> clearValues(0);
        // clearValues[0].color = {0.0f, 0.0f, 0.0f, 0.0f};
        Rect2<int> area(0, static_cast<int>(cFbSize), 0, static_cast<int>(cFbSize));
        Rect2<float> viewport(static_cast<float>(cFbSize), static_cast<float>(cFbSize));

        auto* pass = m_rdg->AddGraphicsPassNode(m_gfxPasses.voxelization, area, clearValues,
                                                "geom_voxelization");

        // TextureHandle textures[]         = {m_voxelTextures.staticFlag, m_voxelTextures.albedo,
        //                                          m_voxelTextures.normal, m_voxelTextures.emissive};
        // RHITextureSubResourceRange ranges[] = {
        //     m_renderDevice->GetTextureSubResourceRange(m_voxelTextures.staticFlag),
        //     m_renderDevice->GetTextureSubResourceRange(m_voxelTextures.albedo),
        //     m_renderDevice->GetTextureSubResourceRange(m_voxelTextures.normal),
        //     m_renderDevice->GetTextureSubResourceRange(m_voxelTextures.emissive)};

        // m_rdg->DeclareTextureAccessForPass(pass, 4, textures, RHITextureUsage::eStorage, ranges,
        //                                    RHIAccessMode::eReadWrite);
        m_rdg->AddGraphicsPassBindVertexBufferNode(pass, m_scene->GetVertexBuffer(), {0});
        m_rdg->AddGraphicsPassBindIndexBufferNode(pass, m_scene->GetIndexBuffer(),
                                                  DataFormat::eR32UInt);
        m_rdg->AddGraphicsPassSetViewportNode(pass, viewport);
        m_rdg->AddGraphicsPassSetScissorNode(pass, area);
        shaderProgram->pushConstantsData.flagStaticVoxels = 1;
        shaderProgram->pushConstantsData.volumeDimension  = m_voxelTexResolution;
        for (auto* node : m_scene->GetRenderableNodes())
        {
            shaderProgram->pushConstantsData.nodeIndex = node->GetRenderableIndex();
            for (auto* subMesh : node->GetComponent<sg::Mesh>()->GetSubMeshes())
            {
                shaderProgram->pushConstantsData.materialIndex = subMesh->GetMaterial()->index;
                m_rdg->AddGraphicsPassSetPushConstants(pass, &shaderProgram->pushConstantsData,
                                                       sizeof(VoxelizationSP::PushConstantsData));
                m_rdg->AddGraphicsPassDrawIndexedNode(pass, subMesh->GetIndexCount(), 1,
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
        VoxelDrawSP* shaderProgram =
            dynamic_cast<VoxelDrawSP*>(m_gfxPasses.voxelDraw->shaderProgram);

        std::vector<RHIRenderPassClearValue> clearValues(2);
        clearValues[0].color   = {0.0f, 0.0f, 0.0f, 0.0f};
        clearValues[1].depth   = 1.0f;
        clearValues[1].stencil = 0;

        Rect2<int> area(0, static_cast<int>(m_viewport->GetWidth()), 0,
                        static_cast<int>(m_viewport->GetHeight()));
        Rect2<float> viewport(static_cast<float>(m_viewport->GetWidth()),
                              static_cast<float>(m_viewport->GetHeight()));

        auto* pass =
            m_rdg->AddGraphicsPassNode(m_gfxPasses.voxelDraw, area, clearValues, "geom_voxel_draw");

        // m_rdg->DeclareTextureAccessForPass(
        //     pass, m_voxelTextures.albedo, RHITextureUsage::eStorage,
        //     m_renderDevice->GetTextureSubResourceRange(m_voxelTextures.albedo), RHIAccessMode::eRead);
        // m_rdg->AddGraphicsPassBindVertexBufferNode(pass, m_voxelVBO, {0});
        m_rdg->AddGraphicsPassSetViewportNode(pass, viewport);
        m_rdg->AddGraphicsPassSetScissorNode(pass, area);
        //        shaderProgram->pushConstantsData.colorChannels   = m_config.drawColorChannels;
        shaderProgram->pushConstantsData.volumeDimension = m_voxelTexResolution;
        m_rdg->AddGraphicsPassSetPushConstants(pass, &shaderProgram->pushConstantsData,
                                               sizeof(VoxelDrawSP::PushConstantsData));
        m_rdg->AddGraphicsPassDrawNode(pass, m_voxelCount, 1);
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
        pso.colorBlendState  = RHIGfxPipelineColorBlendState::CreateDisabled();
        pso.dynamicStates.push_back(RHIDynamicState::eScissor);
        pso.dynamicStates.push_back(RHIDynamicState::eViewPort);

        rc::GraphicsPassBuilder builder(m_renderDevice);
        m_gfxPasses.voxelization =
            builder
                .SetShaderProgramName("VoxelizationSP")
                // .SetNumSamples(SampleCount::e1)
                .SetPipelineState(pso)
                //.AddColorRenderTarget(DataFormat::eR8G8B8A8SRGB, RHITextureUsage::eColorAttachment,
                //                      m_voxelTextures.offscreen1)
                .SetFramebufferInfo(m_viewport, m_voxelTexResolution, m_voxelTexResolution)
                .SetTag("Voxelization")
                .Build();
    }
    // voxel draw graphics pass
    {
        RHIGfxPipelineStates pso{};
        pso.primitiveType                = RHIDrawPrimitiveType::ePointList;
        pso.rasterizationState           = {};
        pso.rasterizationState.cullMode  = RHIPolygonCullMode::eBack;
        pso.rasterizationState.frontFace = RHIPolygonFrontFace::eCounterClockWise;

        pso.depthStencilState =
            RHIGfxPipelineDepthStencilState::Create(true, true, RHIDepthCompareOperator::eLess);
        pso.multiSampleState = {};
        pso.colorBlendState  = RHIGfxPipelineColorBlendState::CreateDisabled(1);
        pso.dynamicStates.push_back(RHIDynamicState::eScissor);
        pso.dynamicStates.push_back(RHIDynamicState::eViewPort);

        rc::GraphicsPassBuilder builder(m_renderDevice);
        m_gfxPasses.voxelDraw =
            builder
                .SetShaderProgramName("VoxelDrawSP")
                // .SetNumSamples(SampleCount::e1)
                .SetPipelineState(pso)
                .AddViewportColorRT(m_viewport, RHIRenderTargetLoadOp::eLoad)
                .SetViewportDepthStencilRT(m_viewport, RHIRenderTargetLoadOp::eClear,
                                           RHIRenderTargetStoreOp::eStore)
                .SetFramebufferInfo(m_viewport)
                .SetTag("VoxelDraw")
                .Build();
    }
}

void GeometryVoxelizer::UpdatePassResources()
{
    // voxelization pass
    {
        std::vector<RHIShaderResourceBinding> set0bindings;
        std::vector<RHIShaderResourceBinding> set1bindings;
        std::vector<RHIShaderResourceBinding> set2bindings;
        // set-0 bindings
        ADD_SHADER_BINDING_SINGLE(set0bindings, 0, RHIShaderResourceType::eStorageBuffer,
                                  m_scene->GetNodesDataSSBO());
        ADD_SHADER_BINDING_SINGLE(
            set0bindings, 1, RHIShaderResourceType::eUniformBuffer,
            m_gfxPasses.voxelization->shaderProgram->GetUniformBufferHandle("uVoxelConfig"));
        ADD_SHADER_BINDING_SINGLE(set0bindings, 2, RHIShaderResourceType::eStorageBuffer,
                                  m_scene->GetMaterialsDataSSBO());

        // set-1 bindings
        ADD_SHADER_BINDING_SINGLE(set1bindings, 0, RHIShaderResourceType::eImage,
                                  m_voxelTextures.albedo);
        ADD_SHADER_BINDING_SINGLE(set1bindings, 1, RHIShaderResourceType::eImage,
                                  m_voxelTextures.normal);
        ADD_SHADER_BINDING_SINGLE(set1bindings, 2, RHIShaderResourceType::eImage,
                                  m_voxelTextures.emissive);
        ADD_SHADER_BINDING_SINGLE(set1bindings, 3, RHIShaderResourceType::eImage,
                                  m_voxelTextures.staticFlag);
        // set-2 bindings: texture array
        ADD_SHADER_BINDING_TEXTURE_ARRAY(set2bindings, 0,
                                         RHIShaderResourceType::eSamplerWithTexture, m_colorSampler,
                                         m_scene->GetSceneTextures())

        rc::GraphicsPassResourceUpdater updater(m_renderDevice, m_gfxPasses.voxelization);
        updater.SetShaderResourceBinding(0, set0bindings)
            .SetShaderResourceBinding(1, set1bindings)
            .SetShaderResourceBinding(2, set2bindings)
            .Update();
    }
    // voxel draw pass
    {
        std::vector<RHIShaderResourceBinding> set0bindings;
        ADD_SHADER_BINDING_SINGLE(set0bindings, 0, RHIShaderResourceType::eImage,
                                  m_voxelTextures.albedoProxy);
        ADD_SHADER_BINDING_SINGLE(
            set0bindings, 1, RHIShaderResourceType::eUniformBuffer,
            m_gfxPasses.voxelDraw->shaderProgram->GetUniformBufferHandle("uVoxelInfo"));

        rc::GraphicsPassResourceUpdater updater(m_renderDevice, m_gfxPasses.voxelDraw);
        updater.SetShaderResourceBinding(0, set0bindings).Update();
    }
}

void GeometryVoxelizer::UpdateUniformData()
{
    const sg::AABB& sceneAABB = m_scene->GetAABB();

    auto center = sceneAABB.GetCenter();
    {
        auto halfSize = m_sceneExtent / 2.0f;
        // projection matrices
        auto projection = glm::ortho(-halfSize, halfSize, -halfSize, halfSize, 0.0f, m_sceneExtent);
        projection[1][1] *= -1;

        // view matrices
        VoxelizationSP* shaderProgram =
            dynamic_cast<VoxelizationSP*>(m_gfxPasses.voxelization->shaderProgram);
        shaderProgram->voxelConfigData.viewProjectionMatrices[0] =
            glm::lookAt(center - Vec3(halfSize, 0.0f, 0.0f), center, Vec3(0.0f, 1.0f, 0.0f));
        shaderProgram->voxelConfigData.viewProjectionMatrices[1] =
            glm::lookAt(center - Vec3(0.0f, halfSize, 0.0f), center, Vec3(-1.0f, 0.0f, 0.0f));
        shaderProgram->voxelConfigData.viewProjectionMatrices[2] =
            glm::lookAt(center - Vec3(0.0f, 0.0f, halfSize), center, Vec3(0.0f, 1.0f, 0.0f));

        shaderProgram->voxelConfigData.worldMinPointScale =
            Vec4(m_scene->GetAABB().GetMin(), 1.0f / m_sceneExtent);

        for (int i = 0; i < 3; ++i)
        {
            shaderProgram->voxelConfigData.viewProjectionMatrices[i] =
                projection * shaderProgram->voxelConfigData.viewProjectionMatrices[i];
            shaderProgram->voxelConfigData.viewProjectionMatricesI[i] =
                glm::inverse(shaderProgram->voxelConfigData.viewProjectionMatrices[i]);
        }

        shaderProgram->UpdateUniformBuffer("uVoxelConfig", shaderProgram->GetVoxelConfigData(), 0);
    }

    {
        const auto* cameraUniformData =
            reinterpret_cast<const sg::CameraUniformData*>(m_scene->GetCameraUniformData());
        VoxelDrawSP* shaderProgram =
            dynamic_cast<VoxelDrawSP*>(m_gfxPasses.voxelDraw->shaderProgram);
        uint32_t drawMipLevel = 0;
        auto vDimension  = static_cast<unsigned>(m_voxelTexResolution / pow(2.0f, drawMipLevel));
        auto vSize       = m_sceneExtent / vDimension;
        auto modelMatrix = glm::translate(Mat4(1.0f), sceneAABB.GetMin()) *
            glm::scale(Mat4(1.0f), glm::vec3(vSize));
        shaderProgram->voxelInfo.modelViewProjection =
            cameraUniformData->projViewMatrix * modelMatrix;

        auto& planes = m_scene->GetCamera()->GetFrustum().GetPlanes();
        for (auto i = 0; i < 6; i++)
        {
            shaderProgram->voxelInfo.frustumPlanes[i] = planes[i];
        }
        shaderProgram->voxelInfo.worldMinPointVoxelSize = Vec4(sceneAABB.GetMin(), m_voxelSize);

        shaderProgram->UpdateUniformBuffer("uVoxelInfo", shaderProgram->GetVoxelInfoData(), 0);
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
    //     dynamic_cast<VoxelizationProgram*>(m_gfxPasses.voxelization.shaderProgram);
    // voxelizationSP->UpdateUniformBuffer("uVoxelConfig", voxelizationSP->GetVoxelConfigData(), 0);
    //
    // VoxelDrawShaderProgram* voxelDrawSP =
    //     dynamic_cast<VoxelDrawShaderProgram*>(m_gfxPasses.voxelDraw.shaderProgram);
    // voxelDrawSP->UpdateUniformBuffer("uVoxelInfo", voxelDrawSP->GetVoxelInfoData(), 0);
}


void GeometryVoxelizer::OnResize()
{
    m_rebuildRDG = true;
    m_renderDevice->UpdateGraphicsPassOnResize(m_gfxPasses.voxelDraw, m_viewport);
}

// void GeometryVoxelizer::VoxelizeStaticScene() {}
//
// void GeometryVoxelizer::VoxelizeDynamicScene() {}
} // namespace zen::rc