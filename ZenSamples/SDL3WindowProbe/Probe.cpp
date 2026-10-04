#include "Platform/NativeWindow.h"
#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_vulkan.h>
#include "imgui.h"
#include "backends/imgui_impl_sdl3.h"
#include <vulkan/vulkan.h>
#include <windows.h>
#include <array>
#include <cstdio>

namespace
{
class Results
{
public:
    void Check(bool success, const char* description)
    {
        std::printf("%s %s\n", success ? "PASS" : "FAIL", description);

        success ? ++passed : ++failed;
    }

    int passed{0};

    int failed{0};
};

struct TitleBarRegion
{
    int menuEnd{250};

    int height{36};

    int controlsWidth{138};

    bool blocked{false};
};

// Isolated evaluation: shares engine value types without linking the production backend.
// SDL access below is private to this standalone integration probe, not a proposed public API.
class ProbeWindow final
{
public:
    ProbeWindow()
    {
        m_window = SDL_CreateWindow("ZenEngine SDL3 evaluation", 1000, 700,
                                    SDL_WINDOW_VULKAN | SDL_WINDOW_BORDERLESS | SDL_WINDOW_RESIZABLE
                                        | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN);
    }

    ~ProbeWindow()
    {
        if (m_window != nullptr)
        {
            SDL_DestroyWindow(m_window);
        }
    }

    zen::platform::WindowExtent GetExtent2D() const
    {
        int width  = 0;

        int height = 0;

        SDL_GetWindowSize(m_window, &width, &height);

        return {static_cast<uint32_t>(width), static_cast<uint32_t>(height)};
    }

    float GetAspect()
    {
        const zen::platform::WindowExtent extent = GetExtent2D();

        return extent.height != 0 ? float(extent.width) / float(extent.height) : 0.0f;
    }

    zen::platform::WindowExtent GetFramebufferExtent() const
    {
        int width  = 0;

        int height = 0;

        SDL_GetWindowSizeInPixels(m_window, &width, &height);

        return {static_cast<uint32_t>(width), static_cast<uint32_t>(height)};
    }

    SDL_Window* Handle() const
    {
        return m_window;
    }

    static SDL_HitTestResult SDLCALL HitTest(SDL_Window* window, const SDL_Point* point, void* data)
    {
        const TitleBarRegion& region = *static_cast<const TitleBarRegion*>(data);

        int width                    = 0;

        int height                   = 0;

        SDL_GetWindowSize(window, &width, &height);

        SDL_HitTestResult result = SDL_HITTEST_NORMAL;

        if ((SDL_GetWindowFlags(window) & SDL_WINDOW_MAXIMIZED) == 0)
        {
            const bool left   = point->x < 6;

            const bool right  = point->x >= width - 6;

            const bool top    = point->y < 6;

            const bool bottom = point->y >= height - 6;

            if (top)
            {
                result = left ? SDL_HITTEST_RESIZE_TOPLEFT : right ? SDL_HITTEST_RESIZE_TOPRIGHT : SDL_HITTEST_RESIZE_TOP;
            }
            else if (bottom)
            {
                result = left  ? SDL_HITTEST_RESIZE_BOTTOMLEFT
                       : right ? SDL_HITTEST_RESIZE_BOTTOMRIGHT
                               : SDL_HITTEST_RESIZE_BOTTOM;
            }
            else if (left || right)
            {
                result = left ? SDL_HITTEST_RESIZE_LEFT : SDL_HITTEST_RESIZE_RIGHT;
            }
        }

        if (result == SDL_HITTEST_NORMAL && !region.blocked && point->y >= 0 && point->y < region.height
            && point->x >= region.menuEnd && point->x < width - region.controlsWidth)
        {
            result = SDL_HITTEST_DRAGGABLE;
        }

        return result;
    }

private:
    SDL_Window* m_window{nullptr};
};

LRESULT NativeHitTest(HWND handle, int x, int y)
{
    POINT point{x, y};

    ClientToScreen(handle, &point);

    return SendMessageW(handle, WM_NCHITTEST, 0, MAKELPARAM(point.x, point.y));
}

void CheckNativeChrome(ProbeWindow& window, TitleBarRegion& region, Results& results)
{
    HWND handle = static_cast<HWND>(
        SDL_GetPointerProperty(SDL_GetWindowProperties(window.Handle()), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));

    results.Check(handle != nullptr, "SDL native HWND access");

    if (handle != nullptr)
    {
        RECT client{};

        GetClientRect(handle, &client);

        results.Check(client.right == 1000 && client.bottom == 700, "borderless client preserves requested extent");

        results.Check(NativeHitTest(handle, 100, 18) == HTCLIENT, "menu stays interactive");

        results.Check(NativeHitTest(handle, 500, 18) == HTCAPTION, "blank title region uses native caption hit");

        results.Check(NativeHitTest(handle, 930, 18) == HTCLIENT, "window buttons stay interactive");

        results.Check(NativeHitTest(handle, 500, 150) == HTCLIENT, "editor content stays interactive");

        const std::array<POINT, 8> points{{{1, 1}, {500, 1}, {998, 1}, {998, 350}, {998, 698}, {500, 698}, {1, 698}, {1, 350}}};

        const std::array<LRESULT, 8> expected{HTTOPLEFT,     HTTOP,    HTTOPRIGHT,   HTRIGHT,
                                              HTBOTTOMRIGHT, HTBOTTOM, HTBOTTOMLEFT, HTLEFT};

        bool bordersCorrect = true;

        for (size_t index = 0; index < points.size(); ++index)
        {
            bordersCorrect = NativeHitTest(handle, points[index].x, points[index].y) == expected[index] && bordersCorrect;
        }

        results.Check(bordersCorrect, "all eight native resize edges and corners");

        region.blocked = true;

        results.Check(NativeHitTest(handle, 500, 18) == HTCLIENT, "popup blocking disables caption drag");

        region.blocked = false;
    }
}

void CheckWindowState(ProbeWindow& window, Results& results)
{
    // Hidden windows defer native state changes; a successful request is not an observed size.
    // Briefly show without activation so this checks real work-area behavior.
    SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");

    results.Check(SDL_ShowWindow(window.Handle()) && SDL_SyncWindow(window.Handle()), "test window shown without activation");

    results.Check(SDL_MaximizeWindow(window.Handle()) && SDL_SyncWindow(window.Handle()), "maximize request completes");

    SDL_PumpEvents();

    results.Check((SDL_GetWindowFlags(window.Handle()) & SDL_WINDOW_MAXIMIZED) != 0, "maximized state reported");

    SDL_Rect workArea{};

    const bool workAreaKnown = SDL_GetDisplayUsableBounds(SDL_GetDisplayForWindow(window.Handle()), &workArea);

    const zen::platform::WindowExtent maximized = window.GetExtent2D();

    std::printf("INFO maximized=%ux%u workarea=%dx%d display-scale=%.2f pixel-density=%.2f\n", maximized.width,
                maximized.height, workArea.w, workArea.h, SDL_GetWindowDisplayScale(window.Handle()),
                SDL_GetWindowPixelDensity(window.Handle()));

    results.Check(workAreaKnown && maximized.width == uint32_t(workArea.w) && maximized.height == uint32_t(workArea.h),
                  "maximized borderless window respects desktop work area");

    HWND handle = static_cast<HWND>(
        SDL_GetPointerProperty(SDL_GetWindowProperties(window.Handle()), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));

    if (handle != nullptr)
    {
        results.Check(NativeHitTest(handle, 1, 1) == HTCLIENT, "maximized window has no resize border");

        SendMessageW(handle, WM_NCLBUTTONDBLCLK, HTCAPTION, 0);

        SDL_PumpEvents();

        results.Check((SDL_GetWindowFlags(window.Handle()) & SDL_WINDOW_MAXIMIZED) == 0,
                      "native caption double click restores window");
    }

    results.Check(SDL_RestoreWindow(window.Handle()) && SDL_SyncWindow(window.Handle()), "restore request completes");

    results.Check(SDL_SetWindowSize(window.Handle(), 1200, 800) && SDL_SyncWindow(window.Handle()), "resize request completes");

    const zen::platform::WindowExtent resized = window.GetExtent2D();

    const zen::platform::WindowExtent pixels  = window.GetFramebufferExtent();

    results.Check(resized.width == 1200 && resized.height == 800, "engine base reports resized window coordinates");

    results.Check(pixels.width > 0 && pixels.height > 0, "framebuffer pixel extent available separately");

    results.Check(SDL_MinimizeWindow(window.Handle()) && SDL_SyncWindow(window.Handle()), "minimize request completes");

    results.Check((SDL_GetWindowFlags(window.Handle()) & SDL_WINDOW_MINIMIZED) != 0, "minimized state reported");

    results.Check(SDL_RestoreWindow(window.Handle()) && SDL_SyncWindow(window.Handle()), "restore after minimize completes");

    SDL_HideWindow(window.Handle());
}

void CheckImGui(ProbeWindow& window, Results& results)
{
    IMGUI_CHECKVERSION();

    ImGui::CreateContext();

    ImGuiIO& io       = ImGui::GetIO();

    io.IniFilename    = nullptr;

    io.ConfigFlags   |= ImGuiConfigFlags_DockingEnable;

    const bool ready  = ImGui_ImplSDL3_InitForVulkan(window.Handle());

    results.Check(ready, "existing ImGui docking version initializes official SDL3 platform backend");

    if (ready)
    {
        unsigned char* fontPixels = nullptr;

        int fontWidth             = 0;

        int fontHeight            = 0;

        io.Fonts->GetTexDataAsRGBA32(&fontPixels, &fontWidth, &fontHeight);

        io.Fonts->SetTexID(ImTextureID(1));

        SDL_Event event{};

        while (SDL_PollEvent(&event))
        {
            ImGui_ImplSDL3_ProcessEvent(&event);
        }

        event              = {};

        event.type         = SDL_EVENT_KEY_DOWN;

        event.key.windowID = SDL_GetWindowID(window.Handle());

        event.key.scancode = SDL_SCANCODE_W;

        event.key.key      = SDLK_W;

        event.key.down     = true;

        results.Check(ImGui_ImplSDL3_ProcessEvent(&event), "SDL key event accepted by ImGui platform bridge");

        event               = {};

        event.type          = SDL_EVENT_TEXT_INPUT;

        event.text.windowID = SDL_GetWindowID(window.Handle());

        event.text.text     = "Zen";

        results.Check(ImGui_ImplSDL3_ProcessEvent(&event), "SDL committed text event accepted by ImGui platform bridge");

        for (int frame = 0; frame < 2; ++frame)
        {
            ImGui_ImplSDL3_NewFrame();

            ImGui::NewFrame();

            ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());

            ImGui::Begin("SDL3 evaluation");

            ImGui::TextUnformatted("ZenEngine keeps its own RDG/RHI renderer.");

            ImGui::End();

            ImGui::Render();
        }

        const ImDrawData* drawData = ImGui::GetDrawData();

        results.Check(fontPixels != nullptr && fontWidth > 0 && fontHeight > 0, "static font atlas is available");

        results.Check(drawData != nullptr && drawData->Valid && drawData->TotalVtxCount > 0,
                      "docking UI emits draw data without an SDL or ImGui Vulkan renderer");

        ImGui_ImplSDL3_Shutdown();
    }

    ImGui::DestroyContext();
}

void CheckVulkan(ProbeWindow& window, Results& results)
{
    Uint32 extensionCount         = 0;

    const char* const* extensions = SDL_Vulkan_GetInstanceExtensions(&extensionCount);

    results.Check(extensions != nullptr && extensionCount > 0, "SDL reports required Vulkan instance extensions");

    VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};

    application.pApplicationName = "ZenEngine SDL3 evaluation";

    application.apiVersion       = VK_API_VERSION_1_2;

    VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};

    instanceInfo.pApplicationInfo        = &application;

    instanceInfo.enabledExtensionCount   = extensionCount;

    instanceInfo.ppEnabledExtensionNames = extensions;

    VkInstance instance                  = VK_NULL_HANDLE;

    const bool instanceReady = extensions != nullptr && vkCreateInstance(&instanceInfo, nullptr, &instance) == VK_SUCCESS;

    results.Check(instanceReady, "Vulkan 1.2 instance created using SDL extensions");

    if (instanceReady)
    {
        uint32_t physicalCount = 0;

        vkEnumeratePhysicalDevices(instance, &physicalCount, nullptr);

        std::array<VkPhysicalDevice, 16> physicalDevices{};

        const bool devicesReady = physicalCount > 0 && physicalCount <= physicalDevices.size()
                               && vkEnumeratePhysicalDevices(instance, &physicalCount, physicalDevices.data()) == VK_SUCCESS;

        results.Check(devicesReady, "physical Vulkan devices enumerated");

        for (int cycle = 0; cycle < 3 && devicesReady; ++cycle)
        {
            const bool resized =
                SDL_SetWindowSize(window.Handle(), 900 + cycle * 100, 600 + cycle * 50) && SDL_SyncWindow(window.Handle());

            VkSurfaceKHR surface    = VK_NULL_HANDLE;

            const bool surfaceReady = SDL_Vulkan_CreateSurface(window.Handle(), instance, nullptr, &surface);

            results.Check(resized && surfaceReady, "SDL Vulkan surface created after window resize");

            if (surfaceReady)
            {
                bool presentSupported = false;

                for (uint32_t deviceIndex = 0; deviceIndex < physicalCount; ++deviceIndex)
                {
                    uint32_t queueCount = 0;

                    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevices[deviceIndex], &queueCount, nullptr);

                    for (uint32_t queueIndex = 0; queueIndex < queueCount; ++queueIndex)
                    {
                        VkBool32 supported = VK_FALSE;

                        const VkResult queried =
                            vkGetPhysicalDeviceSurfaceSupportKHR(physicalDevices[deviceIndex], queueIndex, surface, &supported);

                        presentSupported = (queried == VK_SUCCESS && supported == VK_TRUE) || presentSupported;
                    }
                }

                results.Check(presentSupported, "Vulkan surface has a presentation capable queue");

                VkSurfaceCapabilitiesKHR capabilities{};

                const VkResult queried = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevices[0], surface, &capabilities);

                const zen::platform::WindowExtent pixels = window.GetFramebufferExtent();

                results.Check(queried == VK_SUCCESS && capabilities.currentExtent.width == pixels.width
                                  && capabilities.currentExtent.height == pixels.height,
                              "Vulkan surface extent matches actual framebuffer pixels");

                // SDL-created surfaces use ordinary Vulkan ownership; destroy before the native window.
                vkDestroySurfaceKHR(instance, surface, nullptr);
            }
        }

        vkDestroyInstance(instance, nullptr);
    }
}
} // namespace

int main()
{
    Results results;

    SDL_SetMainReady();

    const bool initialized = SDL_Init(SDL_INIT_VIDEO);

    results.Check(initialized, "SDL video subsystem initialized");

    if (initialized)
    {
        std::printf("INFO SDL=%d video=%s ImGui=%s\n", SDL_GetVersion(), SDL_GetCurrentVideoDriver(), IMGUI_VERSION);

        {
            ProbeWindow window;

            results.Check(window.Handle() != nullptr, "SDL Vulkan window uses engine window value types");

            if (window.Handle() != nullptr)
            {
                TitleBarRegion region;

                results.Check(SDL_SetWindowHitTest(window.Handle(), ProbeWindow::HitTest, &region),
                              "custom hit test installed");

                CheckNativeChrome(window, region, results);

                CheckWindowState(window, results);

                CheckImGui(window, results);

                CheckVulkan(window, results);

                results.Check(SDL_SetWindowHitTest(window.Handle(), nullptr, nullptr),
                              "hit test removed before region lifetime ends");
            }
        }

        // A second window after the first is destroyed checks application-owned SDL lifetime.
        {
            ProbeWindow secondWindow;

            results.Check(secondWindow.Handle() != nullptr, "window destruction leaves shared SDL subsystem usable");
        }

        SDL_Quit();
    }

    if (results.failed != 0)
    {
        std::printf("INFO last SDL error: %s\n", SDL_GetError());
    }

    std::printf("RESULT passed=%d failed=%d\n", results.passed, results.failed);

    return results.failed == 0 ? 0 : 1;
}
