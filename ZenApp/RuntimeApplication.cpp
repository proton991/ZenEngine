#include "RuntimeApplication.h"
#include "RuntimeLog.h"
#include "AssetLib/SceneLoader.h"
#include "Graphics/RenderCore/V2/RenderDevice.h"
#include "Graphics/RenderCore/V2/Renderer/RendererServer.h"
#include "Graphics/RenderCore/V2/Renderer/DeferredLightingRenderer.h"
#include "Graphics/RenderCore/V2/ScenePresentation.h"
#include "Graphics/RenderCore/V2/SceneRendering.h"
#include "Graphics/RenderCore/V2/ShaderProgram.h"
#include "Graphics/RenderCore/V2/Renderer/HybridGIRenderer.h"
#include "Graphics/RenderCore/V2/Renderer/VoxelGIRenderer.h"
#include "Graphics/Shared/LightingCapture.h"
#include "Platform/NativeWindow.h"
#include "ZenApp/RuntimeWindow.h"
#include <fstream>
#include <algorithm>
#include <iomanip>
#include <chrono>
#include <cmath>
#include <cstring>
#if defined(ZEN_RUNTIME_DIAGNOSTICS_UI)
#    include "ZenApp/RuntimeOverlay.h"
#endif

namespace zen::runtime
{
namespace
{
const char* GetRayProviderName(rc::VoxelGISettings::RayProvider provider)
{
    const char* name = "auto";
    if (provider == rc::VoxelGISettings::RayProvider::Voxel)
    {
        name = "compute";
    }
    else if (provider == rc::VoxelGISettings::RayProvider::Hardware)
    {
        name = "hardware";
    }
    return name;
}

class RuntimeApplication final
{
public:
    RuntimeApplication(const RuntimeLaunchSettings& setup, const RuntimeOptions& options) : m_setup(setup), m_options(options)
    {
        m_log      = std::make_shared<RuntimeLog>();
        m_logReady = m_log->Open(std::filesystem::u8path(options.logPath));
        if (!m_logReady)
        {
            m_error = "Cannot create runtime log: " + options.logPath;
        }
        spdlog::default_logger()->sinks().push_back(m_log);
    }

    ~RuntimeApplication()
    {
        if (m_device)
        {
            m_device->WaitForPreviousFrames();
            for (RHIBuffer* buffer : m_captureBuffers)
            {
                m_device->DestroyBuffer(buffer);
            }
#if defined(ZEN_RUNTIME_DIAGNOSTICS_UI)
            m_overlay.Destroy();
#endif
            m_session.Reset();
            if (m_rendering)
            {
                m_rendering->Destroy();
                m_rendering.Reset();
            }
            if (m_target)
            {
                m_target->Destroy();
                m_target.Reset();
            }
            rc::ShaderProgramManager::GetInstance().Destroy();
            m_device->Destroy();
            m_device.Reset();
        }
        std::vector<spdlog::sink_ptr>& sinks = spdlog::default_logger()->sinks();
        sinks.erase(std::remove(sinks.begin(), sinks.end(), m_log), sinks.end());
    }

    RuntimeExit Run()
    {
        RuntimeExit                 result = Initialize();
        bool                        valid  = result == RuntimeExit::Success;
        const std::function<void()> redraw = [this, &valid]() {
            if (valid && m_session && !m_session->IsClosing() && KeepRunning())
            {
                valid = RenderIteration();
            }
        };
        while (valid && m_session && !m_session->IsClosing() && KeepRunning())
        {
            platform::NativeWindow& window = *m_session->GetWindow();
            window.Update(false, redraw);
            redraw();
            const platform::WindowExtent extent = window.GetFramebufferExtent();
            if (window.IsMinimized() || extent.width == 0 || extent.height == 0)
            {
                platform::NativeWindow::WaitEvents(0.05, redraw);
            }
            else if (m_session->GetState() == RuntimeState::Paused)
            {
                platform::NativeWindow::WaitEvents(1.0 / 15.0, redraw);
            }
        }
        if (result == RuntimeExit::Success && (!valid || (!m_options.batch.empty() && !m_batchDone)))
        {
            result = RuntimeExit::Renderer;
        }
        if (m_device)
        {
            m_device->WaitForPreviousFrames();
            m_device->GetRDGMetrics().CollectGPUResults();
        }
        if (result == RuntimeExit::Success && !m_options.profile.empty())
        {
            m_profile.flush();
            if (!m_profile.good() || !WriteProfileSummary())
            {
                m_error = "Cannot write runtime profile output";
                result  = RuntimeExit::Renderer;
            }
        }
        if (!m_error.empty())
        {
            LOGE("ZenApp: {}", m_error);
        }
        return result;
    }

private:
    // Places the camera for the scene frame about to render; navigation input is locked meanwhile.
    bool ApplyCameraPath(uint64_t frame)
    {
        m_session->SetInputCapture(true, true);
        const bool valid = m_camera.SetState(CameraPathState(m_options.cameraKeys, frame, m_camera.GetState()));
        if (!valid)
        {
            m_error = "Camera path pose at frame " + std::to_string(frame) + " is not a valid camera";
        }
        return valid;
    }

    bool KeepRunning() const
    {
        return m_options.batch.empty() ? (m_options.frames == 0 || GetSceneFrame() < m_options.frames) : !m_batchDone;
    }

    bool ApplyLightPath(uint64_t frame)
    {
        bool       found    = false;
        bool       changed  = false;
        const Vec3 position = LightPathPosition(m_options.lightKeys, frame);
        for (rc::RenderingLight& entry : m_setup.rendering.lights)
        {
            if (entry.id == m_options.lightKeys.id && entry.light.type != rc::SceneLightType::eDirectional)
            {
                found                = true;
                changed              = entry.light.position != position;
                entry.light.position = position;
            }
        }
        if (!found)
        {
            m_error = "Light path lightId must identify a point or spot light in the launch setup";
        }
        return found && (!changed || m_rendering->ApplyRenderingSettings(m_setup.rendering, false, m_error));
    }

    bool CreateCaptureBuffers()
    {
        const uint32_t strides[] = {ZEN_LIGHTING_CAPTURE_BYTES_PER_PIXEL, ZEN_HYBRID_CAPTURE_BYTES_PER_PIXEL};
        bool           valid     = true;
        for (uint32_t index = 0; index < 4 && valid; ++index)
        {
            RHIBufferCreateInfo info;
            info.size = uint64_t(m_setup.run.renderWidth) * m_setup.run.renderHeight * strides[index / 2];
            // Batch diagnostics are written only at capture frames. Keeping their two
            // large storage outputs in host memory avoids another 3 GB of resident VRAM
            // at 4K; the existing transfer/readback ordering still governs CPU access.
            info.allocateType = RHIBufferAllocateType::eCPURead;
            info.usageFlags.SetFlags(RHIBufferUsageFlagBits::eStorageBuffer, RHIBufferUsageFlagBits::eTransferSrcBuffer,
                                     RHIBufferUsageFlagBits::eTransferDstBuffer);
            m_captureBuffers[index] = m_device->CreateBuffer(info);
            valid                   = m_captureBuffers[index] != nullptr;
        }
        return valid;
    }

    bool ReadCaptureBuffer(uint32_t index, HeapVector<Vec4>& values)
    {
        RHIBuffer*     buffer = m_captureBuffers[index];
        const uint8_t* data   = buffer->Map();
        bool           valid  = data != nullptr;
        if (valid)
        {
            const size_t stride = index == 1 ? ZEN_LIGHTING_CAPTURE_BYTES_PER_PIXEL : ZEN_HYBRID_CAPTURE_BYTES_PER_PIXEL;
            const size_t bytes  = size_t(m_setup.run.renderWidth) * m_setup.run.renderHeight * stride;
            values.resize(bytes / sizeof(Vec4));
            std::memcpy(values.data(), data, bytes);
            buffer->Unmap();
        }
        return valid;
    }

    static bool WriteCaptureFile(const std::string& path, const HeapVector<Vec4>& values)
    {
        std::ofstream output(std::filesystem::u8path(path), std::ios::binary);
        output.write(reinterpret_cast<const char*>(values.data()), static_cast<std::streamsize>(values.size() * sizeof(Vec4)));
        output.close();
        return output.good();
    }

    // Why the capture just submitted cannot be used, or nullptr.
    const char* BatchCaptureFailure(const rc::DeferredLightingRenderer& renderer)
    {
        const bool                  submitted = m_device->PollFrameSubmissions();
        const rc::HybridGIRenderer* hybrid    = renderer.GetHybridGI();
        return !submitted                                         ? "the frame submission failed"
             : !renderer.WasLightingCaptureRecorded()             ? "the lighting capture was not recorded"
             : hybrid == nullptr || !hybrid->WasCaptureRecorded() ? "the hybrid GI capture was not recorded"
             : !hybrid->IsHistoryValid()                          ? "the hybrid GI history is invalid"
                                                                  : nullptr;
    }

    // Root-mean-square change of D_direct, D_sky and D_bounce since the previous capture.
    bool BatchCaptureConverged(const HeapVector<Vec4>& values, const GroundTruthFrame& frame, double (&rms)[3]) const
    {
        size_t count = 0;
        for (size_t pixel = 0; !m_previousLighting.empty() && pixel < values.size(); pixel += ZEN_LIGHTING_CAPTURE_COMPONENTS)
        {
            if (values[pixel + 7].w > 0 && m_previousLighting[pixel + 7].w > 0)
            {
                ++count;
                for (uint32_t term = 0; term < 3; ++term)
                {
                    const Vec3 difference  = Vec3(values[pixel + 7 + term] - m_previousLighting[pixel + 7 + term]);
                    rms[term]             += glm::dot(difference, difference);
                }
            }
        }
        bool converged = count > 0;
        for (uint32_t term = 0; term < 3; ++term)
        {
            rms[term] = count == 0 ? 0 : std::sqrt(rms[term] / (3.0 * count));
            converged = converged && std::isfinite(rms[term]) && rms[term] <= std::max(1e-7, frame.noiseRms[term]);
        }
        return converged;
    }

    // Only the converged capture pair is published; earlier stability checks stay in memory.
    bool WriteBatchFrame(const GroundTruthFrame& frame, const HeapVector<Vec4>& lighting, const double (&rms)[3])
    {
        const std::string prefix = m_options.output + "/" + frame.id;
        HeapVector<Vec4>  hybrid;
        bool valid = ReadCaptureBuffer(3, hybrid) && WriteCaptureFile(prefix + ".lighting.bin", lighting)
                  && WriteCaptureFile(prefix + ".hybrid.bin", hybrid)
                  && WriteCaptureFile(prefix + ".previous.lighting.bin", m_previousLighting)
                  && WriteFrameSummary(prefix + ".json")
                  && CaptureGroundTruthEnvironment(*m_device, m_rendering->GetScene()->GetEnvTexture().pPrefiltered,
                                                   prefix + ".environment.bin")
                  && WriteRuntimeLaunchSettings(std::filesystem::u8path(prefix + ".zenrender"), m_setup, m_error);
        if (valid)
        {
            std::ofstream convergence(std::filesystem::u8path(prefix + ".convergence.json"));
            convergence << "{\"settledFrames\":" << GetSceneFrame() - m_batchStart << ",\"rms\":[" << rms[0] << ',' << rms[1]
                        << ',' << rms[2] << "]}\n";
            convergence.close();
            valid = convergence.good();
        }
        if (!valid && m_error.empty())
        {
            m_error = "Could not write the ground-truth capture of " + frame.id + " to " + m_options.output;
        }
        return valid;
    }

    bool StartBatchFrame(rc::DeferredLightingRenderer& renderer)
    {
        m_setup = m_options.batch[m_batchIndex].setup;
        // Reloading an unchanged environment would drain the GPU and rebuild its sampling data.
        const bool sameEnvironment = m_setup.rendering.environment.texturePath == m_rendering->GetEnvironment().texturePath;
        const bool valid =
            m_camera.SetState(m_setup.camera)
            && (sameEnvironment || m_rendering->SetEnvironmentTexture(m_setup.rendering.environment.texturePath, m_error))
            && m_rendering->ApplyRenderingSettings(m_setup.rendering, false, m_error);
        if (!valid && m_error.empty())
        {
            m_error = "Could not apply the ground-truth setup of " + m_options.batch[m_batchIndex].id;
        }
        renderer.GetHybridGI()->InvalidateHistory("ground_truth_frame");
        m_batchStart = GetSceneFrame();
        m_batchBegin = GetSceneFrame();
        m_previousLighting.clear();
        return valid;
    }

    bool FinishBatchCapture()
    {
        m_device->WaitForPreviousFrames();
        rc::DeferredLightingRenderer& renderer = *m_device->GetRendererServer()->RequestDeferredLightingRenderer();
        const GroundTruthFrame&       frame    = m_options.batch[m_batchIndex];
        const char*                   failure  = BatchCaptureFailure(renderer);
        HeapVector<Vec4>              values;
        double                        rms[3]{};
        bool                          valid = failure == nullptr && ReadCaptureBuffer(1, values);
        if (!valid)
        {
            m_error = "Ground-truth capture of " + frame.id
                    + " failed: " + (failure != nullptr ? failure : "the lighting readback could not be mapped");
        }
        else if (BatchCaptureConverged(values, frame, rms))
        {
            valid = WriteBatchFrame(frame, values, rms);
            ++m_batchIndex;
            m_batchDone = m_batchIndex == m_options.batch.size();
            valid       = valid && (m_batchDone || StartBatchFrame(renderer));
        }
        else
        {
            valid              = GetSceneFrame() - m_batchStart < std::max(uint64_t(512), m_options.frames * 4);
            m_previousLighting = std::move(values);
            if (!valid)
            {
                m_error = "Ground-truth GI did not converge below the reference noise: " + frame.id;
            }
        }
        renderer.SetLightingCapture(nullptr, nullptr);
        renderer.SetHybridCapture(nullptr, nullptr);
        return valid;
    }

    bool RenderIteration()
    {
        platform::NativeWindow& window = *m_session->GetWindow();
        // Submission failures remain observable while paused/minimized.
        bool                         valid  = m_device->PollFrameSubmissions();
        const platform::WindowExtent extent = window.GetFramebufferExtent();
        if (valid && !m_session->IsClosing())
        {
            if (!window.IsMinimized() && extent.width > 0 && extent.height > 0)
            {
                DrawUI();
            }
            valid = Frame();
        }
        return valid;
    }

    uint64_t GetSceneFrame() const
    {
        return m_session ? m_session->GetSceneFrame() : 0;
    }

    double GetSceneSeconds() const
    {
        return m_session ? m_session->GetSceneSeconds() : 0;
    }

    void DrawUI()
    {
#if defined(ZEN_RUNTIME_DIAGNOSTICS_UI)
        if (!m_options.noUI)
        {
            const HeapVector<RuntimeLogEntry> logs    = m_log->Snapshot();
            const std::string&                source  = m_setup.sourcePath.empty() ? m_setup.packagePath : m_setup.sourcePath;
            const uint64_t                    dropped = m_log->GetDropped();
            if (m_overlay.Draw(*m_session, logs, source, m_options.logPath, false, dropped))
            {
                m_log->Clear();
            }
        }
#endif
    }

    RuntimeExit Initialize()
    {
        if (m_options.rhiOverride)
        {
            LOGI("Command-line override: RHI mode {}",
                 m_setup.run.rhiMode == RHIExecutionMode::eThreaded ? "threaded" : "inline");
        }
        RuntimeExit result = m_logReady ? RuntimeExit::Package : RuntimeExit::Setup;
        bool        valid  = m_logReady && (m_options.batch.empty() || !IsRTSSLoaded());
        if (m_logReady && !valid)
        {
            m_error = "Ground-truth capture refuses to run while RTSS is loaded";
        }
        if (valid)
        {
            asset::SceneImportRequest request{m_setup.packagePath};
            request.retainCanonicalAsset  = false;
            asset::SceneLoadResult loaded = asset::LoadScene(request);
            m_error                       = loaded.error;
            m_loaded                      = std::move(loaded.scene);
            valid                         = m_loaded && m_loaded->scene->GetRenderableCount() > 0;
            if (valid)
            {
                LOGI("ZenApp scene: vertices={}, indices={}, renderable_nodes={}, submitted_lights={}",
                     m_loaded->vertices.size(), m_loaded->indices.size(), m_loaded->scene->GetRenderableCount(),
                     m_loaded->scene->GetComponents<sg::Light>().size());
            }
        }
        if (m_loaded && !valid)
        {
            m_error = "The prepared package has no renderable scene";
        }
        if (valid)
        {
            result = RuntimeExit::Renderer;
            RHIOptions::GetInstance().SetValidationEnabled(m_setup.run.validation);
            RHIOptions::GetInstance().SetGPUMemoryStats(true);
            RHIOptions::GetInstance().SetGPUProfilerMarkers(m_options.gpuMarkers);
            RHIOptions::GetInstance().SetRayTracingDisabled(m_options.disableRT);
            RHIOptions::GetInstance().SetPresentMode(m_setup.run.presentMode);
            rc::RenderConfig& config = rc::RenderConfig::GetInstance();
            config.numFrames         = m_setup.run.framesInFlight;
            config.numThreads        = m_setup.run.workerThreads;
            config.depthBiasConstant = m_setup.run.shadowDepthBias;
            config.depthBiasSlope    = m_setup.run.shadowSlopeBias;
            RuntimeWindowSettings settings;
            settings.windowWidth                     = m_setup.run.windowWidth;
            settings.windowHeight                    = m_setup.run.windowHeight;
            settings.renderWidth                     = m_setup.run.renderWidth;
            settings.renderHeight                    = m_setup.run.renderHeight;
            settings.fullscreen                      = m_setup.run.fullscreen;
            settings.followWindow                    = m_setup.run.followWindow;
            settings.vsync                           = m_setup.run.vsync;
            settings.fixedStep                       = m_options.fixedStep;
            UniquePtr<platform::NativeWindow> window = RuntimeWindow::CreateNativeWindow(settings);
            m_device = MakeUnique<rc::RenderDevice>(RHIAPIType::eVulkan, m_setup.run.framesInFlight, m_setup.run.rhiMode,
                                                    m_setup.rendering.gi.asyncCompute);
            rc::ShaderProgramManager::GetInstance().BuildShaderPrograms(m_device.Get());
            m_device->Init(nullptr);
            m_device->InitializeRendererServer(&m_setup.rendering);
            m_device->GetRendererServer()->RequestDeferredLightingRenderer()->SetGPUCullingEnabled(m_options.gpuCulling);
            m_rendering = MakeUnique<rc::SceneRendering>(*m_device);
            m_rendering->InitializeEnvironment(m_setup.defaultEnvironment, m_setup.rendering.environment);
            valid = valid && m_camera.SetState(m_setup.camera)
                 && m_rendering->PrepareScene(*m_loaded, m_camera.GetCamera(), m_error) && m_rendering->CommitScene(m_error)
                 && m_rendering->ApplyRenderingSettings(m_setup.rendering, true, m_error);
            if (valid)
            {
                const bool animated = !m_loaded->scene->GetAssetData().animations.empty();
                valid               = !m_setup.animationEnabled || !animated
                                   || m_rendering->GetScene()->SetAnimation(m_setup.animationClip, m_setup.animationLoop);

                rc::RDGMetricsOptions metrics;
                metrics.gpuTimings  = m_setup.run.gpuTimings || !m_options.profile.empty();
                metrics.nodeTimings = metrics.gpuTimings;
                if (!m_options.profile.empty())
                {
                    m_profile.open(std::filesystem::u8path(m_options.profile + ".rdg.log"), std::ios::out | std::ios::trunc);
                    valid                       = valid && m_profile.good();
                    metrics.logging.sampleEvery = 1;
                    metrics.logging.minInterval = std::chrono::milliseconds(0);
                    m_device->GetRDGMetrics().SetGPUSink(
                        [this](const rc::RDGMetricsSnapshot& sample) { WriteMetrics(sample); });
                }
                m_device->GetRDGMetrics().Configure(metrics);
                m_target         = MakeUnique<rc::SceneRenderTarget>(*m_device);
                m_session        = MakeUnique<RuntimeWindow>(*m_device, *m_rendering, *m_target, m_camera, std::move(window));
                settings.animate = m_setup.animationEnabled && animated;
                valid            = valid && m_session->Start(settings, m_setup.run.cameraMoveSpeed);
                if (valid)
                {
                    m_session->Update();
                    valid = m_session->GetWindow() != nullptr && !m_session->IsClosing();
                }
            }
            if (valid)
            {
#if defined(ZEN_RUNTIME_DIAGNOSTICS_UI)
                valid = m_options.noUI || m_overlay.Initialize(*m_device, *m_session->GetWindow());
#endif
            }
            if (valid)
            {
                valid = m_options.batch.empty() || CreateCaptureBuffers();
            }
            if (valid)
            {
                LOGI("ZenApp loaded {}. Space: Pause/Resume; Escape: Quit; F1: diagnostics.", m_setup.packagePath);
                result = RuntimeExit::Success;
            }
            else if (m_error.empty())
            {
                m_error = "Runtime graphics, animation, profile output or scene target initialization failed";
            }
        }
        return result;
    }

    bool Frame()
    {
        const std::chrono::steady_clock::time_point frameStart    = std::chrono::steady_clock::now();
        const uint64_t                              previousFrame = GetSceneFrame();
        rc::RenderOverlay*                          overlay       = nullptr;
        bool                                        captureTerms  = false;
        if (!m_options.batch.empty())
        {
            m_session->SetInputCapture(true, true);
            const rc::RendererServer& server = *m_device->GetRendererServer();
            if (server.IsPreparingGI() || server.IsRefiningGI())
            {
                m_batchStart = previousFrame;
                m_previousLighting.clear();
            }
            const uint64_t frame = previousFrame + 1 - m_batchStart;
            captureTerms         = frame >= m_options.frames - 16 && (frame - (m_options.frames - 16)) % 16 == 0;
            if (captureTerms)
            {
                rc::DeferredLightingRenderer& renderer = *m_device->GetRendererServer()->RequestDeferredLightingRenderer();
                renderer.SetLightingCapture(m_captureBuffers[0], m_captureBuffers[1]);
                renderer.SetHybridCapture(m_captureBuffers[2], m_captureBuffers[3]);
            }
        }
#if defined(ZEN_RUNTIME_DIAGNOSTICS_UI)
        overlay = m_options.noUI ? nullptr : &m_overlay;
#endif
        bool valid = (m_options.cameraKeys.empty() || ApplyCameraPath(previousFrame + 1))
                  && (m_options.lightKeys.keys.empty() || ApplyLightPath(previousFrame + 1)) && m_session->Render(overlay);
        if (!m_options.batch.empty() && GetSceneFrame() - m_batchBegin > 4096)
        {
            // Preparation, refinement and convergence share this budget.
            m_error = "Ground-truth frame " + m_options.batch[m_batchIndex].id + " did not settle within 4096 frames";
            valid   = false;
        }
        if (valid && GetSceneFrame() > previousFrame)
        {
            if (captureTerms)
            {
                valid = !IsRTSSLoaded();
                if (!valid)
                {
                    m_error = "Ground-truth capture refuses to run while RTSS is loaded";
                }
                valid = valid && FinishBatchCapture();
            }
            const rc::SceneCullingResult& culling = m_device->GetRendererServer()->GetRenderingStatus().gpuCulling;
            if (!m_cullingReported || culling.status != m_lastSceneCulling.status)
            {
                LOGI("ZenApp culling: requested={}, path={}, reason={}", m_options.gpuCulling,
                     rc::SceneCullingStatusName(culling.status), rc::SceneCullingFallbackReason(culling.status));
                m_cullingReported = true;
            }
            m_lastSceneCulling = culling;
            if (!m_options.profile.empty())
            {
                const rc::RendererServer& server = *m_device->GetRendererServer();
                const RHIGPUMemoryStats   memory = m_device->GetGPUMemoryStats();
                const double              frameMilliseconds =
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - frameStart).count();
                m_profile << "scene_frame=" << GetSceneFrame() << " gpu_culling_requested=" << m_options.gpuCulling
                          << " gpu_culling_path=" << rc::SceneCullingStatusName(culling.status)
                          << " candidates=" << culling.candidateCount << " uploaded_bytes=" << culling.uploadedBytes
                          << " fallback=" << std::quoted(rc::SceneCullingFallbackReason(culling.status))
                          << " frame_cpu_ms=" << frameMilliseconds << " gi_preparing=" << server.IsPreparingGI()
                          << " gi_refining=" << server.IsRefiningGI() << " gpu_allocated_bytes=" << memory.deviceLocalBytes
                          << " gpu_peak_allocated_bytes=" << memory.peakDeviceLocalBytes << '\n';
            }
            if (GetSceneFrame() == 1)
            {
                const rc::RenderView view = m_target->GetView();
                LOGI("ZenApp render: {}x{}, async-compute={}, RHI={}, ray-provider={}, ray-query={}, diagnostics={}",
                     view.width, view.height, rc::GetAsyncComputeStatusReason(m_device->GetAsyncComputeStatus()),
                     m_setup.run.rhiMode == RHIExecutionMode::eThreaded ? "threaded" : "inline",
                     GetRayProviderName(m_setup.rendering.gi.cone.rayProvider),
                     m_device->GetGPUInfo().rayQuery.IsUsable() ? "available" : "unavailable", overlay != nullptr);
                const rc::RendererServer&   server = *m_device->GetRendererServer();
                const rc::HybridGIRenderer* hybrid = server.RequestDeferredLightingRenderer()->GetHybridGI();
                const rc::VoxelGIRenderer*  gi     = server.RequestVoxelGI();
                LOGI("ZenApp GI: gather={}, hardware-mask={}, field-traversal={}, voxel-lighting={}",
                     hybrid != nullptr && hybrid->IsHistoryValid() ? (hybrid->UsesScreenProbes() ? "probes" : "pixel")
                                                                   : "inactive",
                     gi != nullptr ? gi->GetHardwareFeatureMask() : 0u, gi != nullptr && gi->UsesFieldTraversal(),
                     gi != nullptr && gi->UsesVoxelLighting());
                const glm::uvec2 giExtent = hybrid != nullptr ? hybrid->GetGIExtent() : glm::uvec2(0);
                LOGI("ZenApp GI extent: {}x{}, output={}x{}", giExtent.x, giExtent.y, view.width, view.height);
                m_device->FlushRHIThread();
                valid = m_device->PollFrameSubmissions();
                if (valid && m_options.startPaused)
                {
                    Pause(true);
                }
            }
            const bool capture = !m_options.capture.empty()
                              && (GetSceneFrame() == m_options.frames || (m_options.startPaused && GetSceneFrame() == 1)
                                  || (m_options.captureStart != 0 && GetSceneFrame() >= m_options.captureStart));
            if (valid && capture)
            {
                const std::string path = m_options.captureStart == 0
                                           ? m_options.capture
                                           : m_options.capture + ".frame-" + std::to_string(GetSceneFrame()) + ".ppm";
                valid                  = rc::CaptureSceneFrame(*m_device, m_target->GetView().color, path);
                valid                  = valid && WriteFrameSummary(path + ".json");
            }
        }
        if (!valid)
        {
            if (m_error.empty())
            {
                m_error = "Runtime scene rendering or capture failed";
            }
        }
        return valid;
    }

    void Pause(bool paused)
    {
        if (m_session && paused != (m_session->GetState() == RuntimeState::Paused))
        {
            const bool changed = paused ? m_session->Pause() : m_session->Resume();
            if (changed)
            {
                LOGI("ZenApp {} at scene frame {}, time {:.3f}s", paused ? "Paused" : "Running", GetSceneFrame(),
                     GetSceneSeconds());
            }
        }
    }

    void WriteMetrics(const rc::RDGMetricsSnapshot& sample)
    {
        m_profile << rc::RDGMetrics::Format(sample) << '\n';
    }

    bool WriteFrameSummary(const std::string& path)
    {
        const rc::RenderView          view   = m_target ? m_target->GetView() : rc::RenderView{};
        const platform::NativeWindow* window = m_session ? m_session->GetWindow() : nullptr;
        const platform::WindowExtent  framebuffer =
            window != nullptr ? window->GetFramebufferExtent() : platform::WindowExtent{};
        // UI-only or paused submissions must not replace the last scene frame's path.
        const rc::SceneCullingResult& culling  = m_lastSceneCulling;
        const rc::RendererServer&     server   = *m_device->GetRendererServer();
        const rc::HybridGIRenderer*   hybrid   = server.RequestDeferredLightingRenderer()->GetHybridGI();
        const rc::VoxelGIRenderer*    gi       = server.RequestVoxelGI();
        const glm::uvec2              giExtent = hybrid != nullptr ? hybrid->GetGIExtent() : glm::uvec2(0);
        std::ofstream                 summary(std::filesystem::u8path(path));
        summary << "{\"sceneFrames\":" << GetSceneFrame() << ",\"sceneSeconds\":" << GetSceneSeconds()
                << ",\"width\":" << view.width << ",\"height\":" << view.height << ",\"framebufferWidth\":" << framebuffer.width
                << ",\"framebufferHeight\":" << framebuffer.height
                << ",\"fullscreen\":" << (window != nullptr && window->IsFullscreen() ? "true" : "false")
                << ",\"backend\":" << std::quoted(GetRayProviderName(m_setup.rendering.gi.cone.rayProvider)) << ",\"gather\":"
                << std::quoted(hybrid != nullptr && hybrid->IsHistoryValid() ? (hybrid->UsesScreenProbes() ? "probes" : "pixel")
                                                                             : "inactive")
                << ",\"giWidth\":" << giExtent.x << ",\"giHeight\":" << giExtent.y
                << ",\"hardwareFeatureMask\":" << (gi != nullptr ? gi->GetHardwareFeatureMask() : 0u)
                << ",\"fieldTraversal\":" << (gi != nullptr && gi->UsesFieldTraversal() ? "true" : "false")
                << ",\"voxelLighting\":" << (gi != nullptr && gi->UsesVoxelLighting() ? "true" : "false")
                << ",\"asyncCompute\":" << std::quoted(rc::GetAsyncComputeStatusReason(m_device->GetAsyncComputeStatus()))
                << ",\"lightingComponents\":" << ZEN_LIGHTING_CAPTURE_COMPONENTS
                << ",\"hybridComponents\":" << ZEN_HYBRID_CAPTURE_COMPONENTS
                << ",\"environmentCubeSize\":" << m_rendering->GetScene()->GetEnvTexture().pPrefiltered->GetWidth()
                << ",\"gpuCullingRequested\":" << (m_options.gpuCulling ? "true" : "false")
                << ",\"gpuCullingPath\":" << std::quoted(rc::SceneCullingStatusName(culling.status))
                << ",\"gpuCullingFallbackReason\":" << std::quoted(rc::SceneCullingFallbackReason(culling.status))
                << ",\"gpuCullingCandidates\":" << culling.candidateCount
                << ",\"gpuCullingUploadedBytes\":" << culling.uploadedBytes << "}\n";
        summary.close();
        return summary.good();
    }

    bool WriteProfileSummary()
    {
        return WriteFrameSummary(m_options.profile + ".summary.json")
            && WriteRuntimeLaunchSettings(std::filesystem::u8path(m_options.profile + ".setup.zenrender"), m_setup, m_error);
    }

    RuntimeLaunchSettings            m_setup;
    const RuntimeOptions&            m_options;
    UniquePtr<rc::RenderDevice>      m_device;
    UniquePtr<asset::LoadedScene>    m_loaded;
    UniquePtr<rc::SceneRendering>    m_rendering;
    UniquePtr<rc::SceneRenderTarget> m_target;
    UniquePtr<RuntimeWindow>         m_session;
    NavigationCamera                 m_camera;
    std::string                      m_error;
    std::ofstream                    m_profile;
    std::shared_ptr<RuntimeLog>      m_log;
    bool                             m_logReady{false};
    bool                             m_cullingReported{false};
    rc::SceneCullingResult           m_lastSceneCulling;
    RHIBuffer*                       m_captureBuffers[4]{};
    HeapVector<Vec4>                 m_previousLighting;
    size_t                           m_batchIndex{0};
    uint64_t                         m_batchStart{0};
    uint64_t                         m_batchBegin{0};
    bool                             m_batchDone{false};
#if defined(ZEN_RUNTIME_DIAGNOSTICS_UI)
    RuntimeOverlay m_overlay;
#endif
};
} // namespace

RuntimeExit RunApplication(const RuntimeLaunchSettings& setup, const RuntimeOptions& options)
{
    RuntimeApplication application(setup, options);
    return application.Run();
}
} // namespace zen::runtime
