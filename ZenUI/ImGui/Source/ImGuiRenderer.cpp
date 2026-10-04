#include "ImGui/ImGuiRenderer.h"

namespace zen::ui
{
ImGuiRenderer::ImGuiRenderer(rc::RenderDevice& device) : m_device(device), m_renderer(device) {}

bool ImGuiRenderer::Init(ImFontAtlas& fonts)
{
    unsigned char* pixels = nullptr;

    int width             = 0;

    int height            = 0;

    fonts.GetTexDataAsRGBA32(&pixels, &width, &height);

    bool valid =
        m_renderer.Init() && pixels != nullptr && width > 0 && height > 0 && uint64_t(width) * height * 4 <= UINT32_MAX;

    if (valid)
    {
        rc::TextureFormat format;

        format.width                 = uint32_t(width);

        format.height                = uint32_t(height);

        format.depth                 = 1;

        format.format                = DataFormat::eR8G8B8A8UNORM;

        m_fontTexture                = m_device.CreateTextureSampled(format, {.copyUsage = true}, "UIFont");

        RHISamplerCreateInfo sampler = RHISamplerCreateInfo::CreateLinearRepeat();

        sampler.repeatU = sampler.repeatV = sampler.repeatW = RHISamplerRepeatMode::eClampToEdge;

        m_fontHandle = m_renderer.RegisterTexture(m_fontTexture, m_device.CreateSampler(sampler));

        valid        = m_fontHandle.value != 0;

        if (valid)
        {
            RHIBufferTextureCopyRegion region{};

            region.textureSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);

            region.textureSubresources.layerCount = 1;

            region.textureSize                    = {width, height, 1};

            m_device.UpdateTexture(m_fontTexture, MakeVecView(&region, 1), uint32_t(width * height * 4), pixels);

            fonts.SetTexID(GetTextureID(m_fontHandle));
        }
    }

    if (!valid)
    {
        Destroy();
    }

    return valid;
}

void ImGuiRenderer::Destroy()
{
    m_renderer.Destroy();

    m_device.DestroyTexture(m_fontTexture);

    m_fontTexture = nullptr;

    m_fontHandle  = {};
}

bool ImGuiRenderer::BuildRenderGraph(rc::RenderGraph& graph, RHIViewport& viewport, const ImDrawData& data)
{
    UIDrawPacket packet;

    const UIRenderTarget target{viewport.GetColorBackBuffer(), viewport.GetWidth(), viewport.GetHeight()};

    return BuildUIDrawPacket(data, target.width, target.height, packet) && m_renderer.BuildRenderGraph(graph, target, packet);
}

UIRenderer& ImGuiRenderer::GetRenderer()
{
    return m_renderer;
}

ImTextureID ImGuiRenderer::GetTextureID(UITextureHandle handle)
{
    return static_cast<ImTextureID>(handle.value);
}
} // namespace zen::ui
