#pragma once
#include "Graphics/Val/Image.h"

namespace zen
{
class RenderContext;

class TextureManager
{
public:
    TextureManager(const val::Device& valDevice, RenderContext& renderContext) :
        m_valDevice(valDevice), m_renderContext(renderContext)
    {}

    val::Image* RequestTexture2D(const std::string& filename, bool requireMipmap = false);

private:
    const val::Device& m_valDevice;
    RenderContext&     m_renderContext;

    std::unordered_map<std::string, UniquePtr<val::Image>> m_cache;
};
} // namespace zen