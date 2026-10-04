#pragma once

namespace zen::platform
{
class NativeWindow;
}
struct ImGuiContext;
struct ImDrawData;
struct ImFontAtlas;

namespace zen::ui
{
struct UIContextOptions
{
    bool docking{false};
    // Multiplies fontSize and style sizes.
    float scale{1.0f};
    bool  followDisplayScale{true};
    // Unscaled pixel size of the adapter's default font.
    float fontSize{13.0f};
};

// Main/window thread only. Native events always reach ImGui, regardless of capture.
class UIContext
{
public:
    UIContext() = default;

    ~UIContext();

    UIContext(const UIContext&)            = delete;

    UIContext& operator=(const UIContext&) = delete;

    bool Init(platform::NativeWindow& window, const UIContextOptions& options = {});

    void Destroy();

    void BeginFrame(float deltaSeconds, unsigned int targetWidth, unsigned int targetHeight);

    void EndFrame();

    ImFontAtlas& GetFonts();

    const ImDrawData* GetDrawData();

private:
    platform::NativeWindow* m_window{nullptr};
    ImGuiContext*           m_context{nullptr};
    ImGuiContext*           m_previous{nullptr};
    bool                    m_platformReady{false};
    bool                    m_frameActive{false};
    bool                    m_followDisplayScale{true};
    float                   m_monitorScale{1.0f};
};
} // namespace zen::ui
