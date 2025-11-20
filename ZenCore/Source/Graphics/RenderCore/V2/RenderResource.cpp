// #include "Graphics/RenderCore/V2/RenderResource.h"
// #include "Graphics/RenderCore/V2/RenderDevice.h"
//
// namespace zen::rc
// {
// rhi::RHITexture* rhi::RHITexture::Create(const TextureFormat& texFormat, std::string name)
// {
//     rhi::RHITexture* texture = new rhi::RHITexture(texFormat, std::move(name));
//     return texture;
// }
//
// rhi::RHITexture* rhi::RHITexture::CreateProxy(rhi::RHITexture* baseTex,
//                                   const TextureProxyFormat& proxyFormat,
//                                   std::string name)
// {
//     rhi::RHITexture* texture = new rhi::RHITexture(baseTex, proxyFormat, std::move(name));
//     return texture;
// }
//
// void rhi::RHITexture::Init(RenderDevice* device, rhi::RHITexture* handle)
// {
//     m_renderDevice     = device;
//     m_handle           = handle;
//     m_subResourceRange = device->GetTextureSubResourceRange(m_handle);
// }
//
// void rhi::RHITexture::Destroy()
// {
//     m_renderDevice->GetRHI()->DestroyTexture(m_handle);
//     delete this;
// }
// } // namespace zen::rc