#include "SceneRendererDemo.h"
#include "Graphics/RenderCore/V2/RenderDevice.h"
#include "Graphics/RenderCore/V2/VoxelResourcePlanning.h"
#include "Graphics/RenderCore/V2/Renderer/RendererServer.h"
#include "Graphics/RenderCore/V2/Renderer/DeferredLightingRenderer.h"
#include "Graphics/RenderCore/V2/Renderer/VoxelizerBase.h"
#include "Graphics/RenderCore/V2/Renderer/VoxelGIRenderer.h"
#include "Graphics/Shared/LightingCapture.h"
#include "Graphics/RenderCore/V2/Renderer/HybridGIRenderer.h"
#include <cmath>
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
                           uint32_t                  height,
                           const glm::uvec2&         skyCacheCounts)
{
    const rc::SceneUniformData& data     = *reinterpret_cast<const rc::SceneUniformData*>(scene.GetSceneUniformData());

    const sg::CameraUniformData& camera  = *reinterpret_cast<const sg::CameraUniformData*>(scene.GetCameraUniformData());

    const rc::VoxelizerBase& voxelizer   = *server.RequestVoxelizer();

    const glm::uvec2 gbuffer             = server.RequestDeferredLightingRenderer()->GetGBufferExtent();

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
        << ",\"tier\":\""
        << (server.GetRenderOption() == rc::RenderOption::eVoxelGI
                ? (server.RequestVoxelGI()->UsesHardwareQueries() ? "hardware" : "compute")
                : "minimum")
        << "\",\"ray_provider\":\""
        << (reconstructed ? (server.RequestVoxelGI()->UsesHardwareQueries() ? "hardware" : "voxel") : "legacy")
        << "\",\"receiver_position\":\""
        << (reconstructed && server.RequestVoxelGI()->UsesHardwareQueries() ? "raster_fp32" : "depth")
        << "\",\"requested_provider\":" << static_cast<uint32_t>(settings.rayProvider) << ",\"provider_reason\":\""
        << server.RequestVoxelGI()->GetProviderReason() << "\""
        << ",\"ray_scene_ready\":" << (server.RequestVoxelGI()->GetSceneRayQuery().IsReady() ? "true" : "false")
        << ",\"ray_scene_generation\":" << server.RequestVoxelGI()->GetSceneRayQuery().GetGeneration()
        << ",\"ray_scene_bytes\":" << server.RequestVoxelGI()->GetSceneRayQuery().GetMemoryBytes()
        << ",\"ray_query_enabled\":" << (GDynamicRHI->QueryGPUInfo().rayQuery.rayQuery ? "true" : "false")
        << ",\"acceleration_structure_enabled\":"
        << (GDynamicRHI->QueryGPUInfo().rayQuery.accelerationStructure ? "true" : "false")
        << ",\"buffer_device_address_enabled\":"
        << (GDynamicRHI->QueryGPUInfo().rayQuery.bufferDeviceAddress ? "true" : "false")
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
        << ",\"hybrid_components\":[\"raw_sky_nu\",\"sky_nu\",\"raw_bounce\",\"bounce\",\"raw_H_S\",\"H_S\",\"moments\",\"length_rejection\",\"position_valid\",\"normal_roughness\",\"geometric_depth\",\"motion_ndc_previous_w\",\"identity_bits_reserved\"]"
        << ",\"receiver_coverage\":\"opaque_and_mask_triangles; transmission_and_scattering_use_per_surface\"";
    output << ",\"environment_cube_size\":" << scene.GetEnvTexture().pPrefiltered->GetWidth();
    output << ",\"sky_cache_evaluated\":" << skyCacheCounts.x << ",\"sky_cache_center_fallback\":" << skyCacheCounts.y;
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

bool SceneRendererDemo::CaptureProviderSwitching(const std::string& path)
{
    rc::VoxelGIRenderer*      gi        = m_renderDevice->GetRendererServer()->RequestVoxelGI();
    const rc::VoxelGISettings original  = gi->GetSettings();
    bool                      succeeded = true;
    for (uint32_t stage = 0; stage < 3 && succeeded; ++stage)
    {
        rc::VoxelGISettings settings = original;
        settings.rayProvider =
            stage == 1 ? rc::VoxelGISettings::RayProvider::Voxel : rc::VoxelGISettings::RayProvider::Hardware;
        const std::string prefix = fmt::format("{}-{}", path, stage);
        succeeded = gi->SetSettings(settings) && CaptureLighting(prefix + "-frame1") && Run(30, false, 3, {}, true)
                 && CaptureLighting(prefix + "-frame32");
    }
    const bool restored = gi->SetSettings(original);
    return succeeded && restored;
}

bool SceneRendererDemo::CaptureOriginStress(const std::string& path)
{
    const HeapVector<sg::Node*>& nodes = m_scene->GetRenderableNodes();
    HeapVector<Mat4>             transforms;
    transforms.reserve(nodes.size());
    for (const sg::Node* node : nodes)
    {
        transforms.push_back(node->GetData().modelMatrix);
    }
    const Vec3  camera      = m_camera->GetPos();
    const Vec3  front       = -Vec3(glm::inverse(m_camera->GetViewMatrix())[2]);
    const float distances[] = {0.0f, 100.0f, 10000.0f};
    bool        succeeded   = true;
    for (uint32_t stage = 0; stage < 3 && succeeded; ++stage)
    {
        const Vec3 offset(distances[stage], 0.0f, 0.0f);
        for (uint32_t i = 0; i < nodes.size() && succeeded; ++i)
        {
            succeeded = m_renderScene->SetInstanceTransform(nodes[i]->GetRenderableIndex(),
                                                            glm::translate(Mat4(1.0f), offset) * transforms[i]);
        }
        m_camera->SetPose(camera + offset, camera + offset + front);
        succeeded = succeeded && Run(63, false, 3, {}, true) && CaptureLighting(fmt::format("{}-{}", path, stage));
    }
    m_camera->SetPose(camera, camera + front);
    for (uint32_t i = 0; i < nodes.size(); ++i)
    {
        const bool restored = m_renderScene->SetInstanceTransform(nodes[i]->GetRenderableIndex(), transforms[i]);
        succeeded           = restored && succeeded;
    }
    if (!succeeded)
    {
        LOGE("Origin-stress capture or scene restoration failed");
    }
    return succeeded;
}

bool SceneRendererDemo::CaptureStability(const std::string& path)
{
    rc::VoxelGIRenderer*            gi        = m_renderDevice->GetRendererServer()->RequestVoxelGI();
    const rc::VoxelGISettings       settings  = gi->GetSettings();
    const HeapVector<asset::Vertex> vertices  = m_renderScene->GetVertices();
    const HeapVector<sg::Material*> materials = m_scene->GetComponents<sg::Material>();
    HeapVector<sg::MaterialData>    originalMaterials;
    for (const sg::Material* material : materials)
    {
        originalMaterials.push_back(material->data);
    }
    const Vec3     camera      = m_camera->GetPos();
    const Mat4     inverseView = glm::inverse(m_camera->GetViewMatrix());
    const Vec3     front       = -Vec3(inverseView[2]);
    const Vec3     right       = Vec3(inverseView[0]);
    const char*    stages[]    = {"motion", "cut", "deform", "alpha"};
    const uint32_t frames[]    = {1, 4, 8, 32};
    bool           succeeded   = settings.referenceSamples == 0 && gi->UsesHardwareQueries() && !vertices.empty();
    for (uint32_t stage = 0; stage < 4 && succeeded; ++stage)
    {
        // Begin each event with a settled original scene. Captures after the event
        // are compared with a fresh static reference of that same altered scene.
        succeeded = Run(63, false, 3, {}, true);
        if (stage == 0)
        {
            for (uint32_t frame = 1; frame <= 16 && succeeded; ++frame)
            {
                const Vec3 eye = camera + right * (0.02f * float(frame) / 16.0f);
                m_camera->SetPose(eye, eye + front);
                succeeded = Run(1, false, 3, {}, true);
            }
        }
        else if (stage == 1)
        {
            const Vec3 eye = camera + front * 0.3f;
            m_camera->SetPose(eye, eye + front);
        }
        else if (stage == 2)
        {
            HeapVector<asset::Vertex> deformed = vertices;
            float                     minimum  = vertices[0].pos.x;
            float                     maximum  = minimum;
            for (const asset::Vertex& vertex : vertices)
            {
                minimum = glm::min(minimum, vertex.pos.x);
                maximum = glm::max(maximum, vertex.pos.x);
            }
            const float extent = glm::max(maximum - minimum, 1e-6f);
            for (asset::Vertex& vertex : deformed)
            {
                vertex.pos.y += 0.01f * extent * std::sin(6.2831853f * (vertex.pos.x - minimum) / extent);
            }
            succeeded = succeeded && m_renderScene->UpdateVertices(0, deformed);
        }
        else
        {
            uint32_t edited = 0;
            for (uint32_t index = 0; index < originalMaterials.size() && succeeded; ++index)
            {
                sg::MaterialData material = originalMaterials[index];
                if (material.surfaceProperties.y == float(sg::AlphaMode::Mask))
                {
                    material.baseColorFactor.a *= 0.5f;
                    material.diffuseFactor.a   *= 0.5f;
                    succeeded                   = m_renderScene->UpdateMaterial(index, material);
                    ++edited;
                }
            }
            succeeded = succeeded && edited != 0;
        }
        uint32_t previous = 0;
        for (uint32_t frame : frames)
        {
            if (succeeded && frame > previous + 1)
            {
                succeeded = Run(frame - previous - 1, false, 3, {}, true);
            }
            succeeded = succeeded && CaptureLighting(fmt::format("{}-{}-frame{}", path, stages[stage], frame));
            previous  = frame;
        }
        rc::VoxelGISettings reference = settings;
        reference.referenceSamples    = 1024;
        succeeded                     = succeeded && gi->SetSettings(reference) && Run(63, false, 3, {}, true)
                                     && CaptureLighting(fmt::format("{}-{}-reference", path, stages[stage]));
        const bool restoredSettings   = gi->SetSettings(settings);
        bool       restoredScene      = true;
        m_camera->SetPose(camera, camera + front);
        if (stage == 2)
        {
            restoredScene = m_renderScene->UpdateVertices(0, vertices);
        }
        else if (stage == 3)
        {
            for (uint32_t index = 0; index < originalMaterials.size(); ++index)
            {
                const bool restored = m_renderScene->UpdateMaterial(index, originalMaterials[index]);
                restoredScene       = restored && restoredScene;
            }
        }
        succeeded = succeeded && restoredSettings && restoredScene;
    }
    if (!succeeded)
    {
        LOGE("Stability capture requires the hardware shipping tier and masked materials; capture or restoration failed");
    }
    return succeeded;
}

bool SceneRendererDemo::CaptureLighting(const std::string& path)
{
    rc::RendererServer* server             = m_renderDevice->GetRendererServer();

    rc::DeferredLightingRenderer* renderer = server->RequestDeferredLightingRenderer();

    const rc::RenderOption option          = server->GetRenderOption();

    const uint32_t width                   = m_pViewport->GetWidth();

    const uint32_t height                  = m_pViewport->GetHeight();

    const RHIGPUInfo& gpu                  = m_renderDevice->GetGPUInfo();

    uint64_t bytes                         = 0;

    bool succeeded    = (option == rc::RenderOption::ePBR || option == rc::RenderOption::eVoxelGI) && width != 0 && height != 0
                     && gpu.supportFragmentStoresAndAtomics
                     && rc::ValidateGIStorageBuffer(uint64_t(width) * height, ZEN_LIGHTING_CAPTURE_BYTES_PER_PIXEL, gpu, bytes)
                            == rc::GIResourceStatus::eSuccess
                     && !m_renderDevice->AreSubmissionsBlocked();

    RHIBuffer* output = nullptr;

    RHIBuffer* readback       = nullptr;
    RHIBuffer* hybridOutput   = nullptr;
    RHIBuffer* hybridReadback = nullptr;
    glm::uvec2 skyCacheCounts(0);
    const bool captureHybrid = option == rc::RenderOption::eVoxelGI
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
        const rc::EnvTexture& env           = m_renderScene->GetEnvTexture();
        const uint32_t        side          = env.pPrefiltered->GetWidth();
        RHIBuffer*            cubeOutput    = nullptr;
        RHIBuffer*            cubeReadback  = nullptr;
        RHIBuffer*            cacheOutput   = nullptr;
        RHIBuffer*            cacheReadback = nullptr;
        const uint32_t        cacheDepth    = server->RequestVoxelGI()->GetSkyIrradianceTexture()->GetDepth();
        const uint32_t        cacheBytes    = cacheDepth * sizeof(glm::uvec2);
        succeeded =
            rc::ValidateGIStorageBuffer(uint64_t(side) * side * 6, sizeof(Vec4), gpu, bytes) == rc::GIResourceStatus::eSuccess
            && CreateCaptureBuffers(*m_renderDevice, static_cast<uint32_t>(bytes), cubeOutput, cubeReadback)
            && CreateCaptureBuffers(*m_renderDevice, cacheBytes, cacheOutput, cacheReadback);
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
            rc::RDGComputePassDesc cache;
            cache.SetShaderProgramName("CaptureSkyCacheSP");
            cache.BindSampledTexture("sourceSkyCache", server->RequestVoxelizer()->GetVoxelSampler(),
                                     server->RequestVoxelGI()->GetSkyIrradianceTexture()->GetDefaultView());
            cache.BindStorageBuffer("SkyCacheCapture", cacheOutput, rc::RDGContentGuarantee::eFullWrite);
            graph.AddComputePass(std::move(cache)).RecordPassCommands([cacheDepth](rc::RDGPassCmdEncoder& encoder) {
                encoder.Dispatch((cacheDepth + 63) / 64, 1, 1);
            });
            graph.AddTransferPass("ReadSkyCacheCounts").CopyBuffer(cacheOutput, cacheReadback, {0, 0, cacheBytes}).NeverCull();
            succeeded = succeeded && graph.End() && m_renderDevice->ExecuteRenderGraph(graph);
            m_renderDevice->FlushRHIThread();
            m_renderDevice->WaitForIdle();
            succeeded = succeeded && !m_renderDevice->AreSubmissionsBlocked()
                     && WriteCaptureBuffer(path + ".environment.bin", cubeReadback);
            if (succeeded)
            {
                const glm::uvec2* counts = reinterpret_cast<const glm::uvec2*>(cacheReadback->Map());
                succeeded                = counts != nullptr;
                if (succeeded)
                {
                    for (uint32_t z = 0; z < cacheDepth; ++z)
                    {
                        skyCacheCounts += counts[z];
                    }
                    cacheReadback->Unmap();
                }
            }
        }
        m_renderDevice->DestroyBuffer(cubeOutput);
        m_renderDevice->DestroyBuffer(cubeReadback);
        m_renderDevice->DestroyBuffer(cacheOutput);
        m_renderDevice->DestroyBuffer(cacheReadback);
    }

    if (succeeded)
    {
        succeeded = WriteCaptureBuffer(path + ".lighting.bin", readback)
                 && (!captureHybrid || WriteCaptureBuffer(path + ".hybrid.bin", hybridReadback))
                 && WriteLightingMetadata(path, *m_renderScene, *server, width, height, skyCacheCounts);
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
