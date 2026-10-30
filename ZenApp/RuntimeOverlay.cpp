#include "ZenApp/RuntimeOverlay.h"
#include "Graphics/RenderCore/V2/RenderDevice.h"
#include "Graphics/RenderCore/V2/Renderer/RendererServer.h"
#include "imgui.h"
#include <algorithm>

namespace zen::runtime
{
bool RuntimeOverlay::Initialize(rc::RenderDevice& device, platform::NativeWindow& window)
{
    bool valid = m_ui.Init(window, {false, window.GetUIScale(), true, 14});
    if (valid)
    {
        // Space/Escape/F1 control the scene; text fields still capture typed input.
        ImGui::GetIO().ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard;
        m_renderer                  = MakeUnique<ui::ImGuiRenderer>(device);
        valid                       = m_renderer->Init(m_ui.GetFonts());
    }
    if (!valid)
    {
        Destroy();
    }
    return valid;
}

void RuntimeOverlay::Destroy()
{
    if (m_renderer)
    {
        m_renderer->Destroy();
        m_renderer.Reset();
    }
    m_ui.Destroy();
    m_state = {};
}

bool RuntimeOverlay::Draw(RuntimeWindow&                    session,
                          VectorView<const RuntimeLogEntry> logs,
                          const std::string&                source,
                          const std::string&                logPath,
                          bool                              sharedScene,
                          uint64_t                          droppedLogs)
{
    const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    const float seconds = std::clamp(std::chrono::duration<float>(now - m_lastPresentation).count(), 0.001f, 0.1f);
    m_lastPresentation  = now;
    const platform::WindowExtent extent = session.GetWindow()->GetFramebufferExtent();
    m_ui.BeginFrame(seconds, extent.width, extent.height);
    rc::RenderDevice&             device  = session.GetDevice();
    const rc::RendererServer&     server  = *device.GetRendererServer();
    const rc::RenderingStatus&    status  = server.GetRenderingStatus();
    const rc::RDGMetricsSnapshot& metrics = session.GetSceneMetrics();
    const rc::RenderView&         view    = session.GetRenderView();
    ui::SessionDebugSnapshot      snapshot;
    snapshot.paused            = session.GetState() == RuntimeState::Paused;
    snapshot.preparingGI       = server.IsPreparingGI();
    snapshot.frame             = session.GetSceneFrame();
    snapshot.seconds           = session.GetSceneSeconds();
    snapshot.frameMilliseconds = seconds * 1000;
    snapshot.sceneMilliseconds = session.GetSceneMilliseconds();
    snapshot.width             = view.width;
    snapshot.height            = view.height;
    snapshot.memory            = device.GetGPUMemoryStats();
    snapshot.capacityBytes     = device.GetGPUInfo().deviceLocalMemoryBytes;
    snapshot.sharedScene       = sharedScene;
    snapshot.droppedLogs       = droppedLogs;
    snapshot.status            = status.fallbackReason;
    snapshot.source            = source;
    snapshot.logPath           = logPath;
    snapshot.configuration     = fmt::format("{} -> {}{}", status.requested == rc::RenderAlgorithm::eVoxelGI ? "GI" : "PBR",
                                             status.effective == rc::RenderAlgorithm::eVoxelGI ? "GI" : "PBR",
                                             sharedScene ? " | shared editor scene" : "");
    snapshot.graphCPUUs        = metrics.compileCPUUs + metrics.executeCPUUs;
    snapshot.passes            = metrics.nodeCount;
    snapshot.resources         = metrics.resources;
    const ui::SessionDebugCommand command = ui::DrawSessionDebugUI(snapshot, logs, session.ShowDiagnostics(), m_state);
    if (command == ui::SessionDebugCommand::Pause)
    {
        session.Pause();
    }
    else if (command == ui::SessionDebugCommand::Resume)
    {
        session.Resume();
    }
    else if (command == ui::SessionDebugCommand::Quit)
    {
        session.Quit();
    }
    session.SetInputCapture(ImGui::GetIO().WantCaptureMouse, ImGui::GetIO().WantCaptureKeyboard);
    m_ui.EndFrame();
    return command == ui::SessionDebugCommand::ClearLogs;
}

bool RuntimeOverlay::BuildRenderGraph(rc::RenderGraph& graph, RHIViewport& viewport)
{
    const ImDrawData* data = m_ui.GetDrawData();
    return data != nullptr && m_renderer->BuildRenderGraph(graph, viewport, *data);
}
} // namespace zen::runtime
