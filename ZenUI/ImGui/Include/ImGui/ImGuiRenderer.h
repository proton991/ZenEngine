#pragma once
#include "UI/UIRenderer.h"
#include "ImGui/ImGuiDrawPacket.h"

namespace zen::ui
{
class ImGuiRenderer
{
public:
    explicit ImGuiRenderer(rc::RenderDevice& device);

    bool Init(ImFontAtlas& fonts);

    void Destroy();

    bool BuildRenderGraph(rc::RenderGraph& graph, RHIViewport& viewport, const ImDrawData& data);

    UIRenderer& GetRenderer();

    static ImTextureID GetTextureID(UITextureHandle handle);

private:
    rc::RenderDevice& m_device;
    UIRenderer        m_renderer;
    RHITexture*       m_fontTexture{nullptr};
    UITextureHandle   m_fontHandle;
};
} // namespace zen::ui
