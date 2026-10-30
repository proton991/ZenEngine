#include "ZenApp/RuntimeWindow.h"
#include "Graphics/RenderCore/V2/RenderDevice.h"
#include "Graphics/RenderCore/V2/Renderer/RendererServer.h"
#include "Platform/BuildInfo.h"
#include "Platform/InputController.h"

namespace zen::runtime
{
RuntimeWindow::RuntimeWindow(rc::RenderDevice&                 device,
                             rc::SceneRendering&               scene,
                             rc::SceneRenderTarget&            target,
                             NavigationCamera&                 camera,
                             UniquePtr<platform::NativeWindow> window) :
    m_device(device), m_scene(scene), m_target(target), m_camera(camera), m_window(std::move(window))
{}

UniquePtr<platform::NativeWindow> RuntimeWindow::CreateNativeWindow(const RuntimeWindowSettings& settings)
{
    const platform::WindowConfig config{
        platform::BuildApplicationTitle("ZenApp"), true, settings.windowWidth, settings.windowHeight, 0, false};
    return MakeUnique<platform::NativeWindow>(config);
}

RuntimeWindow::~RuntimeWindow()
{
    Shutdown();
}

bool RuntimeWindow::IsActive() const
{
    return m_state != runtime::RuntimeState::Idle && m_state != runtime::RuntimeState::Failed;
}

bool RuntimeWindow::IsClosing() const
{
    return m_state == runtime::RuntimeState::Stopping || (m_window && m_window->ShouldClose());
}

bool RuntimeWindow::Start(const RuntimeWindowSettings& settings, float moveSpeed)
{
    const bool valid = !IsActive() && (m_scene.GetScene() != nullptr) && settings.windowWidth > 0 && settings.windowHeight > 0
                    && settings.windowWidth <= 8192 && settings.windowHeight <= 8192 && settings.renderWidth > 0
                    && settings.renderHeight > 0 && settings.renderWidth <= 8192 && settings.renderHeight <= 8192;
    if (valid)
    {
        m_settings  = settings;
        m_moveSpeed = runtime::ClampNavigationCameraMoveSpeed(moveSpeed);
        m_error.clear();
        m_clock             = {};
        m_sceneMilliseconds = 0;
        m_sceneMetrics      = {};
        m_sceneFrames       = 0;
        m_declines          = 0;
        m_wheel             = 0;
        m_captureMouse      = false;
        m_captureKeyboard   = false;
        m_state             = runtime::RuntimeState::Starting;
    }
    return valid;
}

bool RuntimeWindow::Pause()
{
    const bool valid = m_state == runtime::RuntimeState::Running;
    if (valid)
    {
        m_device.WaitForPreviousFrames();
        m_clock.SetPaused(true, std::chrono::steady_clock::now());
        m_state = runtime::RuntimeState::Paused;
        platform::KeyboardMouseInput::GetInstance().Reset();
        m_wheel = 0;
    }
    return valid;
}

bool RuntimeWindow::Resume()
{
    const bool valid = m_state == runtime::RuntimeState::Paused;
    if (valid)
    {
        m_clock.SetPaused(false, std::chrono::steady_clock::now());
        m_state         = runtime::RuntimeState::Running;
        m_lastSceneTime = std::chrono::steady_clock::now();
        platform::KeyboardMouseInput::GetInstance().Reset();
        m_wheel = 0;
    }
    return valid;
}

bool RuntimeWindow::Quit()
{
    const bool valid = IsActive();
    if (valid)
    {
        m_state = runtime::RuntimeState::Stopping;
    }
    return valid;
}

void RuntimeWindow::Fail(std::string error)
{
    m_error = std::move(error);
    LOGE("Play window: {}", m_error);
    m_state = runtime::RuntimeState::Stopping;
}

void RuntimeWindow::Update()
{
    if (IsClosing())
    {
        Shutdown();
    }
    if (m_state == runtime::RuntimeState::Starting && m_present == nullptr)
    {
        if (!m_window)
        {
            m_window = CreateNativeWindow(m_settings);
        }
        m_window->SetOnInput([this](const platform::InputEvent& event) { OnInput(event); });
        const platform::WindowExtent extent = m_window->GetFramebufferExtent();
        m_present = m_device.CreateViewport(m_window.Get(), extent.width, extent.height, m_settings.vsync);
        if (m_present != nullptr && m_compositor.Init(m_device))
        {
            m_clock.ResetWallTime(std::chrono::steady_clock::now());
            m_window->Show();
            m_window->Focus();
            platform::KeyboardMouseInput::GetInstance().Reset();
            LOGI("ZenApp window opened using the supplied scene and render device");
        }
        else
        {
            Fail("Could not initialize the ZenApp window.");
        }
    }
}

void RuntimeWindow::Shutdown()
{
    if (m_window)
    {
        m_device.DestroyViewport(m_present);
        m_present = nullptr;
        m_window.Reset();
        platform::KeyboardMouseInput::GetInstance().Reset();
        LOGI("ZenApp window closed; the borrowed scene remains with its host");
    }
    m_overlay = nullptr;
    m_wheel   = 0;
    m_state   = m_error.empty() ? runtime::RuntimeState::Idle : runtime::RuntimeState::Failed;
}

void RuntimeWindow::SetInputCapture(bool mouse, bool keyboard)
{
    m_captureMouse    = mouse;
    m_captureKeyboard = keyboard;
}

void RuntimeWindow::OnInput(const platform::InputEvent& event)
{
    if (event.type == platform::InputEventType::Wheel && !m_captureMouse && m_state == runtime::RuntimeState::Running)
    {
        m_wheel += event.deltaY;
    }
    if (event.type == platform::InputEventType::FocusLost)
    {
        m_wheel = 0;
    }
}

void RuntimeWindow::ApplyInput(float seconds)
{
    // Consume motion while UI owns input so releasing a widget cannot jump the camera.
    const CameraInput input = ReadNavigationCameraInput(seconds, m_moveSpeed, m_wheel);
    if (m_window->IsFocused() && !m_captureMouse && !m_captureKeyboard)
    {
        m_camera.Apply(input);
    }
    m_wheel = 0;
}

void RuntimeWindow::ProcessShortcuts()
{
    platform::KeyboardMouseInput& keys = platform::KeyboardMouseInput::GetInstance();
    if (keys.WasKeyPressedOnce(platform::Key::Escape))
    {
        Quit();
    }
    if (keys.WasKeyPressedOnce(platform::Key::F1))
    {
        m_showDiagnostics = !m_showDiagnostics;
    }
    // Consume captured presses so typing cannot pause later when the UI releases input.
    if (keys.WasKeyPressedOnce(platform::Key::Space) && !m_captureKeyboard)
    {
        if (m_state == runtime::RuntimeState::Paused)
        {
            Resume();
        }
        else
        {
            Pause();
        }
    }
}

bool RuntimeWindow::Render(rc::RenderOverlay* overlay)
{
    bool valid = true;
    if (m_window && m_present && !IsClosing())
    {
        if (m_window->IsFocused())
        {
            ProcessShortcuts();
        }
        const platform::WindowExtent extent = m_window->GetFramebufferExtent();
        if (!IsClosing() && !m_window->IsMinimized() && extent.width > 0 && extent.height > 0)
        {
            m_device.ResizeViewport(m_present, extent.width, extent.height);
            const bool drawScene = m_state != runtime::RuntimeState::Paused;
            if (drawScene)
            {
                const uint32_t width  = m_settings.followWindow ? extent.width : m_settings.renderWidth;
                const uint32_t height = m_settings.followWindow ? extent.height : m_settings.renderHeight;
                valid                 = m_target.Resize(width, height);
                if (valid)
                {
                    m_camera.SetExtent(width, height);
                    const float seconds =
                        m_sceneFrames == 0 ? 0.0f : m_clock.Advance(std::chrono::steady_clock::now(), m_settings.fixedStep);
                    ApplyInput(seconds);
                    const Mat4 previousView       = m_camera.GetCamera().GetViewMatrix();
                    const Mat4 previousProjection = m_camera.GetCamera().GetProjectionMatrix();
                    valid                         = (!m_settings.animate || m_scene.GetScene()->AdvanceAnimation(seconds));
                    if (valid
                        && (previousView != m_camera.GetCamera().GetViewMatrix()
                            || previousProjection != m_camera.GetCamera().GetProjectionMatrix()))
                    {
                        m_camera.SynchronizeFromCamera();
                    }
                }
            }
            if (valid)
            {
                m_overlay = overlay;
                const rc::RenderFrameResult result =
                    m_device.GetRendererServer()->DispatchRenderWorkloads(m_target.GetView(), this, drawScene, m_present);
                m_overlay = nullptr;
                valid     = result != rc::RenderFrameResult::eBlocked && result != rc::RenderFrameResult::eFailed
                         && rc::TrackDeclinedFrames(result, m_declines);
                if (drawScene && result == rc::RenderFrameResult::eSubmitted)
                {
                    const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
                    if (m_sceneFrames > 0)
                    {
                        const float elapsed = std::chrono::duration<float, std::milli>(now - m_lastSceneTime).count();
                        m_sceneMilliseconds = m_sceneMilliseconds == 0 ? elapsed : m_sceneMilliseconds * 0.9f + elapsed * 0.1f;
                    }
                    m_lastSceneTime = now;
                    m_sceneMetrics  = m_device.GetRDGMetrics().GetLastSnapshot();
                    m_target.OnSubmitted(true);
                    ++m_sceneFrames;
                    if (m_state == runtime::RuntimeState::Starting)
                    {
                        m_state = runtime::RuntimeState::Running;
                        m_clock.ResetWallTime(std::chrono::steady_clock::now());
                    }
                }
                m_device.NextFrame();
            }
            if (!valid)
            {
                Fail("ZenApp rendering failed.");
            }
        }
        else
        {
            m_clock.ResetWallTime(std::chrono::steady_clock::now());
            m_lastSceneTime = std::chrono::steady_clock::now();
        }
    }
    return valid;
}

bool RuntimeWindow::BuildRenderGraph(rc::RenderGraph& graph, RHIViewport& viewport)
{
    return m_compositor.BuildRenderGraph(graph, rc::RenderView::FromViewport(viewport), m_target.GetView())
        && (m_overlay == nullptr || m_overlay->BuildRenderGraph(graph, viewport));
}
} // namespace zen::runtime
