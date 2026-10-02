#pragma once

struct GLFWwindow;
struct ImGuiContext;
struct ImDrawData;
struct ImFontAtlas;

namespace zen::ui
{
// Main/window thread only. GLFW events always reach ImGui, regardless of capture.
class UIContext
{
public:
    UIContext() = default;

    ~UIContext();

    UIContext(const UIContext&)            = delete;

    UIContext& operator=(const UIContext&) = delete;

    bool Init(GLFWwindow* window);

    void Destroy();

    void BeginFrame(float deltaSeconds, unsigned int targetWidth, unsigned int targetHeight);

    void EndFrame();

    ImFontAtlas& GetFonts();

    const ImDrawData* GetDrawData();

private:
    ImGuiContext* m_context{nullptr};
    ImGuiContext* m_previous{nullptr};
    bool          m_platformReady{false};
    bool          m_frameActive{false};
};
} // namespace zen::ui
