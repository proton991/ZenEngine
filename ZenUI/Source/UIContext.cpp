#include "UI/UIContext.h"
#include "imgui.h"
#include "backends/imgui_impl_glfw.h"
#include <cmath>

namespace zen::ui
{
UIContext::~UIContext()
{
    Destroy();
}

bool UIContext::Init(GLFWwindow* window)
{
    bool valid = window != nullptr && m_context == nullptr;

    if (valid)
    {
        IMGUI_CHECKVERSION();

        m_previous = ImGui::GetCurrentContext();

        m_context = ImGui::CreateContext();

        ImGui::SetCurrentContext(m_context);

        ImGuiIO& io = ImGui::GetIO();

        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

        io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;

        io.BackendRendererName = "ZenRHI_StaticAtlas";

        // Runtime settings are session-only. An editor can opt into its own layout persistence.
        io.IniFilename = nullptr;

        io.LogFilename = nullptr;

        ImGui::StyleColorsDark();

        ImGui::GetStyle().WindowRounding = 5.0f;

        m_platformReady = ImGui_ImplGlfw_InitForOther(window, true);

        valid = m_platformReady;

        if (!valid)
        {
            Destroy();
        }
    }

    return valid;
}

void UIContext::Destroy()
{
    if (m_context != nullptr)
    {
        ImGui::SetCurrentContext(m_context);

        if (m_frameActive)
        {
            ImGui::EndFrame();
        }

        if (m_platformReady)
        {
            ImGui_ImplGlfw_Shutdown();
        }

        ImGui::DestroyContext(m_context);

        ImGui::SetCurrentContext(m_previous);

        m_context = nullptr;

        m_platformReady = false;

        m_frameActive = false;
    }
}

void UIContext::BeginFrame(float deltaSeconds, unsigned int targetWidth, unsigned int targetHeight)
{
    ImGui::SetCurrentContext(m_context);

    ImGui_ImplGlfw_NewFrame();

    ImGuiIO& io = ImGui::GetIO();

    io.DeltaTime = std::isfinite(deltaSeconds) && deltaSeconds > 0 ? deltaSeconds : 1.0f / 60.0f;

    // Draw into the actual engine target, including framebuffer/logical size differences.
    if (io.DisplaySize.x > 0 && io.DisplaySize.y > 0)
    {
        io.DisplayFramebufferScale =
            ImVec2(float(targetWidth) / io.DisplaySize.x, float(targetHeight) / io.DisplaySize.y);
    }

    ImGui::NewFrame();

    m_frameActive = true;
}

void UIContext::EndFrame()
{
    ImGui::SetCurrentContext(m_context);

    ImGui::Render();

    m_frameActive = false;
}

ImFontAtlas& UIContext::GetFonts()
{
    ImGui::SetCurrentContext(m_context);

    return *ImGui::GetIO().Fonts;
}

const ImDrawData* UIContext::GetDrawData()
{
    ImGui::SetCurrentContext(m_context);

    return ImGui::GetDrawData();
}
} // namespace zen::ui
