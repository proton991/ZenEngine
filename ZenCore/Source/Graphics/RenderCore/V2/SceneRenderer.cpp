#include "Graphics/RenderCore/V2/SceneRenderer.h"
#include "Graphics/RenderCore/V2/RenderConfig.h"
#include "Systems/Camera.h"
#include "AssetLib/GLTFLoader.h"

namespace zen::rc
{

SceneRenderer::SceneRenderer(RenderDevice* renderDevice, RHIViewport* viewport) :
    m_renderDevice(renderDevice), m_viewport(viewport)
{}

void SceneRenderer::Bake()
{
    PrepareTextures();

    BuildRenderPipelines();
}

void SceneRenderer::Destroy()
{
    if (m_rdg)
    {
        m_rdg->Destroy();
    }
}

void SceneRenderer::SetScene(const SceneData& sceneData)
{
    m_rebuildRDG = true;
    Clear();
    m_camera = sceneData.camera;
    m_scene  = sceneData.scene;

    // White
    m_sceneUniformData.lightPosition  = sceneData.lightPosition;
    m_sceneUniformData.lightColor     = sceneData.lightColor;
    m_sceneUniformData.lightIntensity = sceneData.lightIntensity;

    auto index = 0u;
    for (auto* node : m_scene->GetRenderableNodes())
    {
        NodeData nodeUniformData{};
        nodeUniformData.modelMatrix = node->GetComponent<sg::Transform>()->GetWorldMatrix();
        // pre-calculate normal transform matrix
        nodeUniformData.normalMatrix = glm::transpose(glm::inverse(nodeUniformData.modelMatrix));
        m_nodesData.emplace_back(nodeUniformData);
        m_nodeUniformIndex[node->GetHash()] = index;
        index++;
    }
    TransformScene();

    uint32_t vbSize = sceneData.numVertices * sizeof(gltf::Vertex);
    m_vertexBuffer  = m_renderDevice->CreateVertexBuffer(
        vbSize, reinterpret_cast<const uint8_t*>(sceneData.vertices));

    m_indexBuffer =
        m_renderDevice->CreateIndexBuffer(sceneData.numIndices * sizeof(uint32_t),
                                          reinterpret_cast<const uint8_t*>(sceneData.indices));
    LoadSceneMaterials();

    LoadSceneTextures();

    PrepareBuffers();

    m_sceneLoaded = true;
}

void SceneRenderer::LoadSceneMaterials()
{
    auto sgMaterials = m_scene->GetComponents<sg::Material>();
    m_materialUniforms.reserve(sgMaterials.size());
    for (const auto* mat : sgMaterials)
    {
        MaterialData materialData{};
        if (mat->baseColorTexture != nullptr)
        {
            materialData.bcTexIndex = mat->baseColorTexture->index;
            materialData.bcTexSet   = mat->texCoordSets.baseColor;
        }
        if (mat->metallicRoughnessTexture != nullptr)
        {
            materialData.mrTexIndex = mat->metallicRoughnessTexture->index;
            materialData.mrTexSet   = mat->texCoordSets.metallicRoughness;
        }
        if (mat->normalTexture != nullptr)
        {
            materialData.normalTexIndex = mat->normalTexture->index;
            materialData.normalTexSet   = mat->texCoordSets.normal;
        }
        if (mat->occlusionTexture != nullptr)
        {
            materialData.aoTexIndex = mat->occlusionTexture->index;
            materialData.aoTexSet   = mat->texCoordSets.occlusion;
        }
        if (mat->emissiveTexture != nullptr)
        {
            materialData.emissiveTexIndex = mat->emissiveTexture->index;
            materialData.emissiveTexSet   = mat->texCoordSets.emissive;
        }
        materialData.metallicFactor  = mat->metallicFactor;
        materialData.roughnessFactor = mat->roughnessFactor;
        materialData.emissiveFactor  = mat->emissiveFactor;
        m_materialUniforms.emplace_back(materialData);
    }
}

void SceneRenderer::LoadSceneTextures()
{
    // default base color texture
    m_defaultBaseColorTexture = m_renderDevice->RequestTexture2D("wood.png");
    // scene textures
    m_renderDevice->RegisterSceneTextures(m_scene, m_sceneTextures);
}

void SceneRenderer::DrawScene()
{
    if (m_rebuildRDG)
    {
        BuildRenderGraph();
        m_rebuildRDG = false;
    }

    m_renderDevice->UpdateBuffer(m_cameraUBO, sizeof(sys::CameraUniformData),
                                 m_camera->GetUniformData());
    m_renderDevice->UpdateBuffer(m_sceneUBO, sizeof(SceneUniformData),
                                 reinterpret_cast<const uint8_t*>(&m_sceneUniformData));

    m_renderDevice->ExecuteFrame(m_viewport, m_rdg.Get());
}

void SceneRenderer::PrepareTextures()
{
    // (World space) Positions
    {
        rhi::TextureInfo texInfo{};
        texInfo.type        = rhi::TextureType::e2D;
        texInfo.format      = DataFormat::eR16G16B16A16SFloat;
        texInfo.width       = RenderConfig::GetInstance().offScreenFbSize;
        texInfo.height      = RenderConfig::GetInstance().offScreenFbSize;
        texInfo.depth       = 1;
        texInfo.mipmaps     = 1;
        texInfo.arrayLayers = 1;
        texInfo.samples     = SampleCount::e1;
        texInfo.usageFlags.SetFlag(TextureUsageFlagBits::eColorAttachment);
        texInfo.usageFlags.SetFlag(TextureUsageFlagBits::eSampled);
        m_offscreenTextures.position = m_renderDevice->CreateTexture(texInfo, "offscreen_position");
    }
    // (World space) Normals
    {
        rhi::TextureInfo texInfo{};
        texInfo.type        = rhi::TextureType::e2D;
        texInfo.format      = DataFormat::eR16G16B16A16SFloat;
        texInfo.width       = RenderConfig::GetInstance().offScreenFbSize;
        texInfo.height      = RenderConfig::GetInstance().offScreenFbSize;
        texInfo.depth       = 1;
        texInfo.mipmaps     = 1;
        texInfo.arrayLayers = 1;
        texInfo.samples     = SampleCount::e1;
        texInfo.usageFlags.SetFlag(TextureUsageFlagBits::eColorAttachment);
        texInfo.usageFlags.SetFlag(TextureUsageFlagBits::eSampled);
        m_offscreenTextures.normal = m_renderDevice->CreateTexture(texInfo, "offscreen_normal");
    }
    // color
    {
        rhi::TextureInfo texInfo{};
        texInfo.type        = rhi::TextureType::e2D;
        texInfo.format      = DataFormat::eR8G8B8A8UNORM;
        texInfo.width       = RenderConfig::GetInstance().offScreenFbSize;
        texInfo.height      = RenderConfig::GetInstance().offScreenFbSize;
        texInfo.depth       = 1;
        texInfo.mipmaps     = 1;
        texInfo.arrayLayers = 1;
        texInfo.samples     = SampleCount::e1;
        texInfo.usageFlags.SetFlag(TextureUsageFlagBits::eColorAttachment);
        texInfo.usageFlags.SetFlag(TextureUsageFlagBits::eSampled);
        m_offscreenTextures.albedo = m_renderDevice->CreateTexture(texInfo, "offscreen_albedo");
    }
    // depth
    {

        rhi::TextureInfo texInfo{};
        texInfo.type        = rhi::TextureType::e2D;
        texInfo.format      = m_viewport->GetDepthStencilFormat();
        texInfo.type        = rhi::TextureType::e2D;
        texInfo.width       = RenderConfig::GetInstance().offScreenFbSize;
        texInfo.height      = RenderConfig::GetInstance().offScreenFbSize;
        texInfo.depth       = 1;
        texInfo.arrayLayers = 1;
        texInfo.mipmaps     = 1;
        texInfo.usageFlags.SetFlag(rhi::TextureUsageFlagBits::eDepthStencilAttachment);
        texInfo.usageFlags.SetFlag(rhi::TextureUsageFlagBits::eSampled);
        m_offscreenTextures.depth = m_renderDevice->CreateTexture(texInfo, "offscreen_depth");
    }
    // offscreen color texture sampler
    {
        SamplerInfo samplerInfo{};
        samplerInfo.borderColor = SamplerBorderColor::eFloatOpaqueWhite;
        m_depthSampler          = m_renderDevice->CreateSampler(samplerInfo);
    }
    // offscreen depth texture sampler
    {
        SamplerInfo samplerInfo{};
        samplerInfo.borderColor = SamplerBorderColor::eFloatOpaqueWhite;
        samplerInfo.minFilter   = rhi::SamplerFilter::eLinear;
        samplerInfo.magFilter   = rhi::SamplerFilter::eLinear;
        samplerInfo.mipFilter   = rhi::SamplerFilter::eLinear;
        samplerInfo.repeatU     = rhi::SamplerRepeatMode::eRepeat;
        samplerInfo.repeatV     = rhi::SamplerRepeatMode::eRepeat;
        samplerInfo.repeatW     = rhi::SamplerRepeatMode::eRepeat;
        samplerInfo.borderColor = SamplerBorderColor::eFloatOpaqueWhite;
        m_colorSampler          = m_renderDevice->CreateSampler(samplerInfo);
    }
}

void SceneRenderer::PrepareBuffers()
{
    {
        // camera ubo
        m_cameraUBO = m_renderDevice->CreateUniformBuffer(sizeof(sys::CameraUniformData),
                                                          m_camera->GetUniformData());
    }

    {
        // prepare node ssbo
        auto ssboSize = sizeof(NodeData) * m_nodesData.size();
        m_nodeSSBO    = m_renderDevice->CreateStorageBuffer(
            ssboSize, reinterpret_cast<const uint8_t*>(m_nodesData.data()));
        m_renderDevice->UpdateBuffer(m_nodeSSBO, ssboSize,
                                     reinterpret_cast<const uint8_t*>(m_nodesData.data()));
    }

    {
        // prepare material ssbo
        const auto ssboSize = sizeof(MaterialData) * m_materialUniforms.size();
        m_materialSSBO      = m_renderDevice->CreateStorageBuffer(
            ssboSize, reinterpret_cast<const uint8_t*>(m_materialUniforms.data()));
    }

    {
        // scene ubo
        m_sceneUniformData.viewPos = Vec4(m_camera->GetPos(), 1.0f);

        const auto uboSize = sizeof(SceneUniformData);
        m_sceneUBO         = m_renderDevice->CreateUniformBuffer(
            uboSize, reinterpret_cast<const uint8_t*>(&m_sceneUniformData));
    }
}

void SceneRenderer::BuildRenderPipelines()
{
    GfxPipelineStates pso{};
    pso.primitiveType      = DrawPrimitiveType::eTriangleList;
    pso.rasterizationState = {};
    pso.depthStencilState =
        GfxPipelineDepthStencilState::Create(true, true, CompareOperator::eLessOrEqual);
    pso.depthStencilState.frontOp.fail      = rhi::StencilOperation::eKeep;
    pso.depthStencilState.frontOp.pass      = rhi::StencilOperation::eKeep;
    pso.depthStencilState.frontOp.depthFail = rhi::StencilOperation::eKeep;
    pso.depthStencilState.frontOp.compare   = rhi::CompareOperator::eNever;
    pso.depthStencilState.backOp.fail       = rhi::StencilOperation::eKeep;
    pso.depthStencilState.backOp.pass       = rhi::StencilOperation::eKeep;
    pso.depthStencilState.backOp.depthFail  = rhi::StencilOperation::eKeep;
    pso.multiSampleState                    = {};

    // offscreen
    {
        pso.colorBlendState = GfxPipelineColorBlendState::CreateDisabled(3);
        pso.dynamicStates.push_back(DynamicState::eScissor);
        pso.dynamicStates.push_back(DynamicState::eViewPort);
        std::vector<ShaderResourceBinding> bufferBindings;
        {
            ShaderResourceBinding binding0{};
            binding0.binding = 0;
            binding0.type    = ShaderResourceType::eUniformBuffer;
            binding0.handles.push_back(m_cameraUBO);
            bufferBindings.emplace_back(std::move(binding0));

            ShaderResourceBinding binding1{};
            binding1.binding = 1;
            binding1.type    = ShaderResourceType::eStorageBuffer;
            binding1.handles.push_back(m_nodeSSBO);
            bufferBindings.emplace_back(std::move(binding1));

            ShaderResourceBinding binding2{};
            binding2.binding = 2;
            binding2.type    = ShaderResourceType::eStorageBuffer;
            binding2.handles.push_back(m_materialSSBO);
            bufferBindings.emplace_back(std::move(binding2));
        }

        std::vector<ShaderResourceBinding> textureBindings;
        {
            ShaderResourceBinding binding{};
            binding.binding = 0;
            binding.type    = ShaderResourceType::eSamplerWithTexture;
            for (TextureHandle& textureHandle : m_sceneTextures)
            {
                binding.handles.push_back(m_colorSampler);
                binding.handles.push_back(textureHandle);
            }
            textureBindings.emplace_back(std::move(binding));
        }

        rc::RenderPipelineBuilder builder(m_renderDevice);
        m_mainRPs.offscreen =
            builder.SetVertexShader("SceneRenderer/offscreen.vert.spv")
                .SetFragmentShader("SceneRenderer/offscreen.frag.spv")
                .SetNumSamples(SampleCount::e1)
                // (World space) Positions
                .AddColorRenderTarget(DataFormat::eR16G16B16A16SFloat,
                                      TextureUsage::eColorAttachment)
                // (World space) Normals
                .AddColorRenderTarget(DataFormat::eR16G16B16A16SFloat,
                                      TextureUsage::eColorAttachment)
                // Albedo (color)
                .AddColorRenderTarget(DataFormat::eR8G8B8A8UNORM, TextureUsage::eColorAttachment)
                .SetDepthStencilTarget(m_viewport->GetDepthStencilFormat(),
                                       rhi::RenderTargetLoadOp::eClear,
                                       rhi::RenderTargetStoreOp::eStore)
                .SetShaderResourceBinding(0, bufferBindings)
                .SetShaderResourceBinding(1, textureBindings)
                .SetPipelineState(pso)
                .SetTag("OffScreen")
                .Build();
    }

    // scene lighting
    {
        pso.dynamicStates.clear();
        pso.dynamicStates.push_back(DynamicState::eScissor);
        pso.dynamicStates.push_back(DynamicState::eViewPort);

        pso.colorBlendState = GfxPipelineColorBlendState::CreateDisabled();

        //        pso.rasterizationState.cullMode = rhi::PolygonCullMode::eFront;
        std::vector<ShaderResourceBinding> uboBindings;
        {
            ShaderResourceBinding binding0{};
            binding0.binding = 0;
            binding0.type    = ShaderResourceType::eUniformBuffer;
            binding0.handles.push_back(m_sceneUBO);
            uboBindings.emplace_back(std::move(binding0));
        }
        std::vector<ShaderResourceBinding> textureBindings;
        {
            ShaderResourceBinding binding0{};
            binding0.binding = 0;
            binding0.type    = ShaderResourceType::eSamplerWithTexture;
            binding0.handles.push_back(m_colorSampler);
            binding0.handles.push_back(m_offscreenTextures.position);
            textureBindings.emplace_back(std::move(binding0));

            ShaderResourceBinding binding1{};
            binding1.binding = 1;
            binding1.type    = ShaderResourceType::eSamplerWithTexture;
            binding1.handles.push_back(m_colorSampler);
            binding1.handles.push_back(m_offscreenTextures.normal);
            textureBindings.emplace_back(std::move(binding1));

            ShaderResourceBinding binding2{};
            binding2.binding = 2;
            binding2.type    = ShaderResourceType::eSamplerWithTexture;
            binding2.handles.push_back(m_colorSampler);
            binding2.handles.push_back(m_offscreenTextures.albedo);
            textureBindings.emplace_back(std::move(binding2));
        }
        rc::RenderPipelineBuilder builder(m_renderDevice);
        m_mainRPs.sceneLighting = builder.SetVertexShader("SceneRenderer/deferred.vert.spv")
                                      .SetFragmentShader("SceneRenderer/deferred.frag.spv")
                                      .SetNumSamples(SampleCount::e1)
                                      .AddColorRenderTarget(m_viewport->GetSwapchainFormat(),
                                                            TextureUsage::eColorAttachment)
                                      .SetDepthStencilTarget(m_viewport->GetDepthStencilFormat(),
                                                             rhi::RenderTargetLoadOp::eClear,
                                                             rhi::RenderTargetStoreOp::eStore)
                                      .SetShaderResourceBinding(0, textureBindings)
                                      .SetShaderResourceBinding(1, uboBindings)
                                      .SetPipelineState(pso)
                                      .SetTag("SceneLighting")
                                      .Build();
    }
}

void SceneRenderer::BuildRenderGraph()
{
    m_rdg = MakeUnique<RenderGraph>();
    m_rdg->Begin();
    // offscreen pass
    {
        const uint32_t cFbSize = RenderConfig::GetInstance().offScreenFbSize;
        std::vector<RenderPassClearValue> clearValues(4);
        clearValues[0].color   = {0.0f, 0.0f, 0.0f, 0.0f};
        clearValues[1].color   = {0.0f, 0.0f, 0.0f, 0.0f};
        clearValues[2].color   = {0.0f, 0.0f, 0.0f, 0.0f};
        clearValues[3].depth   = 1.0f;
        clearValues[3].stencil = 0;

        Rect2 area;
        area.minX = 0;
        area.minY = 0;
        area.maxX = static_cast<int>(cFbSize);
        area.maxY = static_cast<int>(cFbSize);

        Rect2<float> vp;
        vp.minX = 0.0f;
        vp.minY = 0.0f;
        vp.maxX = static_cast<float>(cFbSize);
        vp.maxY = static_cast<float>(cFbSize);

        std::vector<TextureHandle> mrts(4);
        mrts[0] = m_offscreenTextures.position;
        mrts[1] = m_offscreenTextures.normal;
        mrts[2] = m_offscreenTextures.albedo;
        mrts[3] = m_offscreenTextures.depth;

        FramebufferInfo fbInfo{};
        fbInfo.width           = cFbSize;
        fbInfo.height          = cFbSize;
        fbInfo.numRenderTarget = mrts.size();
        fbInfo.renderTargets   = mrts.data();
        FramebufferHandle framebuffer =
            m_viewport->GetCompatibleFramebuffer(m_mainRPs.offscreen.renderPass, &fbInfo);
        auto* pass = m_rdg->AddGraphicsPassNode(m_mainRPs.offscreen.renderPass, framebuffer, area,
                                                clearValues, true);
        m_rdg->DeclareTextureAccessForPass(
            pass, m_offscreenTextures.position, TextureUsage::eColorAttachment,
            TextureSubResourceRange::Color(), rc::RDGAccessType::eReadWrite);
        m_rdg->DeclareTextureAccessForPass(
            pass, m_offscreenTextures.normal, TextureUsage::eColorAttachment,
            TextureSubResourceRange::Color(), rc::RDGAccessType::eReadWrite);
        m_rdg->DeclareTextureAccessForPass(
            pass, m_offscreenTextures.albedo, TextureUsage::eColorAttachment,
            TextureSubResourceRange::Color(), rc::RDGAccessType::eReadWrite);
        m_rdg->DeclareTextureAccessForPass(
            pass, m_offscreenTextures.depth, TextureUsage::eDepthStencilAttachment,
            m_renderDevice->GetTextureSubResourceRange(m_offscreenTextures.depth),
            rc::RDGAccessType::eReadWrite);

        AddMeshDrawNodes(pass, area, vp);
    }
    // scene lighting pass
    {
        std::vector<RenderPassClearValue> clearValues(2);
        clearValues[0].color   = {0.8f, 0.8f, 0.8f, 1.0f};
        clearValues[1].depth   = 1.0f;
        clearValues[1].stencil = 0;

        Rect2 area;
        area.minX = 0;
        area.minY = 0;
        area.maxX = static_cast<int>(m_viewport->GetWidth());
        area.maxY = static_cast<int>(m_viewport->GetHeight());

        Rect2<float> vp;
        vp.minX = 0.0f;
        vp.minY = 0.0f;
        vp.maxX = static_cast<float>(m_viewport->GetWidth());
        vp.maxY = static_cast<float>(m_viewport->GetHeight());

        FramebufferHandle framebuffer =
            m_viewport->GetCompatibleFramebuffer(m_mainRPs.sceneLighting.renderPass);

        auto* pass = m_rdg->AddGraphicsPassNode(m_mainRPs.sceneLighting.renderPass, framebuffer,
                                                area, clearValues, true);
        m_rdg->DeclareTextureAccessForPass(pass, m_offscreenTextures.position,
                                           TextureUsage::eSampled, TextureSubResourceRange::Color(),
                                           rc::RDGAccessType::eRead);
        m_rdg->DeclareTextureAccessForPass(pass, m_offscreenTextures.normal, TextureUsage::eSampled,
                                           TextureSubResourceRange::Color(),
                                           rc::RDGAccessType::eRead);
        m_rdg->DeclareTextureAccessForPass(pass, m_offscreenTextures.albedo, TextureUsage::eSampled,
                                           TextureSubResourceRange::Color(),
                                           rc::RDGAccessType::eRead);
        m_rdg->DeclareTextureAccessForPass(
            pass, m_offscreenTextures.depth, TextureUsage::eSampled,
            m_renderDevice->GetTextureSubResourceRange(m_offscreenTextures.depth),
            rc::RDGAccessType::eRead);
        // Final composition
        // This is done by simply drawing a full screen quad
        // The fragment shader then combines the deferred attachments into the final image
        m_rdg->AddGraphicsPassBindPipelineNode(pass, m_mainRPs.sceneLighting.pipeline,
                                               PipelineType::eGraphics);
        m_rdg->AddGraphicsPassSetScissorNode(pass, area);
        m_rdg->AddGraphicsPassSetViewportNode(pass, vp);
        m_rdg->AddGraphicsPassDrawNode(pass, 3, 1);
    }
    m_rdg->End();
}

void SceneRenderer::AddMeshDrawNodes(RDGPassNode* pass,
                                     const Rect2<int>& area,
                                     const Rect2<float>& viewport)
{
    m_rdg->AddGraphicsPassSetScissorNode(pass, area);
    m_rdg->AddGraphicsPassBindPipelineNode(pass, m_mainRPs.offscreen.pipeline,
                                           PipelineType::eGraphics);
    m_rdg->AddGraphicsPassBindVertexBufferNode(pass, m_vertexBuffer, {0});
    m_rdg->AddGraphicsPassBindIndexBufferNode(pass, m_indexBuffer, DataFormat::eR32UInt);
    m_rdg->AddGraphicsPassSetViewportNode(pass, viewport);
    for (auto* node : m_scene->GetRenderableNodes())
    {
        m_pushConstantsData.nodeIndex = m_nodeUniformIndex[node->GetHash()];
        for (auto* subMesh : node->GetComponent<sg::Mesh>()->GetSubMeshes())
        {
            m_pushConstantsData.materialIndex = subMesh->GetMaterial()->index;
            m_rdg->AddGraphicsPassSetPushConstants(pass, &m_pushConstantsData,
                                                   sizeof(PushConstantsData));
            m_rdg->AddGraphicsPassDrawIndexedNode(pass, subMesh->GetIndexCount(), 1,
                                                  subMesh->GetFirstIndex(), 0, 0);
        }
    }
}

void SceneRenderer::Clear()
{
    if (m_sceneLoaded)
    {
        // clear currently cached scene data
        m_renderDevice->DestroyBuffer(m_vertexBuffer);
        m_renderDevice->DestroyBuffer(m_indexBuffer);
        m_renderDevice->DestroyBuffer(m_nodeSSBO);
        m_renderDevice->DestroyBuffer(m_materialSSBO);

        m_pushConstantsData = {};
        m_nodesData.clear();
        m_materialUniforms.clear();
    }
}

void SceneRenderer::TransformScene()
{
    // Center and scale model
    Vec3 center(0.0f);
    Vec3 extents = m_scene->GetAABB().GetMax() - m_scene->GetAABB().GetMin();
    Vec3 scaleFactors(1.0f);
    scaleFactors.x = glm::abs(extents.x) < glm::epsilon<float>() ? 1.0f : 1.0f / extents.x;
    scaleFactors.y = glm::abs(extents.y) < glm::epsilon<float>() ? 1.0f : 1.0f / extents.y;
    scaleFactors.z = glm::abs(extents.z) < glm::epsilon<float>() ? 1.0f : 1.0f / extents.z;

    auto scaleFactorMax = std::max(scaleFactors.x, std::max(scaleFactors.y, scaleFactors.z));
    Mat4 scaleMat       = glm::scale(Mat4(1.0f), Vec3(scaleFactorMax));
    Mat4 translateMat   = glm::translate(Mat4(1.0f), center - m_scene->GetAABB().GetCenter());

    Mat4 transformMat = scaleMat * translateMat;

    for (auto& nodeData : m_nodesData)
    {
        nodeData.modelMatrix = transformMat * nodeData.modelMatrix;
    }

    m_scene->GetAABB().Transform(transformMat);
}
} // namespace zen::rc
