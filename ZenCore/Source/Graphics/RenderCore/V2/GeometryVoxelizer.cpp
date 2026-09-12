#include "Graphics/RenderCore/V2/Renderer/GeometryVoxelizer.h"
#include "Graphics/RenderCore/V2/Renderer/RendererUtils.h"

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
}

void GeometryVoxelizer::Destroy()
{
    VoxelizerBase::Destroy();
}

void GeometryVoxelizer::BuildRenderGraph()
{
    RenderGraph* pRDG = m_pRenderDevice->GetCurrentFrameRDG();
    VERIFY_EXPR(pRDG != nullptr && m_pScene != nullptr);

    const sg::AABB& sceneAABB = m_pScene->GetAABB();
    const float sceneExtent   = sceneAABB.GetMaxExtent();
    const float voxelSize     = sceneExtent / m_voxelTexResolution;

    if (BeginVoxelization(*pRDG))
    {
        RHIGfxPipelineStates pso{};
        pso.rasterizationState = {};
        pso.depthStencilState =
            RHIGfxPipelineDepthStencilState::Create(false, false, RHIDepthCompareOperator::eNever);
        pso.multiSampleState = {};
        pso.dynamicStates.Enable(RHIDynamicState::eScissor, RHIDynamicState::eViewPort);

        RDGGraphicsPassDesc desc{};
        desc.SetShaderProgramName("VoxelizationSP");
        desc.SetPipelineStates(pso);
        desc.SetRenderArea(0, 0, m_voxelTexResolution, m_voxelTexResolution);
        desc.SetPassTag("Voxelization");

        desc.BindStorageBuffer("NodeBuffer", m_pScene->GetNodesDataSSBO());
        desc.BindStorageBuffer("MaterialBuffer", m_pScene->GetMaterialsDataSSBO());
        desc.BindStorageImage("voxelAlbedo", m_voxelTextures.pAlbedo->GetDefaultView());
        BindSceneTextureArray(desc, m_pColorSampler, m_pScene->GetSceneTextures());

        const Vec3 center = sceneAABB.GetCenter();
        float halfSize    = sceneExtent / 2.0f;
        Mat4 projection   = glm::ortho(-halfSize, halfSize, -halfSize, halfSize, 0.0f, sceneExtent);
        projection[1][1] *= -1;

        VoxelizationSP::VoxelConfigData voxelConfig{};
        voxelConfig.viewProjectionMatrices[0] =
            glm::lookAt(center - Vec3(halfSize, 0.0f, 0.0f), center, Vec3(0.0f, 1.0f, 0.0f));
        voxelConfig.viewProjectionMatrices[1] =
            glm::lookAt(center - Vec3(0.0f, halfSize, 0.0f), center, Vec3(-1.0f, 0.0f, 0.0f));
        voxelConfig.viewProjectionMatrices[2] =
            glm::lookAt(center - Vec3(0.0f, 0.0f, halfSize), center, Vec3(0.0f, 1.0f, 0.0f));

        voxelConfig.worldMinPointScale = Vec4(m_pScene->GetAABB().GetMin(), 1.0f / sceneExtent);

        for (int i = 0; i < 3; ++i)
        {
            voxelConfig.viewProjectionMatrices[i] =
                projection * voxelConfig.viewProjectionMatrices[i];
            voxelConfig.viewProjectionMatricesI[i] =
                glm::inverse(voxelConfig.viewProjectionMatrices[i]);
        }

        desc.BindValue("uVoxelConfig", voxelConfig);
        desc.BindVertexBuffer(m_pScene->GetVertexBuffer());
        desc.BindIndexBuffer(m_pScene->GetIndexBuffer());

        pRDG->AddGraphicsPass(std::move(desc))
            .RecordPassCommands([draws     = SnapshotSceneDraws(*m_pScene),
                                 dimension = m_voxelTexResolution](RDGPassCmdEncoder& encoder) {
                VoxelizationSP::PushConstantsData constants{};
                constants.flagStaticVoxels = 1;
                constants.volumeDimension  = dimension;

                for (SceneMeshDraw const& draw : draws)
                {
                    constants.nodeIndex     = draw.nodeIndex;
                    constants.materialIndex = draw.materialIndex;
                    encoder.SetPushConstants(constants);
                    encoder.DrawIndexed(draw.indexCount, 1, draw.firstIndex, 0, 0);
                }
            });
    }

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

    RDGGraphicsPassDesc draw{};
    draw.SetShaderProgramName("VoxelDrawSP");
    draw.SetPipelineStates(pso);
    draw.AddColorOutput(m_pViewport->GetColorBackBuffer(), RHIRenderTargetLoadOp::eLoad);
    draw.AddDepthStencilOutput(m_pViewport->GetDepthStencilBackBuffer(),
                               RHIRenderTargetLoadOp::eClear, RHIRenderTargetStoreOp::eStore);
    draw.SetRenderArea(0, 0, m_pViewport->GetWidth(), m_pViewport->GetHeight());
    draw.SetPassTag("VoxelDraw");

    const sg::CameraUniformData* pCameraUniformData =
        reinterpret_cast<const sg::CameraUniformData*>(m_pScene->GetCameraUniformData());
    VoxelDrawSP::VoxelInfo voxelInfo{};
    uint32_t drawMipLevel = 0;
    uint32_t vDimension   = static_cast<unsigned>(m_voxelTexResolution / pow(2.0f, drawMipLevel));
    float vSize           = sceneExtent / vDimension;
    Mat4 modelMatrix =
        glm::translate(Mat4(1.0f), sceneAABB.GetMin()) * glm::scale(Mat4(1.0f), glm::vec3(vSize));
    voxelInfo.modelViewProjection = pCameraUniformData->projViewMatrix * modelMatrix;

    const std::array<Vec4, 6>& planes = m_pScene->GetCamera()->GetFrustum().GetPlanes();

    for (int i = 0; i < 6; i++)
    {
        voxelInfo.frustumPlanes[i] = planes[i];
    }

    voxelInfo.worldMinPointVoxelSize = Vec4(sceneAABB.GetMin(), voxelSize);

    draw.BindValue("uVoxelInfo", voxelInfo);
    draw.BindStorageImage("voxelRadiance", m_voxelTextures.pAlbedoView);

    const VoxelDrawSP::PushConstantsData constants{m_voxelTexResolution};

    pRDG->AddGraphicsPass(std::move(draw))
        .RecordPassCommands([constants, count = m_voxelCount](RDGPassCmdEncoder& encoder) {
            encoder.SetPushConstants(constants);
            encoder.Draw(count, 1);
        });
}

} // namespace zen::rc
