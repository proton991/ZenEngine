#include "SceneRendererDemo.h"
#include "AssetLib/Types.h"
#include "Graphics/RenderCore/V2/Renderer/RendererServer.h"
#include "Graphics/RenderCore/V2/Renderer/VoxelizerBase.h"
#include "Graphics/RenderCore/V2/Renderer/VoxelGIRenderer.h"
#include "Graphics/Shared/VoxelGI.h"
#include "Graphics/RenderCore/V2/RenderConfig.h"
#include "Graphics/RenderCore/V2/ShaderProgram.h"
#include "Graphics/RenderCore/V2/Renderer/RendererUtils.h"
#include <fstream>
#include <iomanip>
#include <limits>

namespace zen
{
namespace
{
struct VoxelCaptureBuffer
{
    const char* suffix;
    RHIBuffer* source;
    RHIBuffer* readback{nullptr};
    uint32_t size{0};
    RHIBuffer* copy{nullptr};
};

bool WriteVoxelCaptureBuffer(const std::string& prefix, const VoxelCaptureBuffer& buffer)
{
    std::ofstream output(prefix + "." + buffer.suffix + ".bin", std::ios::binary);
    bool succeeded = output.good();
    if (succeeded && buffer.size > 0)
    {
        const uint8_t* data = buffer.readback->Map();
        succeeded           = data != nullptr;
        if (succeeded)
        {
            output.write(reinterpret_cast<const char*>(data), buffer.size);
            buffer.readback->Unmap();
        }
    }
    output.flush();
    return succeeded && output.good();
}
} // namespace

// Opt-in diagnostic path. All production work completes before CPU mapping; capture
// resources and scene inputs stay alive through the diagnostic graph's completion.
bool SceneRendererDemo::CaptureVoxelVolume(const std::string& path, uint32_t classMask)
{
    rc::VoxelizerBase* output = m_renderDevice->GetRendererServer()->RequestVoxelizer(classMask);
    return output != nullptr && CaptureVoxelOutput(path, *output);
}

bool SceneRendererDemo::CaptureVoxelOutput(const std::string& path, rc::VoxelizerBase& output)
{
    m_renderDevice->FlushRHIThread();
    m_renderDevice->WaitForIdle();
    rc::VoxelizerBase* voxelizer      = &output;
    const rc::VoxelTextures& textures = voxelizer->GetVoxelTextures();
    const uint32_t resolution         = voxelizer->GetVoxelTexResolution();
    const uint64_t bytes              = uint64_t(resolution) * resolution * resolution * 32;
    bool succeeded = !m_renderDevice->AreSubmissionsBlocked() && voxelizer->IsReady() &&
        voxelizer->ProducesRadianceInputs() && voxelizer->GetGeometryRevision() != 0 &&
        bytes <= std::numeric_limits<uint32_t>::max() &&
        bytes <= m_renderDevice->GetGPUInfo().maxStorageBufferRange;
    RHIBuffer* packed = nullptr;
    RHIBuffer* reflectancePacked = nullptr;
    const bool averaged          = voxelizer->UsesAveragedReflectance();
    rc::VoxelGIRenderer* gi      = m_renderDevice->GetRendererServer()->RequestVoxelGI();
    const uint32_t hasRadiance   = output.GetClassMask() == GI_ALL && gi->IsInitialized() &&
            m_renderDevice->GetRendererServer()->GetRenderOption() == rc::RenderOption::eVoxelGI ?
        1u :
        0u;
    if (succeeded)
    {
        RHIBufferCreateInfo info;
        info.size         = static_cast<uint32_t>(bytes);
        info.allocateType = RHIBufferAllocateType::eGPU;
        info.usageFlags.SetFlags(RHIBufferUsageFlagBits::eStorageBuffer,
                                 RHIBufferUsageFlagBits::eTransferSrcBuffer);
        info.tag  = "voxel_capture_packed";
        packed    = m_renderDevice->CreateBuffer(info);
        succeeded = packed != nullptr;
        if (succeeded && averaged)
        {
            info.tag          = "voxel_reflectance_capture";
            reflectancePacked = m_renderDevice->CreateBuffer(info);
            succeeded         = reflectancePacked != nullptr;
        }
    }
    VoxelCaptureBuffer buffers[] = {{"voxels", packed},
                                    {"reflectance", reflectancePacked},
                                    {"vertices", m_renderScene->GetVertexBuffer()},
                                    {"indices", m_renderScene->GetIndexBuffer()},
                                    {"nodes", m_renderScene->GetNodesDataSSBO()},
                                    {"triangles", m_renderScene->GetVoxelTriangleBuffer()},
                                    {"materials", m_renderScene->GetMaterialsDataSSBO()},
                                    {"occupied-list", output.GetOccupiedList()},
                                    {"grid-to-list", output.GetGridToList()},
                                    {"occupied-count", output.GetOccupiedCount()}};
    for (VoxelCaptureBuffer& buffer : buffers)
    {
        if (succeeded && buffer.source != nullptr)
        {
            buffer.size = buffer.source->GetRequiredSize();
            RHIBufferCreateInfo info;
            info.size         = buffer.size;
            info.allocateType = RHIBufferAllocateType::eCPURead;
            info.usageFlags.SetFlag(RHIBufferUsageFlagBits::eTransferDstBuffer);
            info.tag        = "voxel_capture_readback";
            buffer.readback = m_renderDevice->CreateBuffer(info);
            succeeded       = buffer.readback != nullptr;
            if (succeeded && buffer.source != packed && buffer.source != reflectancePacked)
            {
                // Scene storage inputs lack transfer-source usage. Copy their words
                // in compute without changing production allocation flags.
                info.allocateType = RHIBufferAllocateType::eGPU;
                info.usageFlags   = 0;
                info.usageFlags.SetFlags(RHIBufferUsageFlagBits::eStorageBuffer,
                                         RHIBufferUsageFlagBits::eTransferSrcBuffer);
                buffer.copy = m_renderDevice->CreateBuffer(info);
                succeeded   = buffer.copy != nullptr;
            }
        }
    }
    if (succeeded)
    {
        rc::RenderGraph graph("CaptureVoxelVolume");
        succeeded = graph.Begin();
        if (succeeded)
        {
            rc::RDGComputePassDesc pack;
            pack.SetShaderProgramName("CaptureVoxelVolumeSP");
            pack.SetPassTag("PackVoxelVolume");
            pack.BindStorageImage("voxelOwner", textures.pOwner->GetDefaultView());
            pack.BindSampledTexture("voxelAlbedo", voxelizer->GetVoxelSampler(),
                                    textures.pAlbedoView);
            pack.BindSampledTexture("voxelNormal", voxelizer->GetVoxelSampler(),
                                    textures.pNormalView);
            pack.BindSampledTexture("voxelEmissive", voxelizer->GetVoxelSampler(),
                                    textures.pEmissiveView);
            pack.BindStorageBuffer("VolumeCapture", packed, rc::RDGContentGuarantee::eFullWrite);
            const glm::uvec3 groups =
                rc::GetVoxelVolumeDispatchGroups(resolution, m_renderDevice->GetGPUInfo());
            graph.AddComputePass(std::move(pack))
                .RecordPassCommands([groups](rc::RDGPassCmdEncoder& encoder) {
                    encoder.Dispatch(groups.x, groups.y, groups.z);
                });
            if (averaged)
            {
                rc::RDGComputePassDesc reflectance;
                reflectance.SetShaderProgramName("CaptureVoxelReflectanceSP");
                reflectance.BindStorageBuffer("ReflectanceSums", voxelizer->GetReflectanceSums());
                reflectance.BindSampledTexture("voxelReflectance", voxelizer->GetVoxelSampler(),
                                               textures.pReflectance->GetDefaultView());
                reflectance.BindSampledTexture("voxelRadiance", voxelizer->GetVoxelSampler(),
                                               hasRadiance != 0 ?
                                                   gi->GetRadianceTexture()->GetDefaultView() :
                                                   textures.pEmissiveView);
                reflectance.BindStorageBuffer("ReflectanceCapture", reflectancePacked,
                                              rc::RDGContentGuarantee::eFullWrite);
                graph.AddComputePass(std::move(reflectance))
                    .RecordPassCommands([groups, hasRadiance](rc::RDGPassCmdEncoder& encoder) {
                        encoder.SetPushConstants(hasRadiance);
                        encoder.Dispatch(groups.x, groups.y, groups.z);
                    });
            }
            for (const VoxelCaptureBuffer& buffer : buffers)
            {
                if (buffer.source != nullptr)
                {
                    if (buffer.copy != nullptr)
                    {
                        rc::RDGComputePassDesc copy;
                        copy.SetShaderProgramName("CaptureVoxelBufferSP");
                        copy.SetPassTag("CopyVoxelSceneInput");
                        copy.BindStorageBuffer("CaptureSource", buffer.source);
                        copy.BindStorageBuffer("CaptureCopy", buffer.copy,
                                               rc::RDGContentGuarantee::eFullWrite);
                        const uint32_t count = std::min(65535u, (buffer.size / 4 + 63) / 64);
                        graph.AddComputePass(std::move(copy))
                            .RecordPassCommands([count](rc::RDGPassCmdEncoder& encoder) {
                                encoder.Dispatch(count, 1, 1);
                            });
                    }
                    graph.AddTransferPass("ReadVoxelCapture")
                        .CopyBuffer(buffer.copy != nullptr ? buffer.copy : buffer.source,
                                    buffer.readback, {0, 0, buffer.size})
                        .NeverCull();
                }
            }
            succeeded = graph.End() && m_renderDevice->ExecuteRenderGraph(graph);
        }
        m_renderDevice->FlushRHIThread();
        m_renderDevice->WaitForIdle();
        succeeded = succeeded && !m_renderDevice->AreSubmissionsBlocked();
    }
    if (succeeded)
    {
        for (const VoxelCaptureBuffer& buffer : buffers)
        {
            succeeded = WriteVoxelCaptureBuffer(path, buffer) && succeeded;
        }
        std::ofstream metadata(path + ".json");
        const Vec3 origin = voxelizer->GetSceneMinPoint();
        metadata << std::setprecision(std::numeric_limits<float>::max_digits10)
                 << "{\"version\":1,\"resolution\":" << resolution << ",\"grid_min\":[" << origin.x
                 << ',' << origin.y << ',' << origin.z
                 << "],\"voxel_size\":" << voxelizer->GetVoxelSize()
                 << ",\"voxel_record_stride\":32,\"vertex_stride\":" << sizeof(asset::Vertex)
                 << ",\"node_stride\":" << sizeof(sg::NodeData)
                 << ",\"material_stride\":" << sizeof(sg::MaterialData)
                 << ",\"geometry_revision\":" << voxelizer->GetGeometryRevision()
                 << ",\"class_mask\":" << output.GetClassMask()
                 << ",\"scene_revision\":" << m_renderScene->GetGeometryRevision()
                 << ",\"class_scene_revision\":"
                 << m_renderScene->GetGeometryRevision(output.GetClassMask())
                 << ",\"triangle_count\":" << m_renderScene->GetVoxelTriangleCount()
                 << ",\"reflectance_policy\":\"" << (averaged ? "averaged" : "owner")
                 << "\",\"reflectance_scale\":" << ZEN_VOXEL_REFLECTANCE_SCALE
                 << ",\"reflectance_record_stride\":32,\"has_radiance\":" << hasRadiance << "}\n";
        metadata.flush();
        succeeded = succeeded && metadata.good();
    }
    for (const VoxelCaptureBuffer& buffer : buffers)
    {
        m_renderDevice->DestroyBuffer(buffer.readback);
        m_renderDevice->DestroyBuffer(buffer.copy);
    }
    m_renderDevice->DestroyBuffer(packed);
    m_renderDevice->DestroyBuffer(reflectancePacked);
    if (!succeeded)
    {
        LOGE("Voxel volume capture failed: {}", path);
    }
    return succeeded;
}

bool SceneRendererDemo::CaptureVoxelGBuffer(const std::string& path)
{
    // Render the real GBufferSP with a diagnostic orthographic camera. Nine pixels
    // per voxel keep the atlas at LOD 0 and include its exact XY center sample.
    rc::VoxelizerBase* voxelizer = m_renderDevice->GetRendererServer()->RequestVoxelizer();
    const uint32_t dimension     = voxelizer->GetVoxelTexResolution();
    const uint32_t rasterSize    = dimension * 9;
    const sg::AABB bounds        = voxelizer->GetVoxelBounds();
    const Vec3 minimum           = bounds.GetMin();
    const Vec3 extent            = bounds.GetExtent3D();
    sg::CameraUniformData camera;
    camera.projViewMatrix       = Mat4(1.0f);
    camera.projViewMatrix[0][0] = 2.0f / extent.x;
    camera.projViewMatrix[1][1] = 2.0f / extent.y;
    camera.projViewMatrix[2][2] = 1.0f / extent.z;
    camera.projViewMatrix[3] =
        Vec4(-1.0f - 2.0f * minimum.x / extent.x, -1.0f - 2.0f * minimum.y / extent.y,
             -minimum.z / extent.z, 1.0f);
    RHIBufferCreateInfo info;
    info.size         = dimension * dimension * 48;
    info.allocateType = RHIBufferAllocateType::eGPU;
    info.usageFlags.SetFlags(RHIBufferUsageFlagBits::eStorageBuffer,
                             RHIBufferUsageFlagBits::eTransferSrcBuffer);
    RHIBuffer* packed = m_renderDevice->CreateBuffer(info);
    info.allocateType = RHIBufferAllocateType::eCPURead;
    info.usageFlags   = 0;
    info.usageFlags.SetFlag(RHIBufferUsageFlagBits::eTransferDstBuffer);
    RHIBuffer* readback = m_renderDevice->CreateBuffer(info);
    RHISampler* sampler = m_renderDevice->CreateSampler(RHISamplerCreateInfo::CreateLinearRepeat());
    bool succeeded      = packed != nullptr && readback != nullptr && sampler != nullptr &&
        !m_renderDevice->AreSubmissionsBlocked();
    if (succeeded)
    {
        rc::RenderGraph graph("VoxelGBufferCalibration");
        succeeded = graph.Begin();
        if (succeeded)
        {
            RHIGfxPipelineStates states;
            states.rasterizationState.cullMode = RHIPolygonCullMode::eDisabled;
            states.depthStencilState           = RHIGfxPipelineDepthStencilState::Create(
                false, false, RHIDepthCompareOperator::eNever);
            states.colorBlendState.AddAttachments(5);
            rc::RDGGraphicsPassDesc draw;
            draw.SetPipelineStates(states);
            draw.SetShaderProgramName("GBufferSP");
            draw.SetRenderArea(0, 0, rasterSize, rasterSize);
            draw.AddColorOutput(DataFormat::eR16G16B16A16SFloat, rasterSize, rasterSize,
                                "calibration_position");
            draw.AddColorOutput(DataFormat::eR16G16B16A16SFloat, rasterSize, rasterSize,
                                "calibration_normal");
            draw.AddColorOutput(DataFormat::eR8G8B8A8UNORM, rasterSize, rasterSize,
                                "calibration_albedo");
            draw.AddColorOutput(DataFormat::eR8G8B8A8UNORM, rasterSize, rasterSize,
                                "calibration_metallic");
            draw.AddColorOutput(DataFormat::eR16G16B16A16SFloat, rasterSize, rasterSize,
                                "calibration_emission");
            draw.BindStorageBuffer("NodeBuffer", m_renderScene->GetNodesDataSSBO());
            draw.BindStorageBuffer("MaterialBuffer", m_renderScene->GetMaterialsDataSSBO());
            rc::BindSceneTextureArray(draw, sampler, m_renderScene->GetSceneTextures());
            draw.BindValue("uCameraData", camera);
            draw.BindVertexBuffer(m_renderScene->GetVertexBuffer());
            draw.BindIndexBuffer(m_renderScene->GetIndexBuffer());
            graph.AddGraphicsPass(std::move(draw))
                .RecordPassCommands([draws = rc::SnapshotSceneDraws(*m_renderScene)](
                                        rc::RDGPassCmdEncoder& encoder) {
                    for (const rc::SceneMeshDraw& mesh : draws)
                    {
                        const rc::GBufferSP::PushConstantsData constants{mesh.nodeIndex,
                                                                         mesh.materialIndex};
                        encoder.SetPushConstants(constants);
                        encoder.DrawIndexed(mesh.indexCount, 1, mesh.firstIndex, 0, 0);
                    }
                });
            rc::RDGComputePassDesc copy;
            copy.SetShaderProgramName("VoxelCaptureGBufferSP");
            copy.BindSampledTexture("positionMap", sampler, "calibration_position");
            copy.BindSampledTexture("normalMap", sampler, "calibration_normal");
            copy.BindSampledTexture("albedoMap", sampler, "calibration_albedo");
            copy.BindSampledTexture("metallicMap", sampler, "calibration_metallic");
            copy.BindSampledTexture("emissionMap", sampler, "calibration_emission");
            copy.BindStorageBuffer("GBufferCapture", packed, rc::RDGContentGuarantee::eFullWrite);
            graph.AddComputePass(std::move(copy))
                .RecordPassCommands([dimension](rc::RDGPassCmdEncoder& encoder) {
                    encoder.Dispatch((dimension + 7) / 8, (dimension + 7) / 8, 1);
                });
            graph.AddTransferPass("ReadCalibrationGBuffer")
                .CopyBuffer(packed, readback, {0, 0, info.size})
                .NeverCull();
            succeeded = graph.End() && m_renderDevice->ExecuteRenderGraph(graph);
        }
        m_renderDevice->FlushRHIThread();
        m_renderDevice->WaitForIdle();
        succeeded = succeeded && !m_renderDevice->AreSubmissionsBlocked();
        if (succeeded)
        {
            succeeded = WriteVoxelCaptureBuffer(path, {"gbuffer", packed, readback, info.size});
        }
    }
    m_renderDevice->DestroyBuffer(packed);
    m_renderDevice->DestroyBuffer(readback);
    if (!succeeded)
    {
        LOGE("Voxel G-buffer calibration capture failed: {}", path);
    }
    return succeeded;
}

bool SceneRendererDemo::CaptureVoxelLifecycle(const std::string& path)
{
    // Explicit diagnostic updates, with capture completion between changes. This
    // exercises current producers; it is not the later dynamic-scene update API.
    rc::VoxelizerBase* voxelizer = m_renderDevice->GetRendererServer()->RequestVoxelizer();
    voxelizer->RequestVoxelization();
    bool succeeded = Run(1) && CaptureVoxelVolume(path + ".rebuild");
    if (succeeded)
    {
        succeeded =
            Run(1, false, 2) && Run(1, false, 3) && Run(1) && CaptureVoxelVolume(path + ".modes");
    }
    HeapVector<sg::Node*> originals;
    HeapVector<sg::NodeData> moved;
    for (sg::Node* node : m_scene->GetRenderableNodes())
    {
        originals.push_back(node);
        sg::NodeData data = node->GetData();
        data.modelMatrix =
            glm::translate(Mat4(1.0f), Vec3(0.13f, -0.07f, 0.09f)) * data.modelMatrix;
        data.normalMatrix = glm::transpose(glm::inverse(data.modelMatrix));
        moved.push_back(data);
    }
    if (succeeded && !moved.empty())
    {
        m_renderDevice->UpdateBuffer(m_renderScene->GetNodesDataSSBO(),
                                     static_cast<uint32_t>(moved.size() * sizeof(sg::NodeData)),
                                     reinterpret_cast<const uint8_t*>(moved.data()));
        voxelizer->RequestVoxelization();
        succeeded = Run(1) && CaptureVoxelVolume(path + ".moved");
    }
    if (succeeded)
    {
        m_scene->GetRenderableNodes().clear();
        m_renderScene->PrepareBuffers();
        voxelizer->RequestVoxelization();
        succeeded = Run(1) && CaptureVoxelVolume(path + ".empty");
        for (sg::Node* node : originals)
        {
            m_scene->AddRenderableNode(node);
        }
        m_renderScene->PrepareBuffers();
        voxelizer->RequestVoxelization();
        if (succeeded)
        {
            succeeded = Run(1) && CaptureVoxelVolume(path + ".restored");
        }
    }
    if (!succeeded)
    {
        LOGE("Voxel lifecycle capture failed: {}", path);
    }
    return succeeded;
}

bool SceneRendererDemo::CaptureVoxelReference(const std::string& path)
{
    // Reference occupancy harness only: eight raster samples, sample shading OFF,
    // minSampleShading=1 (ineffective while disabled), full sample mask, no depth/cull.
    // State audited from dvbgi commit 581c116061b1294dad2a8eca6afb45b49cc95a7e.
    rc::VoxelizerBase* voxelizer = m_renderDevice->GetRendererServer()->RequestVoxelizer();
    const uint32_t dimension     = voxelizer->GetVoxelTexResolution();
    rc::ShaderProgram* program =
        rc::ShaderProgramManager::GetInstance().RequestShaderProgram("VoxelCalibrationReferenceSP");
    bool succeeded = program != nullptr && program->GetShader() != nullptr;
    rc::TextureFormat format;
    format.dimension = rc::TextureDimension::e3D;
    format.width = format.height = format.depth = dimension;
    format.format                               = DataFormat::eR32UInt;
    RHITexture* owner      = m_renderDevice->CreateTextureStorage(format, {}, "reference_owner");
    format.dimension       = rc::TextureDimension::e2D;
    format.depth           = 1;
    format.format          = DataFormat::eR8G8B8A8UNORM;
    format.sampleCount     = SampleCount::e8;
    RHITexture* attachment = m_renderDevice->CreateTextureColorRT(format, {}, "reference_raster");
    RHIBufferCreateInfo info;
    info.size         = dimension * dimension * dimension * sizeof(uint32_t);
    info.allocateType = RHIBufferAllocateType::eGPU;
    info.usageFlags.SetFlags(RHIBufferUsageFlagBits::eStorageBuffer,
                             RHIBufferUsageFlagBits::eTransferSrcBuffer);
    RHIBuffer* packed = m_renderDevice->CreateBuffer(info);
    info.allocateType = RHIBufferAllocateType::eCPURead;
    info.usageFlags   = 0;
    info.usageFlags.SetFlag(RHIBufferUsageFlagBits::eTransferDstBuffer);
    RHIBuffer* readback = m_renderDevice->CreateBuffer(info);
    succeeded = succeeded && owner != nullptr && attachment != nullptr && packed != nullptr &&
        readback != nullptr && !m_renderDevice->AreSubmissionsBlocked();
    if (succeeded)
    {
        rc::RenderGraph graph("VoxelReferenceCalibration");
        succeeded = graph.Begin();
        if (succeeded)
        {
            rc::RDGComputePassDesc clear;
            clear.SetShaderProgramName("VoxelClearOwnersSP");
            clear.BindStorageImage("voxelOwner", owner->GetDefaultView(),
                                   rc::RDGContentGuarantee::eFullWrite);
            const glm::uvec3 groups =
                rc::GetVoxelVolumeDispatchGroups(dimension, m_renderDevice->GetGPUInfo());
            graph.AddComputePass(std::move(clear))
                .RecordPassCommands([groups](rc::RDGPassCmdEncoder& encoder) {
                    encoder.Dispatch(groups.x, groups.y, groups.z);
                });
            rc::RDGGraphicsPassDesc draw;
            RHIGfxPipelineStates states;
            states.rasterizationState.cullMode = RHIPolygonCullMode::eDisabled;
            states.depthStencilState           = RHIGfxPipelineDepthStencilState::Create(
                false, false, RHIDepthCompareOperator::eNever);
            states.multiSampleState.sampleCount      = SampleCount::e8;
            states.multiSampleState.minSampleShading = 1.0f;
            states.colorBlendState.AddAttachment();
            draw.SetPipelineStates(states);
            draw.SetShaderProgramName("VoxelCalibrationReferenceSP");
            draw.SetPassTag("PinnedReferenceOccupancy");
            draw.SetRenderArea(0, 0, dimension, dimension);
            draw.AddColorOutput(attachment, RHIRenderTargetLoadOp::eClear);
            draw.BindStorageBuffer("NodeBuffer", m_renderScene->GetNodesDataSSBO());
            draw.BindValue("uVoxelGrid",
                           Vec4(voxelizer->GetSceneMinPoint(), voxelizer->GetVoxelSize()));
            draw.BindStorageImage("voxelOwner", owner->GetDefaultView());
            draw.BindVertexBuffer(m_renderScene->GetVertexBuffer());
            draw.BindIndexBuffer(m_renderScene->GetIndexBuffer());
            graph.AddGraphicsPass(std::move(draw))
                .RecordPassCommands([draws = rc::SnapshotSceneDraws(*m_renderScene),
                                     dimension](rc::RDGPassCmdEncoder& encoder) {
                    for (const rc::SceneMeshDraw& mesh : draws)
                    {
                        const rc::VoxelizationSP::PushConstantsData constants{
                            mesh.nodeIndex, mesh.materialIndex, mesh.firstTriangle, dimension};
                        encoder.SetPushConstants(constants);
                        encoder.DrawIndexed(mesh.indexCount, 1, mesh.firstIndex, 0, 0);
                    }
                });
            rc::RDGComputePassDesc copy;
            copy.SetShaderProgramName("VoxelCaptureOwnersSP");
            copy.BindStorageImage("voxelOwner", owner->GetDefaultView());
            copy.BindStorageBuffer("Owners", packed, rc::RDGContentGuarantee::eFullWrite);
            graph.AddComputePass(std::move(copy))
                .RecordPassCommands([dimension](rc::RDGPassCmdEncoder& encoder) {
                    encoder.Dispatch((dimension + 3) / 4, (dimension + 3) / 4, (dimension + 3) / 4);
                });
            graph.AddTransferPass("ReadReferenceOccupancy")
                .CopyBuffer(packed, readback, {0, 0, info.size})
                .NeverCull();
            succeeded = graph.End() && m_renderDevice->ExecuteRenderGraph(graph);
        }
        m_renderDevice->FlushRHIThread();
        m_renderDevice->WaitForIdle();
        succeeded = succeeded && !m_renderDevice->AreSubmissionsBlocked();
        if (succeeded)
        {
            succeeded = WriteVoxelCaptureBuffer(path, {"reference", packed, readback, info.size});
        }
    }
    m_renderDevice->DestroyTexture(owner);
    m_renderDevice->DestroyTexture(attachment);
    m_renderDevice->DestroyBuffer(packed);
    m_renderDevice->DestroyBuffer(readback);
    if (!succeeded)
    {
        LOGE("Pinned-reference occupancy capture failed: {}", path);
    }
    return succeeded;
}
} // namespace zen
