#include "RuntimeApplication.h"
#include "RuntimeLog.h"
#include "AssetLib/SceneLoader.h"
#include "Graphics/RenderCore/V2/RenderDevice.h"
#include "Graphics/RenderCore/V2/Renderer/RendererServer.h"
#include "Graphics/RenderCore/V2/ScenePresentation.h"
#include "Graphics/RenderCore/V2/SceneRendering.h"
#include "Graphics/RenderCore/V2/ShaderProgram.h"
#include "Platform/NativeWindow.h"
#include "ZenApp/RuntimeWindow.h"
#include <fstream>
#include <algorithm>
#if defined(ZEN_RUNTIME_DIAGNOSTICS_UI)
#    include "ZenApp/RuntimeOverlay.h"
#endif

namespace zen::runtime
{
namespace
{
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
            if (valid && m_session && !m_session->IsClosing() && (m_options.frames == 0 || GetSceneFrame() < m_options.frames))
            {
                valid = RenderIteration();
            }
        };
        while (valid && m_session && !m_session->IsClosing() && (m_options.frames == 0 || GetSceneFrame() < m_options.frames))
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
        if (result == RuntimeExit::Success && !valid)
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
        const HeapVector<RuntimeLogEntry> logs    = m_log->Snapshot();
        const std::string&                source  = m_setup.sourcePath.empty() ? m_setup.packagePath : m_setup.sourcePath;
        const uint64_t                    dropped = m_log->GetDropped();
        if (m_overlay.Draw(*m_session, logs, source, m_options.logPath, false, dropped))
        {
            m_log->Clear();
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
        bool        valid  = m_logReady;
        if (valid)
        {
            asset::SceneImportRequest request{m_setup.packagePath};
            asset::SceneLoadResult    loaded = asset::LoadScene(request);
            m_error                          = loaded.error;
            m_loaded                         = std::move(loaded.scene);
            valid                            = m_loaded && m_loaded->scene->GetRenderableCount() > 0;
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
            settings.followWindow                    = m_setup.run.followWindow;
            settings.vsync                           = m_setup.run.vsync;
            settings.fixedStep                       = m_options.fixedStep;
            UniquePtr<platform::NativeWindow> window = RuntimeWindow::CreateNativeWindow(settings);
            m_device = MakeUnique<rc::RenderDevice>(RHIAPIType::eVulkan, m_setup.run.framesInFlight, m_setup.run.rhiMode,
                                                    m_setup.rendering.gi.asyncCompute);
            rc::ShaderProgramManager::GetInstance().BuildShaderPrograms(m_device.Get());
            m_device->Init(nullptr);
            m_device->InitializeRendererServer(&m_setup.rendering);
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
                valid = m_overlay.Initialize(*m_device, *m_session->GetWindow());
#endif
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
        const uint64_t     previousFrame = GetSceneFrame();
        rc::RenderOverlay* overlay       = nullptr;
#if defined(ZEN_RUNTIME_DIAGNOSTICS_UI)
        overlay = &m_overlay;
#endif
        bool valid = m_session->Render(overlay);
        if (valid && GetSceneFrame() > previousFrame)
        {
            if (GetSceneFrame() == 1)
            {
                m_device->FlushRHIThread();
                valid = m_device->PollFrameSubmissions();
                if (valid && m_options.startPaused)
                {
                    Pause(true);
                }
            }
            const bool capture = !m_options.capture.empty()
                              && (GetSceneFrame() == m_options.frames || (m_options.startPaused && GetSceneFrame() == 1));
            if (valid && capture)
            {
                valid = rc::CaptureSceneFrame(*m_device, m_target->GetView().color, m_options.capture);
            }
        }
        if (!valid)
        {
            m_error = "Runtime scene rendering or capture failed";
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

    bool WriteProfileSummary()
    {
        const rc::RenderView view = m_target ? m_target->GetView() : rc::RenderView{};
        std::ofstream        summary(std::filesystem::u8path(m_options.profile + ".summary.json"));
        summary << "{\"sceneFrames\":" << GetSceneFrame() << ",\"sceneSeconds\":" << GetSceneSeconds()
                << ",\"width\":" << view.width << ",\"height\":" << view.height << "}\n";
        summary.close();
        return summary.good();
    }

    const RuntimeLaunchSettings&     m_setup;
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
