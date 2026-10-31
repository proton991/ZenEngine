#include "RenderGraphWindow.h"
#include "Editor/ImGui/EditorTheme.h"
#include "Graphics/RenderCore/V2/Renderer/RendererServer.h"
#include "Graphics/RenderCore/V2/ScenePresentation.h"
#include <algorithm>

namespace zen::editor
{
RenderGraphWindow::RenderGraphWindow(rc::RenderDevice& device, float uiScale) : m_device(device), m_requestedScale(uiScale) {}

RenderGraphWindow::~RenderGraphWindow()
{
    Destroy();
}

bool RenderGraphWindow::Open()
{
    bool          valid    = true;
    ImGuiContext* previous = ImGui::GetCurrentContext();
    if (!m_window)
    {
        m_window = MakeUnique<platform::NativeWindow>(platform::WindowConfig{"Render Graph", true, 1280, 800, 0, false});
        m_window->SetMinimumSize(720, 480);
        const platform::WindowExtent extent = m_window->GetFramebufferExtent();
        m_present = m_device.CreateViewport(m_window.Get(), std::max(extent.width, 1u), std::max(extent.height, 1u), true);
        const float uiScale = m_requestedScale > 0 ? m_requestedScale : m_window->GetUIScale();
        valid               = m_present != nullptr && m_ui.Init(*m_window, {false, uiScale, m_requestedScale <= 0, 15});
        if (valid)
        {
            ApplyEditorTheme(uiScale);
            m_view.InitFonts(uiScale);
            m_renderer = MakeUnique<ui::ImGuiRenderer>(m_device);
            valid      = m_renderer->Init(m_ui.GetFonts());
        }
        if (valid)
        {
            m_window->SetOnInput(std::bind_front(&RenderGraphWindow::OnInput, this));
            m_window->SetOnResize(std::bind_front(&RenderGraphWindow::OnResize, this));
            m_previous = std::chrono::steady_clock::now();
            m_window->Show();
        }
        else
        {
            LOGE("Could not initialize the Render Graph window");
            Destroy();
        }
    }
    if (valid)
    {
        if (m_window->IsMinimized())
        {
            m_window->Restore();
        }
        m_window->Focus();
        m_settleFrames = 3;
    }
    ImGui::SetCurrentContext(previous);
    return valid;
}

void RenderGraphWindow::Destroy()
{
    // Called between frames. Renderer resources retire through RenderDevice;
    // DestroyViewport drains submitted uses before the native surface disappears.
    if (m_window)
    {
        m_window->SetOnInput({});
        m_window->SetOnResize({});
    }
    if (m_renderer)
    {
        m_renderer->Destroy();
        m_renderer.Reset();
    }
    m_ui.Destroy();
    if (m_present != nullptr)
    {
        m_device.DestroyViewport(m_present);
        m_present = nullptr;
    }
    m_window.Reset();
    m_capture      = {};
    m_view         = {};
    m_settleFrames = 3;
    m_declines     = 0;
    m_changeSerial = 0;
    m_capturePath.clear();
    m_captureSucceeded = false;
}

void RenderGraphWindow::Update()
{
    if (m_window && m_window->ShouldClose())
    {
        Destroy();
    }
}

bool RenderGraphWindow::IsVisible() const
{
    return m_window && !m_window->IsMinimized();
}

rc::RDGDebugSnapshot* RenderGraphWindow::PrepareCapture(EditorController& editor, bool sceneVisible, bool editorMinimized)
{
    rc::RDGDebugSnapshot* result = nullptr;
    SynchronizeCapture(editor, sceneVisible, editorMinimized);
    if (IsVisible())
    {
        result = m_capture.RequestCapture();
    }
    return result;
}

void RenderGraphWindow::SynchronizeCapture(EditorController& editor, bool sceneVisible, bool editorMinimized)
{
    if (m_window)
    {
        const runtime::RuntimeWindow&  play      = editor.GetPlaySession();
        const bool                     playing   = play.IsActive();
        const EditorRenderingState&    rendering = editor.GetRenderingState();
        const RenderGraphCaptureSource source{playing, editor.GetScene().GetGeneration(), rendering.GetAppliedRevision(),
                                              rendering.GetApplied().scenePath};
        std::string                    unavailable;
        if (!editor.GetViewport().HasScene())
        {
            unavailable = "No scene loaded. Open a scene to capture its render graph.";
        }
        else if (playing && play.GetState() == runtime::RuntimeState::Paused)
        {
            unavailable = "Play is paused; showing the last captured scene frame.";
        }
        else if (playing && (!play.GetWindow() || play.GetWindow()->IsMinimized()))
        {
            unavailable = "Play is minimized; waiting for scene rendering.";
        }
        else if (!playing && (!sceneVisible || editorMinimized))
        {
            unavailable = "Scene view is hidden or minimized; waiting for scene rendering.";
        }
        m_capture.Synchronize(source, unavailable, rendering.GetApplyStatus() == EditorRenderingApplyStatus::AwaitingApply);
    }
}

void RenderGraphWindow::CompleteCapture(bool submitted)
{
    const rc::RenderingStatus& status = m_device.GetRendererServer()->GetRenderingStatus();
    const std::string algorithm = fmt::format("{} -> {}", status.requested == rc::RenderAlgorithm::eVoxelGI ? "GI" : "PBR",
                                              status.effective == rc::RenderAlgorithm::eVoxelGI ? "GI" : "PBR");
    m_capture.CompleteCapture(submitted, m_device.GetCurrentFrameRDG()->GetResult().message, algorithm, status.fallbackReason);
}

void RenderGraphWindow::OnInput(const platform::InputEvent&)
{
    m_settleFrames = 3;
}

void RenderGraphWindow::OnResize(uint32_t, uint32_t)
{
    m_settleFrames = 3;
}

bool RenderGraphWindow::NeedsRedraw() const
{
    bool needed = false;
    if (IsVisible())
    {
        const platform::WindowExtent extent = m_window->GetFramebufferExtent();
        needed                              = extent.width > 0 && extent.height > 0
                                           && (m_settleFrames > 0 || m_changeSerial != m_capture.GetChangeSerial() || m_window->IsFocused() != m_focused
                                               || m_minimized || m_window->GetUIScale() != m_scale || extent.width != m_present->GetWidth()
                                               || extent.height != m_present->GetHeight());
    }
    return needed;
}

void RenderGraphWindow::Draw()
{
    if (m_window)
    {
        const platform::WindowExtent extent    = m_window->GetFramebufferExtent();
        const bool                   focused   = m_window->IsFocused();
        const bool                   minimized = m_window->IsMinimized();
        const float                  scale     = m_window->GetUIScale();
        if (focused != m_focused || minimized != m_minimized || scale != m_scale || extent.width != m_present->GetWidth()
            || extent.height != m_present->GetHeight())
        {
            m_settleFrames = 3;
        }
        m_focused   = focused;
        m_minimized = minimized;
        m_scale     = scale;
        if (NeedsRedraw())
        {
            if (m_settleFrames > 0)
            {
                --m_settleFrames;
            }
            ImGuiContext* previous = ImGui::GetCurrentContext();
            m_device.ResizeViewport(m_present, extent.width, extent.height);
            const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
            const float seconds = std::clamp(std::chrono::duration<float>(now - m_previous).count(), 0.001f, 0.1f);
            m_previous          = now;
            m_ui.BeginFrame(seconds, extent.width, extent.height);
            ImGui::SetNextWindowPos(ImVec2(0, 0));
            ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
            ImGui::Begin("Render Graph", nullptr,
                         ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
            m_view.Draw(m_capture);
            ImGui::End();
            if (ImGui::IsAnyItemActive())
            {
                m_settleFrames = 3;
            }
            else if (m_view.IsAnimating())
            {
                m_settleFrames = std::max(m_settleFrames, 1u);
            }
            m_ui.EndFrame();
            const rc::RenderFrameResult result = m_device.GetRendererServer()->DispatchRenderWorkloads(
                rc::RenderView::FromViewport(*m_present), this, false, m_present);
            m_changeSerial = m_capture.GetChangeSerial();
            ImGui::SetCurrentContext(previous);
            CompleteFrame(result);
        }
    }
}

void RenderGraphWindow::CompleteFrame(rc::RenderFrameResult result)
{
    // A declined frame has no presented image. Keep the request through NextFrame
    // so the caller can retry, with the same bounded decline policy as normal draws.
    if (!m_capturePath.empty() && result == rc::RenderFrameResult::eSubmitted)
    {
        m_captureSucceeded = rc::CaptureSceneFrame(m_device, m_present->GetColorBackBuffer(), m_capturePath);
        m_capturePath.clear();
    }
    m_device.NextFrame();
    if (result == rc::RenderFrameResult::eDeclined)
    {
        m_settleFrames = 3;
    }
    if (result == rc::RenderFrameResult::eFailed || result == rc::RenderFrameResult::eBlocked
        || !rc::TrackDeclinedFrames(result, m_declines))
    {
        LOGE("Render Graph presentation failed; closing the viewer");
        Destroy();
    }
}

bool RenderGraphWindow::BuildRenderGraph(rc::RenderGraph& graph, RHIViewport& viewport)
{
    const ImDrawData* data = m_ui.GetDrawData();
    return data != nullptr && m_renderer->BuildRenderGraph(graph, viewport, *data);
}

void RenderGraphWindow::SelectPass(const std::string& name)
{
    m_view.RequestSelection(name);
}

void RenderGraphWindow::RequestFrameCapture(const std::string& path)
{
    if (m_window)
    {
        m_capturePath      = path;
        m_captureSucceeded = false;
        m_settleFrames     = std::max(m_settleFrames, 1u);
    }
}

bool RenderGraphWindow::CaptureSucceeded() const
{
    return m_captureSucceeded;
}

bool RenderGraphWindow::IsFrameCapturePending() const
{
    return !m_capturePath.empty();
}
} // namespace zen::editor
