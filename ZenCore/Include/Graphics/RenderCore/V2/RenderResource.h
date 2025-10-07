#pragma once
#include "ObjectBase.h"
#include "RenderDevice.h"
#include "Graphics/RenderCore/V2/RenderCoreDefs.h"

namespace zen::rc
{
class RenderDevice;

class TextureRD : public ObjectBase
{
public:
    static TextureRD* Create(const TextureFormat& texFormat, std::string name);

    static TextureRD* CreateProxy(TextureRD* baseTex,
                                  const TextureProxyFormat& proxyFormat,
                                  std::string name);

    void Init(RenderDevice* device, rhi::TextureHandle handle);

    void DecreaseRefCount() override
    {
        if (IsProxy())
        {
            GetBaseTexture()->DecreaseRefCount();
        }
        ObjectBase::DecreaseRefCount();
    }

    rhi::TextureHandle GetHandle() const
    {
        return m_handle;
    }

    DataFormat GetFormat() const
    {
        return m_format;
    }

    uint32_t GetWidth() const
    {
        return m_width;
    }

    uint32_t GetHeight() const
    {
        return m_height;
    }

    uint32_t GetDepth() const
    {
        return m_depth;
    }

    const rhi::TextureSubResourceRange& GetTextureSubResourceRange() const
    {
        return m_subResourceRange;
    }
    TextureRD* GetBaseTexture() const
    {
        return m_baseTex;
    }

    std::string GetName() const
    {
        return m_name;
    }

    bool IsProxy() const
    {
        return m_isProxy;
    }

protected:
    void Destroy() final;

private:
    TextureRD(const TextureFormat& texFormat, std::string name) :
        m_name(std::move(name)),
        m_format(texFormat.format),
        m_width(texFormat.width),
        m_height(texFormat.height),
        m_depth(texFormat.depth),
        m_arrayLayers(texFormat.arrayLayers),
        m_mipmaps(texFormat.mipmaps)
    {}

    TextureRD(TextureRD* baseTex, const TextureProxyFormat& proxyFormat, std::string name) :
        m_name(std::move(name)),
        m_format(proxyFormat.format),
        m_arrayLayers(proxyFormat.arrayLayers),
        m_mipmaps(proxyFormat.mipmaps),
        m_baseTex(baseTex),
        m_isProxy(true)
    {
        m_baseTex->IncreaseRefCount();
    }

    RenderDevice* m_renderDevice{nullptr};
    rhi::TextureHandle m_handle;
    rhi::TextureSubResourceRange m_subResourceRange;
    std::string m_name;
    DataFormat m_format{DataFormat::eUndefined};
    uint32_t m_width{0};
    uint32_t m_height{0};
    uint32_t m_depth{1};
    uint32_t m_arrayLayers{1};
    uint32_t m_mipmaps{1};

    TextureRD* m_baseTex{nullptr};
    bool m_isProxy{false};
};
} // namespace zen::rc