#include "SceneRendererDemo.h"
#include "Graphics/RenderCore/V2/RenderDevice.h"
#include "Graphics/RenderCore/V2/VoxelResourcePlanning.h"
#include "Graphics/RenderCore/V2/Renderer/RendererServer.h"
#include "Graphics/RenderCore/V2/Renderer/DeferredLightingRenderer.h"
#include "Graphics/RenderCore/V2/Renderer/VoxelizerBase.h"
#include "Graphics/RenderCore/V2/Renderer/VoxelGIRenderer.h"
#include "Graphics/Shared/LightingCapture.h"
#include "Graphics/RenderCore/V2/Renderer/HybridGIRenderer.h"
#include <fstream>
#include <iomanip>
#include <limits>

namespace zen
{
namespace
{
bool CreateCaptureBuffers(rc::RenderDevice& device, uint32_t bytes, RHIBuffer*& output, RHIBuffer*& readback)
{
    RHIBufferCreateInfo info;

    info.size         = bytes;

    info.allocateType = RHIBufferAllocateType::eGPU;

    info.usageFlags.SetFlags(RHIBufferUsageFlagBits::eStorageBuffer, RHIBufferUsageFlagBits::eTransferSrcBuffer);

    info.tag = "lighting_capture";

    output   = device.CreateBuffer(info);

    if (output != nullptr)
    {
        info.allocateType = RHIBufferAllocateType::eCPURead;

        info.usageFlags   = 0;

        info.usageFlags.SetFlag(RHIBufferUsageFlagBits::eTransferDstBuffer);

        info.tag = "lighting_capture_readback";

        readback = device.CreateBuffer(info);
    }

    return output != nullptr && readback != nullptr;
}

bool WriteCaptureBuffer(const std::string& path, RHIBuffer* buffer)
{
    const uint8_t* data = buffer->Map();

    bool valid          = data != nullptr;

    if (valid)
    {
        std::ofstream file(path, std::ios::binary);

        file.write(reinterpret_cast<const char*>(data), buffer->GetRequiredSize());

        file.flush();

        valid = file.good();

        buffer->Unmap();
    }

    return valid;
}

void WriteFloatArray(std::ofstream& output, const float* values, uint32_t count)
{
    output << '[';

    for (uint32_t i = 0; i < count; ++i)
    {
        output << (i == 0 ? "" : ",") << values[i];
    }

    output << ']';
}

bool WriteLightingMetadata(const std::string&        path,
                           const rc::RenderScene&    scene,
                           const rc::RendererServer& server,
                           uint32_t                  width,
                           uint32_t                  height)
{
    const rc::SceneUniformData& data    = *reinterpret_cast<const rc::SceneUniformData*>(scene.GetSceneUniformData());

    const sg::CameraUniformData& camera = *reinterpret_cast<const sg::CameraUniformData*>(scene.GetCameraUniformData());

    const rc::VoxelizerBase& voxelizer  = *server.RequestVoxelizer();

    const glm::uvec2 gbuffer            = server.RequestDeferredLightingRenderer()->GetGBufferExtent();

    const rc::HybridGIRenderer* hybrid   = server.RequestDeferredLightingRenderer()->GetHybridGI();
    const rc::VoxelGISettings&  settings = server.RequestVoxelGI()->GetSettings();
    const bool    reconstructed          = server.GetRenderOption() == rc::RenderOption::eVoxelGI
                                        && settings.rayProvider != rc::VoxelGISettings::RayProvider::Legacy && hybrid != nullptr;
    std::ofstream output(path + ".lighting.json");

    output
        << std::setprecision(std::numeric_limits<float>::max_digits10) << "{\"version\":2,\"width\":" << width
        << ",\"height\":" << height << ",\"format\":\"little-endian float32, pixel-interleaved float4 components, x fastest\""
        << ",\"bytes_per_pixel\":" << ZEN_LIGHTING_CAPTURE_BYTES_PER_PIXEL
        << ",\"components\":[\"combined\",\"analytic_direct\",\"diffuse_outgoing\",\"specular_ibl\",\"visible_emission\",\"escaped_environment_diffuse\",\"bounced_diffuse\"]"
        << ",\"units\":\"scene-linear outgoing RGB before exposure, tone mapping and gamma; receiver BRDF and AO included\""
        << ",\"mask\":\"alpha 1 for covered deferred or forward opaque/mask surfaces, 0 otherwise; before marker overlay\""
        << ",\"capture_frame\":\"one additional frame after --frames; use frozen inputs for baseline comparisons\""
        << ",\"method\":\""
        << (reconstructed                                            ? "hybrid"
            : server.GetRenderOption() == rc::RenderOption::eVoxelGI ? "cone"
                                                                     : "pbr")
        << "\",\"analytic_visibility\":\"mesh_shadow_maps\""
        << ",\"reflectance_policy\":\"" << (voxelizer.UsesAveragedReflectance() ? "averaged" : "owner")
        << "\",\"voxel_resolution\":" << voxelizer.GetVoxelTexResolution()
        << ",\"voxel_geometry_generation\":" << voxelizer.GetGeometryRevision()
        << ",\"scene_geometry_generation\":" << scene.GetGeometryRevision()
        << ",\"scene_surface_generation\":" << scene.GetSurfaceRevision()
        << ",\"voxel_coverage_mask\":" << scene.GetVoxelCoverageMask(voxelizer.GetVoxelBounds()) << ",\"gbuffer_extent\":["
        << gbuffer.x << ',' << gbuffer.y << ']'
        << ",\"indirect_intensity\":" << server.RequestVoxelGI()->GetSettings().indirectIntensity << ",\"camera_position\":";

    WriteFloatArray(output, &data.viewPos.x, 3);

    output << ",\"projection_view_column_major\":";

    WriteFloatArray(output, &camera.projViewMatrix[0][0], 16);

    output << ",\"environment_intensity_rotation_enabled_visible\":";

    WriteFloatArray(output, &data.environment.x, 4);

    output
        << ",\"tier\":\"" << (server.GetRenderOption() == rc::RenderOption::eVoxelGI ? "compute" : "minimum")
        << "\",\"ray_provider\":\"" << (reconstructed ? "voxel" : "legacy")
        << "\",\"requested_provider\":" << static_cast<uint32_t>(settings.rayProvider)
        << ",\"provider_reason\":\"Hardware queries arrive in P4; compute provider selected\""
        << ",\"preset\":\"custom\",\"bounce_source\":\"cone\",\"sample_seed\":0"
        << ",\"sample_count\":" << (settings.referenceSamples != 0 ? settings.referenceSamples : settings.samples)
        << ",\"frame\":" << (hybrid != nullptr ? hybrid->GetFrame() : 0)
        << ",\"environment_generation\":" << scene.GetEnvironmentRevision()
        << ",\"lighting_generation\":" << scene.GetLightingRevision()
        << ",\"temporal\":" << (settings.temporal ? "true" : "false")
        << ",\"static_reference_accumulation\":" << (settings.referenceSamples != 0 ? "true" : "false")
        << ",\"history_valid\":" << (hybrid != nullptr && hybrid->IsHistoryValid() ? "true" : "false")
        << ",\"history_limit\":" << (settings.referenceSamples != 0 ? 256u : settings.historyFrames)
        << ",\"filter_iterations\":" << (settings.filter && settings.referenceSamples == 0 ? 5 : 0) << ",\"reset_reason\":\""
        << (hybrid != nullptr ? hybrid->GetResetReason() : "inactive") << "\""
        << ",\"hybrid_bytes_per_pixel\":" << (reconstructed ? ZEN_HYBRID_CAPTURE_BYTES_PER_PIXEL : 0)
        << ",\"hybrid_components\":[\"raw_sky_nu\",\"sky_nu\",\"raw_bounce\",\"bounce\",\"raw_H_S\",\"H_S\",\"moments\",\"length_rejection\",\"position_valid\",\"normal_roughness\",\"geometric_depth\",\"previous_clip\",\"identity_bits_reserved\"]"
        << ",\"receiver_coverage\":\"opaque_and_mask_triangles; transmission_and_scattering_use_per_surface\"";
    output << ",\"environment_cube_size\":" << scene.GetEnvTexture().pPrefiltered->GetWidth();
    output << ",\"environment_orientation\":";
    WriteFloatArray(output, &data.environmentOrientation.x, 4);
    output << ",\"lights\":[";

    for (uint32_t i = 0; i < static_cast<uint32_t>(data.lightInfo.x); ++i)
    {
        const rc::GPULight& light = data.lights[i];

        output << (i == 0 ? "" : ",") << "{\"position_range\":";

        WriteFloatArray(output, &light.positionRange.x, 4);

        output << ",\"direction_type\":";

        WriteFloatArray(output, &light.directionType.x, 4);

        output << ",\"color_intensity\":";

        WriteFloatArray(output, &light.colorIntensity.x, 4);

        output << ",\"cone_shadow\":";

        WriteFloatArray(output, &light.coneShadow.x, 4);

        output << '}';
    }

    output << "]}\n";

    output.flush();

    return output.good();
}
} // namespace

bool SceneRendererDemo::CaptureLighting(const std::string& path)
{
    rc::RendererServer* server             = m_renderDevice->GetRendererServer();

    rc::DeferredLightingRenderer* renderer = server->RequestDeferredLightingRenderer();

    const rc::RenderOption option          = server->GetRenderOption();

    const uint32_t width                   = m_pViewport->GetWidth();

    const uint32_t height                  = m_pViewport->GetHeight();

    const RHIGPUInfo& gpu                  = m_renderDevice->GetGPUInfo();

    uint64_t bytes                         = 0;

    bool succeeded = (option == rc::RenderOption::ePBR || option == rc::RenderOption::eVoxelGI) && width != 0 && height != 0
                  && gpu.supportFragmentStoresAndAtomics
                  && rc::ValidateGIStorageBuffer(uint64_t(width) * height, ZEN_LIGHTING_CAPTURE_BYTES_PER_PIXEL, gpu, bytes)
                         == rc::GIResourceStatus::eSuccess
                  && !m_renderDevice->AreSubmissionsBlocked();

    RHIBuffer* output   = nullptr;

    RHIBuffer* readback = nullptr;
    RHIBuffer* hybridOutput   = nullptr;
    RHIBuffer* hybridReadback = nullptr;
    const bool captureHybrid  = option == rc::RenderOption::eVoxelGI
                             && server->RequestVoxelGI()->GetSettings().rayProvider != rc::VoxelGISettings::RayProvider::Legacy;

    if (succeeded)
    {
        succeeded = CreateCaptureBuffers(*m_renderDevice, static_cast<uint32_t>(bytes), output, readback);
    }

    if (succeeded)
    {
        if (captureHybrid)
        {
            succeeded = rc::ValidateGIStorageBuffer(uint64_t(width) * height, ZEN_HYBRID_CAPTURE_BYTES_PER_PIXEL, gpu, bytes)
                         == rc::GIResourceStatus::eSuccess
                     && CreateCaptureBuffers(*m_renderDevice, static_cast<uint32_t>(bytes), hybridOutput, hybridReadback);
        }
    }

    if (succeeded)
    {
        renderer->SetHybridCapture(hybridOutput, hybridReadback);
        renderer->SetLightingCapture(output, readback);

        succeeded = Run(1, false, static_cast<uint32_t>(server->GetRequestedRenderOption()) + 1)
                 && renderer->WasLightingCaptureRecorded();

        renderer->SetLightingCapture(nullptr, nullptr);
        renderer->SetHybridCapture(nullptr, nullptr);

        // Diagnostic-only synchronization: own both buffers until copy completion.
        m_renderDevice->FlushRHIThread();

        m_renderDevice->WaitForIdle();

        succeeded = succeeded && !m_renderDevice->AreSubmissionsBlocked();
    }

    if (succeeded && captureHybrid)
    {
        const rc::EnvTexture& env          = m_renderScene->GetEnvTexture();
        const uint32_t        side         = env.pPrefiltered->GetWidth();
        RHIBuffer*            cubeOutput   = nullptr;
        RHIBuffer*            cubeReadback = nullptr;
        succeeded =
            rc::ValidateGIStorageBuffer(uint64_t(side) * side * 6, sizeof(Vec4), gpu, bytes) == rc::GIResourceStatus::eSuccess
            && CreateCaptureBuffers(*m_renderDevice, static_cast<uint32_t>(bytes), cubeOutput, cubeReadback);
        if (succeeded)
        {
            rc::RenderGraph graph("capture_hybrid_environment");
            succeeded = graph.Begin();
            rc::RDGComputePassDesc pass;
            pass.SetShaderProgramName("CaptureHybridEnvironmentSP");
            pass.BindSampledTexture("sourceEnvironment", env.pPrefilteredSampler, env.pPrefiltered->GetDefaultView());
            pass.BindStorageBuffer("EnvironmentCapture", cubeOutput, rc::RDGContentGuarantee::eFullWrite);
            graph.AddComputePass(std::move(pass)).RecordPassCommands([side](rc::RDGPassCmdEncoder& encoder) {
                encoder.Dispatch((side + 7) / 8, (side + 7) / 8, 6);
            });
            graph.AddTransferPass("ReadHybridEnvironment").CopyBuffer(cubeOutput, cubeReadback, {0, 0, bytes}).NeverCull();
            succeeded = succeeded && graph.End() && m_renderDevice->ExecuteRenderGraph(graph);
            m_renderDevice->FlushRHIThread();
            m_renderDevice->WaitForIdle();
            succeeded = succeeded && !m_renderDevice->AreSubmissionsBlocked()
                     && WriteCaptureBuffer(path + ".environment.bin", cubeReadback);
        }
        m_renderDevice->DestroyBuffer(cubeOutput);
        m_renderDevice->DestroyBuffer(cubeReadback);
    }

    if (succeeded)
    {
        succeeded = WriteCaptureBuffer(path + ".lighting.bin", readback)
                 && (!captureHybrid || WriteCaptureBuffer(path + ".hybrid.bin", hybridReadback))
                 && WriteLightingMetadata(path, *m_renderScene, *server, width, height);
    }

    m_renderDevice->DestroyBuffer(hybridOutput);
    m_renderDevice->DestroyBuffer(hybridReadback);
    m_renderDevice->DestroyBuffer(output);

    m_renderDevice->DestroyBuffer(readback);

    if (!succeeded)
    {
        LOGE("Linear lighting capture failed (requires PBR/GI, fragment stores, valid extent and storage range): {}", path);
    }

    return succeeded;
}
} // namespace zen
