#pragma once
#include "Editor/ImGui/RenderGraphView.h"
#include "Editor/Services/EditorController.h"
#include "ImGui/UIContext.h"
#include "ImGui/ImGuiRenderer.h"

namespace zen::editor
{
class RenderGraphWindow final : public rc::RenderOverlay
{
public:
    explicit RenderGraphWindow(rc::RenderDevice& device, float uiScale = 0);
    ~RenderGraphWindow();
    bool                  Open();
    void                  Destroy();
    void                  Update();
    bool                  IsVisible() const;
    bool                  NeedsRedraw() const;
    void                  SynchronizeCapture(EditorController& editor, bool sceneVisible, bool editorMinimized);
    rc::RDGDebugSnapshot* PrepareCapture(EditorController& editor, bool sceneVisible, bool editorMinimized);
    void                  CompleteCapture(bool submitted);
    void                  Draw();
    bool                  BuildRenderGraph(rc::RenderGraph& graph, RHIViewport& viewport) override;
    // Selects the first pass with this name once a capture contains it.
    void SelectPass(const std::string& name);
    // Forces the next Draw and writes its presented image, for documentation and checks.
    void RequestFrameCapture(const std::string& path);
    bool IsFrameCapturePending() const;
    bool CaptureSucceeded() const;

private:
    friend struct RenderGraphWindowTestAccess;
    void                                  CompleteFrame(rc::RenderFrameResult result);
    void                                  OnInput(const platform::InputEvent& event);
    void                                  OnResize(uint32_t width, uint32_t height);
    rc::RenderDevice&                     m_device;
    UniquePtr<platform::NativeWindow>     m_window;
    RHIViewport*                          m_present{nullptr};
    ui::UIContext                         m_ui;
    UniquePtr<ui::ImGuiRenderer>          m_renderer;
    RenderGraphCaptureState               m_capture;
    RenderGraphView                       m_view;
    std::chrono::steady_clock::time_point m_previous;
    uint64_t                              m_changeSerial{0};
    uint32_t                              m_settleFrames{3};
    uint32_t                              m_declines{0};
    bool                                  m_focused{false};
    bool                                  m_minimized{false};
    float                                 m_scale{1};
    float                                 m_requestedScale{0};
    std::string                           m_capturePath;
    bool                                  m_captureSucceeded{false};
};
} // namespace zen::editor
