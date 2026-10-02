#include "UI/RuntimeDebugUI.h"
#include "Graphics/RenderCore/V2/Renderer/RendererServer.h"
#include "Graphics/RenderCore/V2/Renderer/VoxelizerBase.h"
#include "Platform/GlfwWindow.h"
#include "Platform/InputController.h"
#include "imgui.h"
#include <algorithm>
#include <cstdio>

namespace zen::ui
{
namespace
{
const char* RenderModeName(rc::RenderOption option)
{
    const char* name = "PBR";

    if (option == rc::RenderOption::eVoxelize)
    {
        name = "Voxels";
    }
    else if (option == rc::RenderOption::eVoxelGI)
    {
        name = "Voxel GI";
    }

    return name;
}

void DrawGPUMemoryStats(const rc::RenderDevice& device)
{
    const RHIGPUMemoryStats memory = device.GetGPUMemoryStats();

    if (memory.available)
    {
        constexpr double bytesPerMiB = 1024.0 * 1024.0;

        ImGui::Text("GPU memory (engine): %.0f MiB  |  peak %.0f MiB", memory.deviceLocalBytes / bytesPerMiB,
                    memory.peakDeviceLocalBytes / bytesPerMiB);

        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Memory reserved for engine buffers and textures, including reusable pools.\n"
                              "Excludes driver, swapchain and other applications.\n"
                              "All engine memory heaps: %.0f MiB (peak %.0f MiB).",
                              memory.committedBytes / bytesPerMiB, memory.peakCommittedBytes / bytesPerMiB);
        }

        const uint64_t capacity = device.GetGPUInfo().deviceLocalMemoryBytes;

        if (capacity != 0)
        {
            char label[80]{};

            std::snprintf(label, sizeof(label), "%.0f / %.0f MiB GPU capacity", memory.deviceLocalBytes / bytesPerMiB,
                          capacity / bytesPerMiB);

            const float fraction = std::clamp(float(double(memory.deviceLocalBytes) / double(capacity)), 0.0f, 1.0f);

            ImGui::ProgressBar(fraction, ImVec2(-1, 0), label);
        }
    }
    else
    {
        ImGui::TextDisabled("GPU memory usage unavailable");
    }
}
} // namespace

RuntimeDebugUI::RuntimeDebugUI(rc::RenderDevice&         device,
                               platform::GlfwWindowImpl& window,
                               RuntimeSceneControls&     sceneControls) :
    m_device(device), m_window(window), m_sceneControls(sceneControls), m_renderer(device)
{}

RuntimeDebugUI::~RuntimeDebugUI()
{
    m_renderer.Destroy();

    platform::KeyboardMouseInput::GetInstance().SetUICapture(false, false);

    platform::KeyboardMouseInput::GetInstance().Reset();

    m_context.Destroy();
}

bool RuntimeDebugUI::Init()
{
    m_initialized = m_context.Init(m_window.GetHandle());

    if (m_initialized)
    {
        m_initialized = m_renderer.Init(m_context.GetFonts());
    }

    if (m_initialized)
    {
        ReloadSettings();

        SetVisible(true);
    }

    return m_initialized;
}

void RuntimeDebugUI::SetVisible(bool visible)
{
    m_visible                           = visible;

    platform::KeyboardMouseInput& input = platform::KeyboardMouseInput::GetInstance();

    if (visible)
    {
        m_window.ShowCursor();

        input.Pause();
    }
    else
    {
        m_window.HideCursor();

        input.Resume();
    }

    input.SetDirty(!visible);
}

void RuntimeDebugUI::Update(float deltaSeconds, RHIViewport& viewport)
{
    m_context.BeginFrame(deltaSeconds, viewport.GetWidth(), viewport.GetHeight());

    const bool focused = glfwGetWindowAttrib(m_window.GetHandle(), GLFW_FOCUSED) == GLFW_TRUE;

    if (focused && ImGui::IsKeyPressed(ImGuiKey_F1, false))
    {
        SetVisible(!m_visible);
    }

    if (!focused && !m_visible)
    {
        SetVisible(true);
    }

    if (m_visible)
    {
        BuildPanel();
    }

    ImGuiIO& io = ImGui::GetIO();

    // F1 explicitly switches between debug interaction and captured camera control.
    // All input still reaches the chained platform callbacks, including releases.
    platform::KeyboardMouseInput::GetInstance().SetUICapture(!focused || m_visible || io.WantCaptureMouse,
                                                             !focused || m_visible || io.WantCaptureKeyboard);

    if (!m_visible && platform::KeyboardMouseInput::GetInstance().WasKeyPressedOnce(GLFW_KEY_ESCAPE))
    {
        glfwSetWindowShouldClose(m_window.GetHandle(), GLFW_TRUE);
    }

    m_context.EndFrame();
}

bool RuntimeDebugUI::BuildRenderGraph(rc::RenderGraph& graph, RHIViewport& viewport)
{
    const ImDrawData* data = m_context.GetDrawData();

    return data != nullptr && m_renderer.BuildRenderGraph(graph, viewport, *data);
}

void RuntimeDebugUI::BuildPanel()
{
    SynchronizeModelRevision();

    ImGui::SetNextWindowPos(ImVec2(16, 16), ImGuiCond_FirstUseEver);

    ImGui::SetNextWindowSize(ImVec2(600, 680), ImGuiCond_FirstUseEver);

    if (ImGui::Begin("ZenEngine | Runtime debug", nullptr, ImGuiWindowFlags_NoSavedSettings))
    {
        rc::RendererServer& server = *m_device.GetRendererServer();

        ImGui::TextUnformatted("F1: debug controls / camera");

        ImGui::Text("%.1f FPS  |  %.2f ms", ImGui::GetIO().Framerate, 1000.0f / ImGui::GetIO().Framerate);

        DrawGPUMemoryStats(m_device);

        ImGui::Separator();

        BuildModelSelector();

        ImGui::Separator();

        int mode = static_cast<int>(server.GetRequestedRenderOption());

        if (ImGui::Combo("View", &mode, "Voxels\0PBR\0Voxel GI\0"))
        {
            server.SetRenderOption(static_cast<rc::RenderOption>(mode));
        }

        ImGui::Text("Last rendered view: %s", RenderModeName(server.GetRenderOption()));

        if (server.GetRequestedRenderOption() == rc::RenderOption::eVoxelGI)
        {
            ImGui::TextUnformatted("GI: Cone tracing");

            if (server.GetRenderOption() == rc::RenderOption::ePBR)
            {
                ImGui::TextWrapped("PBR fallback is active: voxel GI and direct mesh shadows are inactive.");
            }
        }
        else
        {
            ImGui::TextUnformatted("Select the Voxel GI view to enable cone tracing.");
        }

        ImGui::Separator();

        ImGui::Checkbox("Apply changes automatically", &m_autoApply);

        ImGui::TextWrapped("Live controls update while dragging. Resource changes apply when the edit ends.");

        if (!m_sceneDirty)
        {
            m_sceneDraft    = m_sceneControls.GetRuntimeSceneSettings();

            m_sceneBaseline = m_sceneDraft;
        }

        ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x * 0.48f);

        if (ImGui::BeginTabBar("Configuration"))
        {
            if (ImGui::BeginTabItem("GI"))
            {
                BuildSettings();

                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Scene"))
            {
                BuildSceneSettings();

                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Config reference"))
            {
                BuildConfigReference();

                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
        }

        ImGui::PopItemWidth();

        ImGui::Separator();

        const bool pending = m_dirty || m_sceneDirty;

        ImGui::BeginDisabled(!pending);

        const bool manual = ImGui::Button("Apply pending");

        ImGui::EndDisabled();

        ImGui::SameLine();

        if (ImGui::Button("Reload current"))
        {
            ReloadSettings();
        }

        ApplyPendingSettings(manual);

        ImGui::TextWrapped("%s", m_status);

        ImGui::Separator();

        if (ImGui::Button("Rebuild voxels"))
        {
            server.RequestVoxelizer()->RequestVoxelization();
        }

        ImGui::SameLine();

        if (ImGui::Button("Capture RDG stats"))
        {
            m_device.GetRDGMetrics().RequestCapture();
        }

        const rc::RDGMetricsSnapshot& stats = m_device.GetRDGMetrics().GetLastSnapshot();

        ImGui::Text("RDG snapshot: %u passes, %u resources", stats.nodeCount, stats.resources);

        ImGui::Text("Graph preparation: %.3f ms", stats.compileCPUUs / 1000.0);

        const RHIThreadMetrics rhi = m_device.GetRHIThreadMetrics();

        ImGui::Text("RHI completed batches: %llu", static_cast<unsigned long long>(rhi.completedBatches));
    }

    ImGui::End();
}

} // namespace zen::ui
