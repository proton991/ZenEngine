#include "RuntimeUI/SessionDebugUI.h"
#include "imgui.h"
#include <algorithm>
#include <cfloat>

namespace zen::ui
{
void DrawRuntimeMemoryStats(const RHIGPUMemoryStats& memory, uint64_t capacityBytes)
{
    constexpr double bytesPerMiB = 1024.0 * 1024.0;
    if (memory.available)
    {
        ImGui::Text("GPU memory (engine): %.0f MiB | peak %.0f MiB", memory.deviceLocalBytes / bytesPerMiB,
                    memory.peakDeviceLocalBytes / bytesPerMiB);
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip(
                "Engine buffers, textures and reusable pools. Excludes other applications and driver allocations.");
        }
    }
    else
    {
        ImGui::TextDisabled("GPU memory usage unavailable");
    }
    if (capacityBytes > 0)
    {
        ImGui::Text("Device-local capacity: %.0f MiB", capacityBytes / bytesPerMiB);
    }
    for (uint32_t index = 0; memory.budgetAvailable && index < memory.heapCount && index < memory.heaps.size(); ++index)
    {
        const RHIGPUMemoryStats::Heap& heap = memory.heaps[index];
        if (heap.deviceLocal && heap.budgetBytes > 0)
        {
            const uint64_t available = heap.budgetBytes > heap.usageBytes ? heap.budgetBytes - heap.usageBytes : 0;
            ImGui::Text("Heap %u driver estimate: %.0f MiB available / %.0f MiB budget", index, available / bytesPerMiB,
                        heap.budgetBytes / bytesPerMiB);
        }
    }
}

SessionDebugCommand DrawSessionDebugUI(const SessionDebugSnapshot&                snapshot,
                                       VectorView<const runtime::RuntimeLogEntry> logs,
                                       bool&                                      visible,
                                       SessionDebugUIState&                       state)
{
    SessionDebugCommand command = SessionDebugCommand::None;
    if (visible)
    {
        const ImGuiIO& io = ImGui::GetIO();
        ImGui::SetNextWindowPos(ImVec2(12, 12), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(std::min(520.0f, io.DisplaySize.x - 24), std::min(520.0f, io.DisplaySize.y - 24)),
                                 ImGuiCond_FirstUseEver);
        if (ImGui::Begin("ZenApp diagnostics", &visible))
        {
            if (ImGui::Button(snapshot.paused ? "Resume" : "Pause"))
            {
                command = snapshot.paused ? SessionDebugCommand::Resume : SessionDebugCommand::Pause;
            }
            ImGui::SameLine();
            if (ImGui::Button("Quit"))
            {
                command = SessionDebugCommand::Quit;
            }
            ImGui::SameLine();
            ImGui::TextDisabled("Space: pause/resume | F1: hide");
            ImGui::Text("%s | frame %llu | %.3f s", snapshot.paused ? "Paused" : "Running",
                        static_cast<unsigned long long>(snapshot.frame), snapshot.seconds);
            ImGui::TextWrapped("%s", snapshot.source.c_str());
            ImGui::TextWrapped("%s", snapshot.configuration.c_str());
            ImGui::Text("%u x %u | %.2f ms / present", snapshot.width, snapshot.height, snapshot.frameMilliseconds);
            ImGui::Text("Scene: %.1f FPS | %.2f ms%s",
                        snapshot.paused || snapshot.sceneMilliseconds <= 0 ? 0 : 1000 / snapshot.sceneMilliseconds,
                        snapshot.sceneMilliseconds, snapshot.paused ? " (last active)" : "");
            if (!snapshot.paused && snapshot.frame != state.lastFrame)
            {
                state.history[state.nextSample] = snapshot.sceneMilliseconds;
                state.nextSample                = (state.nextSample + 1) % static_cast<int>(state.history.size());
                state.lastFrame                 = snapshot.frame;
            }
            if (ImGui::CollapsingHeader("Frame diagnostics"))
            {
                ImGui::PlotLines("Scene ms", state.history.data(), static_cast<int>(state.history.size()), state.nextSample,
                                 nullptr, 0, FLT_MAX, ImVec2(0, 55));
                ImGui::Text("Last scene graph: %u passes, %u resources; CPU %.2f ms", snapshot.passes, snapshot.resources,
                            snapshot.graphCPUUs / 1000);
            }
            if (ImGui::CollapsingHeader("GPU memory"))
            {
                DrawRuntimeMemoryStats(snapshot.memory, snapshot.capacityBytes);
                if (snapshot.sharedScene)
                {
                    ImGui::TextUnformatted("Scene resources are shared with the editor.");
                }
            }
            if (snapshot.preparingGI)
            {
                ImGui::TextWrapped(snapshot.paused ? "GI preparation is paused." : "Preparing GI; the scene is running.");
            }
            if (!snapshot.status.empty())
            {
                ImGui::TextWrapped("%s", snapshot.status.c_str());
            }
            if (snapshot.droppedLogs > 0)
            {
                ImGui::TextDisabled("%llu older log messages omitted", static_cast<unsigned long long>(snapshot.droppedLogs));
            }
            ImGui::SeparatorText("Output");
            ImGui::SetNextItemWidth(120);
            ImGui::InputTextWithHint("##LogFilter", "Filter output", state.filter.data(), state.filter.size());
            ImGui::SameLine();
            ImGui::SetNextItemWidth(95);
            const char* severities[] = {"All", "Debug+", "Info+", "Warning+", "Error+", "Critical"};
            ImGui::Combo("##LogLevel", &state.severity, severities, 6);
            ImGui::SameLine();
            if (ImGui::Button("Clear"))
            {
                command = SessionDebugCommand::ClearLogs;
            }
            ImGui::SameLine();
            const bool  copy = ImGui::Button("Copy");
            std::string copied;
            ImGui::TextDisabled("Log: %s", snapshot.logPath.c_str());
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("%s", snapshot.logPath.c_str());
            }
            if (ImGui::BeginChild("RuntimeOutput", ImVec2(0, 0), ImGuiChildFlags_Borders))
            {
                const bool follow = ImGui::GetScrollY() >= ImGui::GetScrollMaxY();
                for (const runtime::RuntimeLogEntry& entry : logs)
                {
                    if (entry.level >= state.severity && entry.text.find(state.filter.data()) != std::string::npos)
                    {
                        const ImVec4 color = entry.level >= 4 ? ImVec4(1, 0.4f, 0.35f, 1)
                                           : entry.level == 3 ? ImVec4(1, 0.8f, 0.35f, 1)
                                                              : ImGui::GetStyleColorVec4(ImGuiCol_Text);
                        ImGui::PushStyleColor(ImGuiCol_Text, color);
                        ImGui::TextUnformatted(entry.text.c_str());
                        ImGui::PopStyleColor();
                        if (copy)
                        {
                            copied += entry.text + '\n';
                        }
                    }
                }
                if (follow)
                {
                    ImGui::SetScrollHereY(1);
                }
            }
            ImGui::EndChild();
            if (copy)
            {
                ImGui::SetClipboardText(copied.c_str());
            }
        }
        ImGui::End();
    }
    return command;
}
} // namespace zen::ui
