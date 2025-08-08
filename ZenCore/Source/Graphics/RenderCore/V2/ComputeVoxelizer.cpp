#include "Graphics/RenderCore/V2/Renderer/ComputeVoxelizer.h"
#include "AssetLib/FastGLTFLoader.h"
#include "Graphics/RenderCore/V2/RenderObject.h"
#include "Graphics/RenderCore/V2/RenderScene.h"
#include "Graphics/RenderCore/V2/ShaderProgram.h"
#include "Graphics/RenderCore/V2/TextureManager.h"
#include "Graphics/Val/CommandBuffer.h"
#include "Platform/ConfigLoader.h"
#include "SceneGraph/Scene.h"
#include "SceneGraph/Camera.h"

using namespace zen::rhi;

namespace zen::rc
{
void ComputeVoxelizer::Init()
{
    m_voxelTexResolution = 256;
    m_voxelTexFormat     = DataFormat::eR8G8B8A8UNORM;
    m_voxelCount         = m_voxelTexResolution * m_voxelTexResolution * m_voxelTexResolution;

    m_config.drawColorChannels = Vec4(1.0f);


    // std::vector<SimpleVertex> vertices;
    // vertices.resize(m_config.voxelCount);

    // m_voxelVBO = m_renderDevice->CreateVertexBuffer(
    //     m_config.voxelCount * sizeof(Vec4), reinterpret_cast<const uint8_t*>(vertices.data()));

    PrepareTextures();

    PrepareBuffers();

    BuildGraphicsPasses();

    BuildComputePasses();

    LoadCubeModel();
}

void ComputeVoxelizer::Destroy()
{
    if (m_cube)
    {
        delete m_cube;
    }
}

void ComputeVoxelizer::LoadCubeModel()
{
    m_cube = new RenderObject(m_renderDevice,
                              platform::ConfigLoader::GetInstance().GetGLTFModelPath("Box"));
}

void ComputeVoxelizer::PrepareTextures()
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
    // {
    //     rhi::SamplerInfo samplerInfo{};
    //     samplerInfo.magFilter = rhi::SamplerFilter::eLinear;
    //     samplerInfo.magFilter = rhi::SamplerFilter::eLinear;
    //     samplerInfo.mipFilter = rhi::SamplerFilter::eLinear;
    //
    //     m_voxelSampler = m_renderDevice->CreateSampler(samplerInfo);
    // }
    //
    // // offscreen depth texture sampler
    // {
    //     SamplerInfo samplerInfo{};
    //     samplerInfo.borderColor = SamplerBorderColor::eFloatOpaqueWhite;
    //     samplerInfo.minFilter   = rhi::SamplerFilter::eLinear;
    //     samplerInfo.magFilter   = rhi::SamplerFilter::eLinear;
    //     samplerInfo.mipFilter   = rhi::SamplerFilter::eLinear;
    //     samplerInfo.repeatU     = rhi::SamplerRepeatMode::eRepeat;
    //     samplerInfo.repeatV     = rhi::SamplerRepeatMode::eRepeat;
    //     samplerInfo.repeatW     = rhi::SamplerRepeatMode::eRepeat;
    //     samplerInfo.borderColor = SamplerBorderColor::eFloatOpaqueWhite;
    //
    //     m_colorSampler = m_renderDevice->CreateSampler(samplerInfo);
    // }
}

void ComputeVoxelizer::PrepareBuffers()
{
    ComputeIndirectCommand indirectCommand{};
    indirectCommand.x               = 0;
    indirectCommand.y               = 1;
    indirectCommand.z               = 1;
    m_buffers.computeIndirectBuffer = m_renderDevice->CreateIndirectBuffer(
        sizeof(ComputeIndirectCommand), reinterpret_cast<const uint8_t*>(&indirectCommand));

    m_buffers.largeTriangleBuffer =
        m_renderDevice->CreateStorageBuffer(sizeof(LargeTriangle) * 200000, nullptr);

    m_buffers.instancePositionBuffer =
        m_renderDevice->CreateStorageBuffer(sizeof(Vec4) * 3000000, nullptr);

    m_buffers.instanceColorBuffer =
        m_renderDevice->CreateStorageBuffer(sizeof(Vec4) * 3000000, nullptr);

    DrawIndexedIndirectCommand drawIndirectCmd{};
    drawIndirectCmd.instanceCount = 1;
    drawIndirectCmd.firstInstance = 0;
    drawIndirectCmd.indexCount    = 36;
    drawIndirectCmd.firstIndex    = 0;
    drawIndirectCmd.vertexOffset  = 0;

    m_buffers.drawIndirectBuffer = m_renderDevice->CreateIndirectBuffer(
        sizeof(DrawIndexedIndirectCommand), reinterpret_cast<const uint8_t*>(&drawIndirectCmd));
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
    m_rdg = MakeUnique<RenderGraph>();
    m_rdg->Begin();
    int workgroupCount;
    // voxelization pass
    if (m_needVoxelization)
    {
        rhi::TextureHandle textures[] = {
            // m_voxelTextures.staticFlag,
            m_voxelTextures.albedo,
            // m_voxelTextures.normal,
            // m_voxelTextures.emissive
        };
        rhi::TextureSubResourceRange ranges[] = {
            // m_renderDevice->GetTextureSubResourceRange(m_voxelTextures.staticFlag),
            m_renderDevice->GetTextureSubResourceRange(m_voxelTextures.albedo),
            // m_renderDevice->GetTextureSubResourceRange(m_voxelTextures.normal),
            // m_renderDevice->GetTextureSubResourceRange(m_voxelTextures.emissive)
        };
        // reset voxel texture
        {
            auto* pass = m_rdg->AddComputePassNode(m_computePasses.resetVoxelTexture);
            pass->tag  = "reset_voxel_texture";
            m_rdg->DeclareTextureAccessForPass(pass, 1, textures, TextureUsage::eStorage, ranges,
                                               AccessMode::eReadWrite);
            workgroupCount = static_cast<int>(m_voxelTexResolution / 8);
            m_rdg->AddComputePassDispatchNode(pass, workgroupCount, workgroupCount, workgroupCount);
        }
        // reset compute indirect
        {
            auto* pass = m_rdg->AddComputePassNode(m_computePasses.resetComputeIndirect);
            pass->tag  = "reset_compute_indirect";
            m_rdg->DeclareBufferAccessForPass(pass, m_buffers.computeIndirectBuffer,
                                              BufferUsage::eStorageBuffer, AccessMode::eReadWrite);
            m_rdg->AddComputePassDispatchNode(pass, 1, 1, 1);
        }
        // voxelize small triangles
        {
            VoxelizationCompSP* shaderProgram =
                dynamic_cast<VoxelizationCompSP*>(m_computePasses.voxelization.shaderProgram);
            auto* pass = m_rdg->AddComputePassNode(m_computePasses.voxelization);
            pass->tag  = "voxelization_compute";

            m_rdg->DeclareTextureAccessForPass(pass, 1, textures, TextureUsage::eStorage, ranges,
                                               AccessMode::eReadWrite);
            m_rdg->DeclareBufferAccessForPass(pass, m_buffers.computeIndirectBuffer,
                                              BufferUsage::eStorageBuffer, AccessMode::eReadWrite);
            m_rdg->DeclareBufferAccessForPass(pass, m_buffers.largeTriangleBuffer,
                                              BufferUsage::eStorageBuffer, AccessMode::eReadWrite);

            shaderProgram->pushConstantsData.largeTriangleThreshold = 15;

            const int localSize = 32;
            for (auto* node : m_scene->GetRenderableNodes())
            {
                const int triangleCount = node->GetComponent<sg::Mesh>()->GetNumIndices() / 3;
                ASSERT(triangleCount == m_scene->GetNumIndices() / 3);
                workgroupCount = ceil(double(triangleCount) / double(localSize));

                shaderProgram->pushConstantsData.nodeIndex     = node->GetRenderableIndex();
                shaderProgram->pushConstantsData.triangleCount = triangleCount;
                m_rdg->AddComputePassSetPushConstants(pass, &shaderProgram->pushConstantsData,
                                                      sizeof(VoxelizationCompSP::PushConstantData));
                m_rdg->AddComputePassDispatchNode(pass, workgroupCount, 1, 1);
            }
        }
        // voxelize large triangles
        {
            auto* pass = m_rdg->AddComputePassNode(m_computePasses.voxelizationLargeTriangle);
            pass->tag  = "voxelization_compute_large_triangle";
            m_rdg->DeclareTextureAccessForPass(pass, 1, textures, TextureUsage::eStorage, ranges,
                                               AccessMode::eReadWrite);
            m_rdg->DeclareBufferAccessForPass(pass, m_buffers.computeIndirectBuffer,
                                              BufferUsage::eIndirectBuffer, AccessMode::eRead);
            m_rdg->DeclareBufferAccessForPass(pass, m_buffers.largeTriangleBuffer,
                                              BufferUsage::eStorageBuffer, AccessMode::eRead);
            m_rdg->AddComputePassDispatchIndirectNode(pass, m_buffers.computeIndirectBuffer, 0);
        }
        // reset draw indirect
        {
            auto* pass = m_rdg->AddComputePassNode(m_computePasses.resetDrawIndirect);
            pass->tag  = "reset_draw_indirect_compute";
            m_rdg->DeclareBufferAccessForPass(pass, m_buffers.drawIndirectBuffer,
                                              BufferUsage::eStorageBuffer, AccessMode::eReadWrite);
            m_rdg->AddComputePassDispatchNode(pass, 1, 1, 1);
        }
        // voxel pre-draw pass
        {
            auto* pass = m_rdg->AddComputePassNode(m_computePasses.voxelPreDraw);
            pass->tag  = "voxel_pre_draw_compute";
            m_rdg->DeclareTextureAccessForPass(
                pass, m_voxelTextures.albedo, TextureUsage::eStorage,
                m_renderDevice->GetTextureSubResourceRange(m_voxelTextures.albedo),
                AccessMode::eRead);
            m_rdg->DeclareBufferAccessForPass(pass, m_buffers.instancePositionBuffer,
                                              BufferUsage::eStorageBuffer, AccessMode::eReadWrite);
            m_rdg->DeclareBufferAccessForPass(pass, m_buffers.instanceColorBuffer,
                                              BufferUsage::eStorageBuffer, AccessMode::eReadWrite);
            m_rdg->DeclareBufferAccessForPass(pass, m_buffers.drawIndirectBuffer,
                                              BufferUsage::eStorageBuffer, AccessMode::eReadWrite);
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
        pass->tag = "voxel_draw2";
        m_rdg->DeclareBufferAccessForPass(pass, m_buffers.instancePositionBuffer,
                                          BufferUsage::eStorageBuffer, AccessMode::eRead);
        m_rdg->DeclareBufferAccessForPass(pass, m_buffers.instanceColorBuffer,
                                          BufferUsage::eStorageBuffer, AccessMode::eRead);
        m_rdg->DeclareBufferAccessForPass(pass, m_buffers.drawIndirectBuffer,
                                          BufferUsage::eIndirectBuffer, AccessMode::eRead);
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
    GfxPipelineStates pso{};
    pso.rasterizationState           = {};
    pso.rasterizationState.cullMode  = PolygonCullMode::eDisabled;
    pso.rasterizationState.frontFace = PolygonFrontFace::eCounterClockWise;

    pso.depthStencilState =
        GfxPipelineDepthStencilState::Create(true, true, CompareOperator::eLess);
    pso.multiSampleState = {};
    pso.colorBlendState  = GfxPipelineColorBlendState::CreateDisabled(1);
    pso.dynamicStates.push_back(DynamicState::eScissor);
    pso.dynamicStates.push_back(DynamicState::eViewPort);
    rc::GraphicsPassBuilder builder(m_renderDevice);
    m_gfxPasses.voxelDraw =
        builder.SetShaderProgramName("VoxelDrawSP2")
            .SetNumSamples(SampleCount::e1)
            .SetPipelineState(pso)
            .AddColorRenderTarget(m_viewport->GetSwapchainFormat(), TextureUsage::eColorAttachment,
                                  m_viewport->GetColorBackBuffer())
            .SetDepthStencilTarget(m_viewport->GetDepthStencilFormat(),
                                   m_viewport->GetDepthStencilBackBuffer(),
                                   RenderTargetLoadOp::eClear, RenderTargetStoreOp::eStore)
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
        std::vector<ShaderResourceBinding> set0bindings;
        ADD_SHADER_BINDING_SINGLE(set0bindings, 0, ShaderResourceType::eStorageBuffer,
                                  m_buffers.drawIndirectBuffer);
        ComputePassResourceUpdater updater(m_renderDevice, &m_computePasses.resetDrawIndirect);
        updater.SetShaderResourceBinding(0, set0bindings).Update();
    }
    // reset voxel texture
    {
        std::vector<ShaderResourceBinding> set0bindings;
        ADD_SHADER_BINDING_SINGLE(set0bindings, 0, ShaderResourceType::eImage,
                                  m_voxelTextures.albedo);
        ComputePassResourceUpdater updater(m_renderDevice, &m_computePasses.resetVoxelTexture);
        updater.SetShaderResourceBinding(0, set0bindings).Update();
    }
    // reset compute indirect
    {
        std::vector<ShaderResourceBinding> set0bindings;
        ADD_SHADER_BINDING_SINGLE(set0bindings, 0, ShaderResourceType::eStorageBuffer,
                                  m_buffers.computeIndirectBuffer);
        ComputePassResourceUpdater updater(m_renderDevice, &m_computePasses.resetComputeIndirect);
        updater.SetShaderResourceBinding(0, set0bindings).Update();
    }
    // voxelization pass
    {
        std::vector<ShaderResourceBinding> set0bindings;
        std::vector<ShaderResourceBinding> set1bindings;
        std::vector<ShaderResourceBinding> set2bindings;
        std::vector<ShaderResourceBinding> set3bindings;
        std::vector<ShaderResourceBinding> set4bindings;
        std::vector<ShaderResourceBinding> set5bindings;
        // set-0 bindings
        ADD_SHADER_BINDING_SINGLE(set0bindings, 0, ShaderResourceType::eImage,
                                  m_voxelTextures.albedo);
        // set-1 bindings
        ADD_SHADER_BINDING_SINGLE(
            set1bindings, 0, ShaderResourceType::eUniformBuffer,
            m_computePasses.voxelization.shaderProgram->GetUniformBufferHandle("uSceneInfo"));
        // set-2 bindings
        ADD_SHADER_BINDING_SINGLE(set2bindings, 0, ShaderResourceType::eStorageBuffer,
                                  m_scene->GetVertexBuffer());
        ADD_SHADER_BINDING_SINGLE(set2bindings, 1, ShaderResourceType::eStorageBuffer,
                                  m_scene->GetIndexBuffer());
        ADD_SHADER_BINDING_SINGLE(set2bindings, 2, ShaderResourceType::eStorageBuffer,
                                  m_scene->GetNodesDataSSBO());
        // set-3 bindings: texture array
        ADD_SHADER_BINDING_TEXTURE_ARRAY(set3bindings, 0, ShaderResourceType::eSamplerWithTexture,
                                         m_colorSampler, m_scene->GetSceneTextures())
        // set-4 bindings
        ADD_SHADER_BINDING_SINGLE(set4bindings, 0, ShaderResourceType::eStorageBuffer,
                                  m_buffers.computeIndirectBuffer);
        ADD_SHADER_BINDING_SINGLE(set4bindings, 1, ShaderResourceType::eStorageBuffer,
                                  m_buffers.largeTriangleBuffer);

        // set-5 bindings
        ADD_SHADER_BINDING_SINGLE(set5bindings, 0, ShaderResourceType::eStorageBuffer,
                                  m_scene->GetTriangleMapBuffer());

        ComputePassResourceUpdater updater(m_renderDevice, &m_computePasses.voxelization);
        updater.SetShaderResourceBinding(0, set0bindings)
            .SetShaderResourceBinding(1, set1bindings)
            .SetShaderResourceBinding(2, set2bindings)
            .SetShaderResourceBinding(3, set3bindings)
            .SetShaderResourceBinding(4, set4bindings)
            .SetShaderResourceBinding(5, set5bindings)
            .Update();

        // large triangles
        ComputePassResourceUpdater updater2(m_renderDevice,
                                            &m_computePasses.voxelizationLargeTriangle);
        updater2.SetShaderResourceBinding(0, set0bindings)
            .SetShaderResourceBinding(1, set1bindings)
            .SetShaderResourceBinding(2, set2bindings)
            .SetShaderResourceBinding(3, set3bindings)
            .SetShaderResourceBinding(4, set4bindings)
            .SetShaderResourceBinding(5, set5bindings)
            .Update();
    }
    // voxel pre-draw pass
    {
        std::vector<ShaderResourceBinding> set0bindings;
        std::vector<ShaderResourceBinding> set1bindings;
        std::vector<ShaderResourceBinding> set2bindings;
        std::vector<ShaderResourceBinding> set3bindings;
        std::vector<ShaderResourceBinding> set4bindings;

        // set-0 bindings
        ADD_SHADER_BINDING_SINGLE(set0bindings, 0, ShaderResourceType::eImage,
                                  m_voxelTextures.albedo);
        // set-1 bindings
        ADD_SHADER_BINDING_SINGLE(set1bindings, 0, ShaderResourceType::eStorageBuffer,
                                  m_buffers.instancePositionBuffer);
        // set-2 bindings
        ADD_SHADER_BINDING_SINGLE(set2bindings, 0, ShaderResourceType::eStorageBuffer,
                                  m_buffers.instanceColorBuffer);
        // set-3 bindings
        ADD_SHADER_BINDING_SINGLE(set3bindings, 0, ShaderResourceType::eStorageBuffer,
                                  m_buffers.drawIndirectBuffer);
        // set-4 bindings
        ADD_SHADER_BINDING_SINGLE(
            set4bindings, 0, ShaderResourceType::eUniformBuffer,
            m_computePasses.voxelPreDraw.shaderProgram->GetUniformBufferHandle("uSceneInfo"));


        ComputePassResourceUpdater updater(m_renderDevice, &m_computePasses.voxelPreDraw);
        updater.SetShaderResourceBinding(0, set0bindings)
            .SetShaderResourceBinding(1, set1bindings)
            .SetShaderResourceBinding(2, set2bindings)
            .SetShaderResourceBinding(3, set3bindings)
            .SetShaderResourceBinding(4, set4bindings)
            .Update();
    }
    // voxel draw pass
    {
        VoxelDrawSP2* shaderProgram =
            dynamic_cast<VoxelDrawSP2*>(m_gfxPasses.voxelDraw.shaderProgram);

        std::vector<ShaderResourceBinding> set0bindings;
        std::vector<ShaderResourceBinding> set1bindings;
        std::vector<ShaderResourceBinding> set2bindings;
        // set-0 bindings
        ADD_SHADER_BINDING_SINGLE(set0bindings, 0, ShaderResourceType::eUniformBuffer,
                                  shaderProgram->GetUniformBufferHandle("uTransformData"));
        // set-1 bindings
        ADD_SHADER_BINDING_SINGLE(set1bindings, 0, ShaderResourceType::eStorageBuffer,
                                  m_buffers.instancePositionBuffer);
        // set-2 bindings
        ADD_SHADER_BINDING_SINGLE(set2bindings, 0, ShaderResourceType::eStorageBuffer,
                                  m_buffers.instanceColorBuffer);
        rc::GraphicsPassResourceUpdater updater(m_renderDevice, &m_gfxPasses.voxelDraw);
        updater.SetShaderResourceBinding(0, set0bindings)
            .SetShaderResourceBinding(1, set1bindings)
            .SetShaderResourceBinding(2, set2bindings)
            .Update();
    }
}

void ComputeVoxelizer::UpdateUniformData()
{
    {
        VoxelizationCompSP* shaderProgram =
            dynamic_cast<VoxelizationCompSP*>(m_computePasses.voxelization.shaderProgram);
        shaderProgram->sceneInfo.aabbMax = Vec4(m_voxelAABB.GetMax(), 1.0f);
        shaderProgram->sceneInfo.aabbMin = Vec4(m_voxelAABB.GetMin(), 1.0f);
        shaderProgram->UpdateUniformBuffer("uSceneInfo", shaderProgram->GetSceneInfoData(), 0);
    }
    {
        VoxelizationLargeTriangleCompSP* shaderProgram =
            dynamic_cast<VoxelizationLargeTriangleCompSP*>(
                m_computePasses.voxelizationLargeTriangle.shaderProgram);
        shaderProgram->sceneInfo.aabbMax = Vec4(m_voxelAABB.GetMax(), 1.0f);
        shaderProgram->sceneInfo.aabbMin = Vec4(m_voxelAABB.GetMin(), 1.0f);
        shaderProgram->UpdateUniformBuffer("uSceneInfo", shaderProgram->GetSceneInfoData(), 0);
    }
    {
        VoxelPreDrawSP* shaderProgram =
            dynamic_cast<VoxelPreDrawSP*>(m_computePasses.voxelPreDraw.shaderProgram);
        shaderProgram->sceneInfo.aabbMax = Vec4(m_voxelAABB.GetMax(), 1.0f);
        shaderProgram->sceneInfo.aabbMin = Vec4(m_voxelAABB.GetMin(), 1.0f);
        shaderProgram->UpdateUniformBuffer("uSceneInfo", shaderProgram->GetSceneInfoData(), 0);
    }
    {
        VoxelDrawSP2* shaderProgram =
            dynamic_cast<VoxelDrawSP2*>(m_gfxPasses.voxelDraw.shaderProgram);
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

// void ComputeVoxelizer::VoxelizeStaticScene() {}
//
// void ComputeVoxelizer::VoxelizeDynamicScene() {}
} // namespace zen::rc