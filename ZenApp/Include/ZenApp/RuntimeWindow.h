#pragma once
#include "Graphics/RenderCore/V2/SceneRendering.h"
#include "Runtime/NavigationCamera.h"
#include "Graphics/RenderCore/V2/ScenePresentation.h"
#include "Graphics/RenderCore/V2/Renderer/RenderOverlay.h"
#include "Platform/NativeWindow.h"
#include "Runtime/SceneClock.h"
#include "Graphics/RenderCore/V2/RenderGraph/RDGMetrics.h"
#include "Runtime/RuntimeState.h"

namespace zen::runtime
{
struct RuntimeWindowSettings
{
    uint32_t windowWidth{1440};
    uint32_t windowHeight{900};
    uint32_t renderWidth{1440};
    uint32_t renderHeight{900};
    bool     followWindow{true};
    bool     vsync{true};
    bool     fixedStep{false};
    bool     animate{true};
};

// Borrows a loaded scene, camera and render target. Owns the ZenApp window only.
// The frontend must destroy its play-window UI before Update closes the window.
class RuntimeWindow final : public rc::RenderOverlay
{
public:
    RuntimeWindow(rc::RenderDevice&                 device,
                  rc::SceneRendering&               scene,
                  rc::SceneRenderTarget&            target,
                  NavigationCamera&                 camera,
                  UniquePtr<platform::NativeWindow> window = {});
    ~RuntimeWindow();
    // Standalone hosts create this before RenderDevice so Vulkan can discover surface extensions.
    static UniquePtr<platform::NativeWindow> CreateNativeWindow(const RuntimeWindowSettings& settings);
    bool                                     Start(const RuntimeWindowSettings& settings, float moveSpeed);
    bool                                     Pause();
    bool                                     Resume();
    bool                                     Quit();
    void                                     Update();
    void                                     Shutdown();
    bool                                     Render(rc::RenderOverlay* overlay = nullptr);
    bool                                     BuildRenderGraph(rc::RenderGraph& graph, RHIViewport& viewport) override;
    bool                                     IsActive() const;
    bool                                     IsClosing() const;
    runtime::RuntimeState                    GetState() const
    {
        return m_state;
    }
    const std::string& GetError() const
    {
        return m_error;
    }
    platform::NativeWindow* GetWindow() const
    {
        return m_window.Get();
    }
    uint64_t GetSceneFrame() const
    {
        return m_sceneFrames;
    }
    double GetSceneSeconds() const
    {
        return m_clock.GetElapsed();
    }
    bool& ShowDiagnostics()
    {
        return m_showDiagnostics;
    }
    void              SetInputCapture(bool mouse, bool keyboard);
    rc::RenderDevice& GetDevice() const
    {
        return m_device;
    }
    const rc::RenderView& GetRenderView() const
    {
        return m_target.GetView();
    }
    float GetSceneMilliseconds() const
    {
        return m_sceneMilliseconds;
    }
    const rc::RDGMetricsSnapshot& GetSceneMetrics() const
    {
        return m_sceneMetrics;
    }

private:
    friend struct RuntimeWindowTestAccess;
    void OnInput(const platform::InputEvent& event);
    void ApplyInput(float seconds);
    void ProcessShortcuts();
    void Fail(std::string error);

    rc::RenderDevice&                     m_device;
    rc::SceneRendering&                   m_scene;
    rc::SceneRenderTarget&                m_target;
    NavigationCamera&                     m_camera;
    RuntimeWindowSettings                 m_settings;
    UniquePtr<platform::NativeWindow>     m_window;
    RHIViewport*                          m_present{nullptr};
    rc::SceneCompositor                   m_compositor;
    rc::RenderOverlay*                    m_overlay{nullptr};
    runtime::SceneClock                   m_clock;
    runtime::RuntimeState                 m_state{runtime::RuntimeState::Idle};
    std::string                           m_error;
    uint64_t                              m_sceneFrames{0};
    float                                 m_sceneMilliseconds{0};
    rc::RDGMetricsSnapshot                m_sceneMetrics;
    std::chrono::steady_clock::time_point m_lastSceneTime;
    uint32_t                              m_declines{0};
    float                                 m_moveSpeed{1};
    float                                 m_wheel{0};
    bool                                  m_captureMouse{false};
    bool                                  m_captureKeyboard{false};
    bool                                  m_showDiagnostics{true};
};
} // namespace zen::runtime
