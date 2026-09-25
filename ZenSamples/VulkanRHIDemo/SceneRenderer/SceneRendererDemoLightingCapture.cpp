#include "SceneRendererDemo.h"
#include "Graphics/RenderCore/V2/RenderDevice.h"
#include "Graphics/RenderCore/V2/RenderConfig.h"
#include "Graphics/RenderCore/V2/DynamicVoxelGIPlanning.h"
#include "Graphics/RenderCore/V2/Renderer/RendererServer.h"
#include "Graphics/RenderCore/V2/Renderer/DeferredLightingRenderer.h"
#include "Graphics/RenderCore/V2/Renderer/VoxelizerBase.h"
#include "Graphics/RenderCore/V2/Renderer/VoxelGIRenderer.h"
#include "Graphics/Shared/LightingCapture.h"
#include <fstream>
#include <iomanip>
#include <limits>

namespace zen
{
namespace
{
bool CreateCaptureBuffers(rc::RenderDevice& device,
                          uint32_t bytes,
                          RHIBuffer*& output,
                          RHIBuffer*& readback)
{
    RHIBufferCreateInfo info;
    info.size         = bytes;
    info.allocateType = RHIBufferAllocateType::eGPU;
    info.usageFlags.SetFlags(RHIBufferUsageFlagBits::eStorageBuffer,
                             RHIBufferUsageFlagBits::eTransferSrcBuffer);
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

bool WriteLightingMetadata(const std::string& path,
                           const rc::RenderScene& scene,
                           const rc::RendererServer& server,
                           uint32_t width,
                           uint32_t height,
                           uint32_t fallbackFlags)
{
    const rc::SceneUniformData& data =
        *reinterpret_cast<const rc::SceneUniformData*>(scene.GetSceneUniformData());
    const sg::CameraUniformData& camera =
        *reinterpret_cast<const sg::CameraUniformData*>(scene.GetCameraUniformData());
    const rc::VoxelizerBase& voxelizer = *server.RequestVoxelizer();
    std::ofstream output(path + ".lighting.json");
    output
        << std::setprecision(std::numeric_limits<float>::max_digits10)
        << "{\"version\":1,\"width\":" << width << ",\"height\":" << height
        << ",\"format\":\"little-endian float32, pixel-interleaved float4 components, x fastest\""
        << ",\"bytes_per_pixel\":" << ZEN_LIGHTING_CAPTURE_BYTES_PER_PIXEL
        << ",\"components\":[\"combined\",\"analytic_direct\",\"diffuse_outgoing\",\"specular_ibl\",\"visible_emission\",\"escaped_environment_diffuse\",\"bounced_diffuse\"]"
        << ",\"units\":\"scene-linear outgoing RGB before exposure, tone mapping and gamma; receiver BRDF and AO included\""
        << ",\"mask\":\"alpha 1 for deferred surfaces, 0 for background; captured before marker overlay\""
        << ",\"capture_frame\":\"one additional frame after --frames; use frozen inputs for baseline comparisons\""
        << ",\"directional_diffuse_note\":\"dynamic_voxel stores combined hit/escaped diffuse in bounced_diffuse; use transport toggles or zero indirect gain to isolate terms\""
        << ",\"method\":\""
        << (server.GetRenderOption() == rc::RenderOption::eVoxelGI ?
                (server.GetVoxelGISelection().method == rc::VoxelGIMethod::eDynamicVoxel &&
                         fallbackFlags == 0 ?
                     "dynamic_voxel" :
                     "cone") :
                "pbr")
        << "\",\"query_backend\":\""
        << (server.GetRenderOption() == rc::RenderOption::eVoxelGI && fallbackFlags == 0 ?
                server.GetVoxelGISelection().backend :
                "none")
        << "\",\"analytic_visibility\":\"mesh_shadow_maps\""
        << ",\"directional_reconstruction\":\"even_quadratic_odd_linear\""
        << ",\"spatial_kernel\":\"gaussian_times_surface_normal_cosine\""
        << ",\"reflectance_policy\":\""
        << (voxelizer.UsesAveragedReflectance() ? "averaged" : "owner")
        << "\",\"voxel_resolution\":" << voxelizer.GetVoxelTexResolution()
        << ",\"voxel_geometry_generation\":" << voxelizer.GetGeometryRevision()
        << ",\"scene_geometry_generation\":" << scene.GetGeometryRevision()
        << ",\"scene_surface_generation\":" << scene.GetSurfaceRevision()
        << ",\"voxel_coverage_mask\":" << scene.GetVoxelCoverageMask(voxelizer.GetVoxelBounds())
        << ",\"gbuffer_extent\":" << rc::RenderConfig::GetInstance().offScreenFbSize
        << ",\"indirect_intensity\":" << server.RequestVoxelGI()->GetSettings().indirectIntensity
        << ",\"camera_position\":";
    WriteFloatArray(output, &data.viewPos.x, 3);
    output << ",\"projection_view_column_major\":";
    WriteFloatArray(output, &camera.projViewMatrix[0][0], 16);
    output << ",\"environment_intensity_rotation_enabled_visible\":";
    WriteFloatArray(output, &data.environment.x, 4);
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
    bool succeeded = (option == rc::RenderOption::ePBR || option == rc::RenderOption::eVoxelGI) &&
        width != 0 && height != 0 && gpu.supportFragmentStoresAndAtomics &&
        rc::ValidateGIStorageBuffer(uint64_t(width) * height, ZEN_LIGHTING_CAPTURE_BYTES_PER_PIXEL,
                                    gpu, bytes) == rc::GIResourceStatus::eSuccess &&
        !m_renderDevice->AreSubmissionsBlocked();
    const bool dynamic = option == rc::RenderOption::eVoxelGI &&
        server->GetVoxelGISelection().method == rc::VoxelGIMethod::eDynamicVoxel;
    uint32_t fallbackFlags     = 0;
    RHIBuffer* surfaceOutput   = nullptr;
    RHIBuffer* surfaceReadback = nullptr;
    RHIBuffer* output   = nullptr;
    RHIBuffer* readback = nullptr;
    if (succeeded)
    {
        succeeded =
            CreateCaptureBuffers(*m_renderDevice, static_cast<uint32_t>(bytes), output, readback);
        if (succeeded && dynamic)
        {
            succeeded = CreateCaptureBuffers(*m_renderDevice,
                                             width * height * ZEN_SURFACE_CAPTURE_BYTES_PER_PIXEL,
                                             surfaceOutput, surfaceReadback);
        }
    }
    if (succeeded)
    {
        renderer->SetLightingCapture(output, readback, surfaceOutput, surfaceReadback);
        succeeded = Run(1, false, static_cast<uint32_t>(server->GetRequestedRenderOption()) + 1) &&
            renderer->WasLightingCaptureRecorded();
        renderer->SetLightingCapture(nullptr, nullptr);
        // Diagnostic-only synchronization: own both buffers until copy completion.
        m_renderDevice->FlushRHIThread();
        m_renderDevice->WaitForIdle();
        succeeded = succeeded && !m_renderDevice->AreSubmissionsBlocked();
    }
    if (succeeded)
    {
        succeeded = WriteCaptureBuffer(path + ".lighting.bin", readback) &&
            (!dynamic ||
             (WriteCaptureBuffer(path + ".surface.bin", surfaceReadback) &&
              CaptureStaticGI(path, fallbackFlags))) &&
            WriteLightingMetadata(path, *m_renderScene, *server, width, height, fallbackFlags);
    }
    m_renderDevice->DestroyBuffer(surfaceOutput);
    m_renderDevice->DestroyBuffer(surfaceReadback);
    m_renderDevice->DestroyBuffer(output);
    m_renderDevice->DestroyBuffer(readback);
    if (!succeeded)
    {
        LOGE(
            "Linear lighting capture failed (requires PBR/GI, fragment stores, valid extent and storage range): {}",
            path);
    }
    return succeeded;
}
} // namespace zen
