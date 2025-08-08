#include "Graphics/RenderCore/V2/Renderer/GeometryVoxelizer.h"
#include "Graphics/RenderCore/V2/RenderScene.h"
#include "Graphics/RenderCore/V2/ShaderProgram.h"
#include "Graphics/RenderCore/V2/TextureManager.h"
#include "SceneGraph/Scene.h"
#include "SceneGraph/Camera.h"

using namespace zen::rhi;

namespace zen::rc
{
void GeometryVoxelizer::Init()
{
    m_voxelTexResolution = 256;
    m_voxelTexFormat     = DataFormat::eR32UInt;
    m_voxelCount         = m_voxelTexResolution * m_voxelTexResolution * m_voxelTexResolution;

    m_config.drawColorChannels = Vec4(1.0f);

    PrepareTextures();

    PrepareBuffers();

    BuildGraphicsPasses();
}

void GeometryVoxelizer::Destroy() {}

void GeometryVoxelizer::PrepareTextures()
{
    VoxelizerBase::PrepareTextures();
    using namespace zen::rhi;
    {
        INIT_TEXTURE_INFO(texInfo, rhi::TextureType::e2D, DataFormat::eR8G8B8A8SRGB,
                          m_voxelTexResolution, m_voxelTexResolution, 1, 1, 1, SampleCount::e1,
                          "voxel_offscreen1", TextureUsageFlagBits::eColorAttachment,
                          TextureUsageFlagBits::eSampled);
        m_voxelTextures.offscreen1 = m_renderDevice->CreateTexture(texInfo, texInfo.name);
    }
    {
        INIT_TEXTURE_INFO(texInfo, rhi::TextureType::e2D, DataFormat::eR8G8B8A8UNORM,
                          m_viewport->GetWidth(), m_viewport->GetHeight(), 1, 1, 1, SampleCount::e1,
                          "voxel_offscreen2", TextureUsageFlagBits::eColorAttachment,
                          TextureUsageFlagBits::eSampled);
        m_voxelTextures.offscreen2 = m_renderDevice->CreateTexture(texInfo, texInfo.name);
    }
    {
        INIT_TEXTURE_INFO(texInfo, rhi::TextureType::e3D, DataFormat::eR8UNORM,
                          m_voxelTexResolution, m_voxelTexResolution, m_voxelTexResolution, 1, 1,
                          SampleCount::e1, "voxel_static_flag", TextureUsageFlagBits::eStorage,
                          TextureUsageFlagBits::eSampled);
        m_voxelTextures.staticFlag = m_renderDevice->CreateTexture(texInfo, texInfo.name);
    }
    {
        INIT_TEXTURE_INFO(texInfo, rhi::TextureType::e3D, m_voxelTexFormat, m_voxelTexResolution,
                          m_voxelTexResolution, m_voxelTexResolution, 1, 1, SampleCount::e1,
                          "voxel_albedo", TextureUsageFlagBits::eStorage,
                          TextureUsageFlagBits::eSampled);
        texInfo.mutableFormat  = true;
        m_voxelTextures.albedo = m_renderDevice->CreateTexture(texInfo, texInfo.name);
    }
    {
        rhi::TextureProxyInfo textureProxyInfo{};
        textureProxyInfo.type        = rhi::TextureType::e3D;
        textureProxyInfo.arrayLayers = 1;
        textureProxyInfo.mipmaps     = 1;
        textureProxyInfo.format      = DataFormat::eR8G8B8A8UNORM;
        textureProxyInfo.name        = "voxel_albedo_proxy";
        m_voxelTextures.albedoProxy =
            m_renderDevice->CreateTextureProxy(m_voxelTextures.albedo, textureProxyInfo);
    }
    {
        INIT_TEXTURE_INFO(texInfo, rhi::TextureType::e3D, m_voxelTexFormat, m_voxelTexResolution,
                          m_voxelTexResolution, m_voxelTexResolution, 1, 1, SampleCount::e1,
                          "voxel_normal", TextureUsageFlagBits::eStorage,
                          TextureUsageFlagBits::eSampled);
        texInfo.mutableFormat  = true;
        m_voxelTextures.normal = m_renderDevice->CreateTexture(texInfo, texInfo.name);
    }
    {
        rhi::TextureProxyInfo textureProxyInfo{};
        textureProxyInfo.type        = rhi::TextureType::e3D;
        textureProxyInfo.arrayLayers = 1;
        textureProxyInfo.mipmaps     = 1;
        textureProxyInfo.format      = DataFormat::eR8G8B8A8UNORM;
        textureProxyInfo.name        = "voxel_normal_proxy";
        m_voxelTextures.normalProxy =
            m_renderDevice->CreateTextureProxy(m_voxelTextures.normal, textureProxyInfo);
    }
    {
        INIT_TEXTURE_INFO(texInfo, rhi::TextureType::e3D, m_voxelTexFormat, m_voxelTexResolution,
                          m_voxelTexResolution, m_voxelTexResolution, 1, 1, SampleCount::e1,
                          "voxel_emissive", TextureUsageFlagBits::eStorage,
                          TextureUsageFlagBits::eSampled);
        texInfo.mutableFormat    = true;
        m_voxelTextures.emissive = m_renderDevice->CreateTexture(texInfo, texInfo.name);
    }
    {
        rhi::TextureProxyInfo textureProxyInfo{};
        textureProxyInfo.type        = rhi::TextureType::e3D;
        textureProxyInfo.arrayLayers = 1;
        textureProxyInfo.mipmaps     = 1;
        textureProxyInfo.format      = DataFormat::eR8G8B8A8UNORM;
        textureProxyInfo.name        = "voxel_emissive_proxy";
        m_voxelTextures.emissiveProxy =
            m_renderDevice->CreateTextureProxy(m_voxelTextures.emissive, textureProxyInfo);
    }
    {
        INIT_TEXTURE_INFO(texInfo, rhi::TextureType::e3D, m_voxelTexFormat, m_voxelTexResolution,
                          m_voxelTexResolution, m_voxelTexResolution, 1, 1, SampleCount::e1,
                          "voxel_radiance", TextureUsageFlagBits::eStorage,
                          TextureUsageFlagBits::eSampled);
        m_voxelTextures.radiance = m_renderDevice->CreateTexture(texInfo, texInfo.name);
    }
    const auto halfDim = m_voxelTexResolution / 2;
    for (uint32_t i = 0; i < 6; i++)
    {
        const auto texName = "voxel_mipmap_face_" + std::to_string(i);
        INIT_TEXTURE_INFO(texInfo, rhi::TextureType::e3D, m_voxelTexFormat, halfDim, halfDim,
                          halfDim, CalculateTextureMipLevels(halfDim, halfDim, halfDim), 1,
                          SampleCount::e1, texName, TextureUsageFlagBits::eStorage,
                          TextureUsageFlagBits::eSampled);
        m_voxelTextures.mipmaps[i] = m_renderDevice->CreateTexture(texInfo, texInfo.name);
        //        m_RHI->ChangeTextureLayout(m_renderDevice->GetCurrentUploadCmdList(),
        //                                   m_voxelTextures.mipmaps[i], TextureLayout::eUndefined,
        //                                   TextureLayout::eGeneral);
    }
    m_RHI->WaitForCommandList(m_renderDevice->GetCurrentUploadCmdList());
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
    m_rdg = MakeUnique<RenderGraph>();
    m_rdg->Begin();
    // voxelization pass
    if (m_needVoxelization)
    {
        VoxelizationSP* shaderProgram =
            dynamic_cast<VoxelizationSP*>(m_gfxPasses.voxelization.shaderProgram);
        const uint32_t cFbSize = m_voxelTexResolution;
        std::vector<RenderPassClearValue> clearValues(1);
        clearValues[0].color = {0.0f, 0.0f, 0.0f, 0.0f};
        Rect2<int> area(0, static_cast<int>(cFbSize), 0, static_cast<int>(cFbSize));
        Rect2<float> viewport(static_cast<float>(cFbSize), static_cast<float>(cFbSize));

        auto* pass =
            m_rdg->AddGraphicsPassNode(m_gfxPasses.voxelization, area, clearValues, false, false);

        rhi::TextureHandle textures[]         = {m_voxelTextures.staticFlag, m_voxelTextures.albedo,
                                                 m_voxelTextures.normal, m_voxelTextures.emissive};
        rhi::TextureSubResourceRange ranges[] = {
            m_renderDevice->GetTextureSubResourceRange(m_voxelTextures.staticFlag),
            m_renderDevice->GetTextureSubResourceRange(m_voxelTextures.albedo),
            m_renderDevice->GetTextureSubResourceRange(m_voxelTextures.normal),
            m_renderDevice->GetTextureSubResourceRange(m_voxelTextures.emissive)};

        m_rdg->DeclareTextureAccessForPass(pass, 4, textures, TextureUsage::eStorage, ranges,
                                           AccessMode::eReadWrite);
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
                                                       sizeof(VoxelizationSP::PushConstantData));
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
            dynamic_cast<VoxelDrawSP*>(m_gfxPasses.voxelDraw.shaderProgram);

        std::vector<RenderPassClearValue> clearValues(2);
        clearValues[0].color   = {0.0f, 0.0f, 0.0f, 0.0f};
        clearValues[1].depth   = 1.0f;
        clearValues[1].stencil = 0;

        Rect2<int> area(0, static_cast<int>(m_viewport->GetWidth()), 0,
                        static_cast<int>(m_viewport->GetHeight()));
        Rect2<float> viewport(static_cast<float>(m_viewport->GetWidth()),
                              static_cast<float>(m_viewport->GetHeight()));

        auto* pass =
            m_rdg->AddGraphicsPassNode(m_gfxPasses.voxelDraw, area, clearValues, true, true);

        // m_rdg->AddGraphicsPassBindVertexBufferNode(pass, m_voxelVBO, {0});
        m_rdg->AddGraphicsPassSetViewportNode(pass, viewport);
        m_rdg->AddGraphicsPassSetScissorNode(pass, area);
        //        shaderProgram->pushConstantsData.colorChannels   = m_config.drawColorChannels;
        shaderProgram->pushConstantsData.volumeDimension = m_voxelTexResolution;
        m_rdg->AddGraphicsPassSetPushConstants(pass, &shaderProgram->pushConstantsData,
                                               sizeof(VoxelDrawSP::PushConstantData));
        m_rdg->AddGraphicsPassDrawNode(pass, m_voxelCount, 1);
    }
    m_rdg->End();
}

void GeometryVoxelizer::BuildGraphicsPasses()
{
    // voxelization graphics pass, set static flag
    {
        // disable depth cull, color write, depth stencil test and write.
        GfxPipelineStates pso{};
        pso.rasterizationState = {};
        pso.depthStencilState =
            GfxPipelineDepthStencilState::Create(false, false, CompareOperator::eNever);
        pso.multiSampleState = {};
        pso.colorBlendState  = GfxPipelineColorBlendState::CreateDisabled();
        pso.dynamicStates.push_back(DynamicState::eScissor);
        pso.dynamicStates.push_back(DynamicState::eViewPort);

        rc::GraphicsPassBuilder builder(m_renderDevice);
        m_gfxPasses.voxelization =
            builder.SetShaderProgramName("VoxelizationSP")
                .SetNumSamples(SampleCount::e1)
                .SetPipelineState(pso)
                //.AddColorRenderTarget(DataFormat::eR8G8B8A8SRGB, TextureUsage::eColorAttachment,
                //                      m_voxelTextures.offscreen1)
                .SetFramebufferInfo(m_viewport, m_voxelTexResolution, m_voxelTexResolution)
                .SetTag("Voxelization")
                .Build();
    }
    // voxel draw graphics pass
    {
        GfxPipelineStates pso{};
        pso.primitiveType                = DrawPrimitiveType::ePointList;
        pso.rasterizationState           = {};
        pso.rasterizationState.cullMode  = PolygonCullMode::eBack;
        pso.rasterizationState.frontFace = PolygonFrontFace::eCounterClockWise;

        pso.depthStencilState =
            GfxPipelineDepthStencilState::Create(true, true, CompareOperator::eLess);
        pso.multiSampleState = {};
        pso.colorBlendState  = GfxPipelineColorBlendState::CreateDisabled(1);
        pso.dynamicStates.push_back(DynamicState::eScissor);
        pso.dynamicStates.push_back(DynamicState::eViewPort);

        rc::GraphicsPassBuilder builder(m_renderDevice);
        m_gfxPasses.voxelDraw =
            builder.SetShaderProgramName("VoxelDrawSP")
                .SetNumSamples(SampleCount::e1)
                .SetPipelineState(pso)
                .AddColorRenderTarget(m_viewport->GetSwapchainFormat(),
                                      TextureUsage::eColorAttachment,
                                      m_viewport->GetColorBackBuffer())
                .SetDepthStencilTarget(m_viewport->GetDepthStencilFormat(),
                                       m_viewport->GetDepthStencilBackBuffer(),
                                       RenderTargetLoadOp::eClear, RenderTargetStoreOp::eStore)
                .SetFramebufferInfo(m_viewport)
                .SetTag("VoxelDraw")
                .Build();
    }
}

void GeometryVoxelizer::UpdatePassResources()
{
    // voxelization pass
    {
        std::vector<ShaderResourceBinding> set0bindings;
        std::vector<ShaderResourceBinding> set1bindings;
        std::vector<ShaderResourceBinding> set2bindings;
        // set-0 bindings
        ADD_SHADER_BINDING_SINGLE(set0bindings, 0, ShaderResourceType::eStorageBuffer,
                                  m_scene->GetNodesDataSSBO());
        ADD_SHADER_BINDING_SINGLE(
            set0bindings, 1, ShaderResourceType::eUniformBuffer,
            m_gfxPasses.voxelization.shaderProgram->GetUniformBufferHandle("uVoxelConfig"));
        ADD_SHADER_BINDING_SINGLE(set0bindings, 2, ShaderResourceType::eStorageBuffer,
                                  m_scene->GetMaterialsDataSSBO());

        // set-1 bindings
        ADD_SHADER_BINDING_SINGLE(set1bindings, 0, ShaderResourceType::eImage,
                                  m_voxelTextures.albedo);
        ADD_SHADER_BINDING_SINGLE(set1bindings, 1, ShaderResourceType::eImage,
                                  m_voxelTextures.normal);
        ADD_SHADER_BINDING_SINGLE(set1bindings, 2, ShaderResourceType::eImage,
                                  m_voxelTextures.emissive);
        ADD_SHADER_BINDING_SINGLE(set1bindings, 3, ShaderResourceType::eImage,
                                  m_voxelTextures.staticFlag);
        // set-2 bindings: texture array
        ADD_SHADER_BINDING_TEXTURE_ARRAY(set2bindings, 0, ShaderResourceType::eSamplerWithTexture,
                                         m_colorSampler, m_scene->GetSceneTextures())

        rc::GraphicsPassResourceUpdater updater(m_renderDevice, &m_gfxPasses.voxelization);
        updater.SetShaderResourceBinding(0, set0bindings)
            .SetShaderResourceBinding(1, set1bindings)
            .SetShaderResourceBinding(2, set2bindings)
            .Update();
    }
    // voxel draw pass
    {
        std::vector<ShaderResourceBinding> set0bindings;
        ADD_SHADER_BINDING_SINGLE(set0bindings, 0, ShaderResourceType::eImage,
                                  m_voxelTextures.albedoProxy);
        ADD_SHADER_BINDING_SINGLE(
            set0bindings, 1, ShaderResourceType::eUniformBuffer,
            m_gfxPasses.voxelDraw.shaderProgram->GetUniformBufferHandle("uVoxelInfo"));

        rc::GraphicsPassResourceUpdater updater(m_renderDevice, &m_gfxPasses.voxelDraw);
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
            dynamic_cast<VoxelizationSP*>(m_gfxPasses.voxelization.shaderProgram);
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
            dynamic_cast<VoxelDrawSP*>(m_gfxPasses.voxelDraw.shaderProgram);
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