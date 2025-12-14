// #include "Graphics/RenderCore/V2/RenderResource.h"
// #include "Graphics/RenderCore/V2/RenderDevice.h"
//
// namespace zen::rc
// {
// RHITexture* RHITexture::Create(const TextureFormat& texFormat, std::string name)
// {
//     RHITexture* texture = new RHITexture(texFormat, std::move(name));
//     return texture;
// }
//
// RHITexture* RHITexture::CreateProxy(RHITexture* baseTex,
//                                   const TextureProxyFormat& proxyFormat,
//                                   std::string name)
// {
//     RHITexture* texture = new RHITexture(baseTex, proxyFormat, std::move(name));
//     return texture;
// }
//
// void RHITexture::Init(RenderDevice* device, RHITexture* handle)
// {
//     m_renderDevice     = device;
//     m_handle           = handle;
//     m_subResourceRange = device->GetTextureSubResourceRange(m_handle);
// }
//
// void RHITexture::Destroy()
// {
//     m_renderDevice->GetRHI()->DestroyTexture(m_handle);
//     delete this;
// }
// } // namespace zen::rc