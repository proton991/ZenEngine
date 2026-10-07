#include "ImGui/UIContext.h"
#include "imgui.h"
#include "Platform/WindowBackend.h"
#include "Utils/Errors.h"
#if defined(ZEN_WINDOW_SDL3)
#    include "backends/imgui_impl_sdl3.h"
#else
#    include "backends/imgui_impl_glfw.h"
#endif
#include <cmath>

namespace zen::ui
{
#if defined(ZEN_WINDOW_SDL3)
namespace
{
void ProcessPlatformEvent(void* context, const void* event)
{
    ImGuiContext* previous = ImGui::GetCurrentContext();

    ImGui::SetCurrentContext(static_cast<ImGuiContext*>(context));

    ImGui_ImplSDL3_ProcessEvent(static_cast<const SDL_Event*>(event));

    ImGui::SetCurrentContext(previous);
}
} // namespace
#endif

UIContext::~UIContext()
{
    Destroy();
}

bool UIContext::Init(platform::NativeWindow& window, const UIContextOptions& options)
{
    bool valid = m_context == nullptr;

    if (valid)
    {
        IMGUI_CHECKVERSION();

        m_previous = ImGui::GetCurrentContext();

        m_context  = ImGui::CreateContext();

        ImGui::SetCurrentContext(m_context);

        ImGuiIO& io     = ImGui::GetIO();

        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

        if (options.docking)
        {
            io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
        }

        const float scale                   = std::isfinite(options.scale) && options.scale > 0 ? options.scale : 1.0f;

        const platform::WindowExtent units  = window.GetExtent2D();

        const platform::WindowExtent pixels = window.GetFramebufferExtent();

        const float density = units.width > 0 && pixels.width > 0 ? float(pixels.width) / float(units.width) : 1.0f;

        ImFontConfig font;

        font.SizePixels        = (std::isfinite(options.fontSize) && options.fontSize > 0 ? options.fontSize : 13.0f) * scale;

        font.RasterizerDensity = density;

        io.Fonts->AddFontDefaultVector(&font);

        io.DisplayFramebufferScale  = ImVec2(density, density);

        m_monitorScale              = window.GetUIScale();

        m_followDisplayScale        = options.followDisplayScale;

        io.BackendFlags            |= ImGuiBackendFlags_RendererHasVtxOffset;

        io.BackendRendererName      = "ZenRHI_StaticAtlas";

        // Runtime settings are session-only. An editor can opt into its own layout persistence.
        io.IniFilename = nullptr;

        io.LogFilename = nullptr;

        ImGui::StyleColorsDark();

        ImGui::GetStyle().WindowRounding = 5.0f;

        ImGui::GetStyle().ScaleAllSizes(scale);

        m_window = &window;

#if defined(ZEN_WINDOW_SDL3)
        // ImGui changes this global hint for detached viewports. Preserve the host's
        // native window style so later restore/maximize operations still respect the taskbar.
        constexpr const char* borderlessStyleHint = "SDL_BORDERLESS_WINDOWED_STYLE";

        const bool borderlessWindowedStyle        = SDL_GetHintBoolean(borderlessStyleHint, true);

        m_platformReady                           = ImGui_ImplSDL3_InitForVulkan(platform::WindowBackend::Borrow(window));

        // A higher-priority hint may reject the write while already retaining the original value.
        valid = SDL_SetHint(borderlessStyleHint, borderlessWindowedStyle ? "1" : "0")
             || SDL_GetHintBoolean(borderlessStyleHint, true) == borderlessWindowedStyle;

        if (!valid)
        {
            LOGE("Could not preserve the native window style after ImGui initialization: {}", SDL_GetError());
        }

        if (m_platformReady)
        {
            platform::WindowBackend::Observe(window, ProcessPlatformEvent, m_context);
        }
#else
        m_platformReady = ImGui_ImplGlfw_InitForOther(platform::WindowBackend::Borrow(window), true);
#endif

        valid = valid && m_platformReady;

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
#if defined(ZEN_WINDOW_SDL3)
            platform::WindowBackend::Observe(*m_window, nullptr, nullptr);

            ImGui_ImplSDL3_Shutdown();
#else
            ImGui_ImplGlfw_Shutdown();
#endif
        }

        ImGui::DestroyContext(m_context);

        ImGui::SetCurrentContext(m_previous);

        m_context       = nullptr;

        m_platformReady = false;

        m_frameActive   = false;
    }
}

void UIContext::BeginFrame(float deltaSeconds, unsigned int targetWidth, unsigned int targetHeight)
{
    ImGui::SetCurrentContext(m_context);

#if defined(ZEN_WINDOW_SDL3)
    ImGui_ImplSDL3_NewFrame();
#else
    ImGui_ImplGlfw_NewFrame();
#endif

    const float monitorScale = m_window->GetUIScale();

    if (m_followDisplayScale && std::isfinite(monitorScale) && monitorScale > 0 && m_monitorScale > 0
        && std::abs(monitorScale - m_monitorScale) > 0.01f)
    {
        const float ratio = monitorScale / m_monitorScale;

        ImGuiStyle& style = ImGui::GetStyle();

        style.ScaleAllSizes(ratio);

        style.FontScaleDpi *= ratio;

        m_monitorScale      = monitorScale;
    }

    ImGuiIO& io  = ImGui::GetIO();

    io.DeltaTime = std::isfinite(deltaSeconds) && deltaSeconds > 0 ? deltaSeconds : 1.0f / 60.0f;

    // Draw into the actual engine target, including framebuffer/logical size differences.
    if (io.DisplaySize.x > 0 && io.DisplaySize.y > 0)
    {
        io.DisplayFramebufferScale = ImVec2(float(targetWidth) / io.DisplaySize.x, float(targetHeight) / io.DisplaySize.y);
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
