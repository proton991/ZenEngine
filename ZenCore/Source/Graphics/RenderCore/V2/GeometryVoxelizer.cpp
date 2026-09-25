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
    m_voxelTexResolution = platform::ConfigLoader::GetInstance().GetVoxelResolution();
    m_voxelTexFormat     = DataFormat::eR8G8B8A8UNORM;
    m_voxelCount         = m_voxelTexResolution * m_voxelTexResolution * m_voxelTexResolution;
}

void GeometryVoxelizer::Destroy()
{
    VoxelizerBase::Destroy();
}

void GeometryVoxelizer::BuildVoxelizationGraph()
{
    RenderGraph* pRDG = m_pRenderDevice->GetCurrentFrameRDG();
    VERIFY_EXPR(pRDG != nullptr && m_pScene != nullptr);

    if (BeginVoxelization(*pRDG))
    {
        if (m_pScene->GetVoxelTriangleCount() != 0)
        {
            RHIGfxPipelineStates pso{};
            pso.rasterizationState          = {};
            pso.rasterizationState.cullMode = RHIPolygonCullMode::eDisabled;
            pso.depthStencilState           = RHIGfxPipelineDepthStencilState::Create(
                false, false, RHIDepthCompareOperator::eNever);
            pso.multiSampleState = {};
            pso.dynamicStates.Enable(RHIDynamicState::eScissor, RHIDynamicState::eViewPort);

            RDGGraphicsPassDesc desc{};
            desc.SetShaderProgramName(UsesAveragedReflectance() ? "VoxelizationAveragedSP" :
                                                                  "VoxelizationSP");
            desc.SetPipelineStates(pso);
            desc.SetRenderArea(0, 0, m_voxelTexResolution, m_voxelTexResolution);
            desc.SetPassTag("Voxelization");

            BindVoxelScene(desc);
            BindReflectanceSums(desc);
            desc.BindStorageImage("voxelOwner", m_voxelTextures.pOwner->GetDefaultView());
            desc.BindVertexBuffer(m_pScene->GetVertexBuffer());
            desc.BindIndexBuffer(m_pScene->GetIndexBuffer());

            pRDG->AddGraphicsPass(std::move(desc))
                .RecordPassCommands([draws     = SnapshotSceneDraws(*m_pScene, m_classMask),
                                     dimension = m_voxelTexResolution](RDGPassCmdEncoder& encoder) {
                    VoxelizationSP::PushConstantsData constants{};
                    constants.firstTriangle   = 0;
                    constants.volumeDimension = dimension;

                    for (SceneMeshDraw const& draw : draws)
                    {
                        constants.nodeIndex     = draw.nodeIndex;
                        constants.materialIndex = draw.materialIndex;
                        constants.firstTriangle = draw.firstTriangle;
                        encoder.SetPushConstants(constants);
                        encoder.DrawIndexed(draw.indexCount, 1, draw.firstIndex, 0, 0);
                    }
                });
        }
        ResolveSurface(RDGQueuePreference::eDefault);
        BuildCompaction(RDGQueuePreference::eDefault);
    }
}

void GeometryVoxelizer::BuildVisualizationGraph()
{
    RenderGraph* pRDG        = m_pRenderDevice->GetCurrentFrameRDG();
    const sg::AABB sceneAABB = GetVoxelBounds();
    const float sceneExtent  = sceneAABB.GetMaxExtent();
    const float voxelSize    = GetVoxelSize();

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
