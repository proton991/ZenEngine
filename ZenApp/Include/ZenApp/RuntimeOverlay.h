#pragma once
#include "ZenApp/RuntimeWindow.h"
#include "RuntimeUI/SessionDebugUI.h"
#include "ImGui/UIContext.h"
#include "ImGui/ImGuiRenderer.h"

namespace zen::runtime
{
// Optional diagnostics shared by the editor-hosted and standalone ZenApp window.
class RuntimeOverlay final : public rc::RenderOverlay
{
public:
    bool Initialize(rc::RenderDevice& device, platform::NativeWindow& window);
    void Destroy();
    bool IsInitialized() const
    {
        return m_renderer.Get() != nullptr;
    }
    // Returns true when the user requests clearing this diagnostics view's log.
    bool Draw(RuntimeWindow&                    session,
              VectorView<const RuntimeLogEntry> logs,
              const std::string&                source,
              const std::string&                logPath,
              bool                              sharedScene = false,
              uint64_t                          droppedLogs = 0);
    bool BuildRenderGraph(rc::RenderGraph& graph, RHIViewport& viewport) override;

private:
    ui::UIContext                         m_ui;
    UniquePtr<ui::ImGuiRenderer>          m_renderer;
    ui::SessionDebugUIState               m_state;
    std::chrono::steady_clock::time_point m_lastPresentation{std::chrono::steady_clock::now()};
};
} // namespace zen::runtime
