#include "Graphics/RenderCore/V2/RenderResource.h"
#include "Graphics/RenderCore/V2/RenderDevice.h"

namespace zen::rc
{
TextureRD* TextureRD::Create(const TextureFormat& texFormat, std::string name)
{
    TextureRD* texture = new TextureRD(texFormat, std::move(name));
    return texture;
}

TextureRD* TextureRD::CreateProxy(TextureRD* baseTex,
                                  const TextureProxyFormat& proxyFormat,
                                  std::string name)
{
    TextureRD* texture = new TextureRD(baseTex, proxyFormat, std::move(name));
    return texture;
}

void TextureRD::Init(RenderDevice* device, rhi::TextureHandle handle)
{
    m_renderDevice     = device;
    m_handle           = std::move(handle);
    m_subResourceRange = device->GetTextureSubResourceRange(m_handle);
}

void TextureRD::Destroy()
{
    m_renderDevice->GetRHI()->DestroyTexture(m_handle);
    delete this;
}
} // namespace zen::rc