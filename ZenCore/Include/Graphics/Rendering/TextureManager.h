#pragma once
#include "Graphics/Val/Image.h"

namespace zen
{
class RenderContext;

struct TextureInfo
{
    uint32_t width{0};
    uint32_t height{0};
    VkFormat format{VK_FORMAT_UNDEFINED};
    bool     hasMipMap{false};
    // level 0 byte data
    std::vector<uint8_t> baseLevelData;
    // other levels
    std::vector<std::vector<uint8_t>> otherLeveData;
};

class TextureManager
{
public:
    TextureManager(val::Device& valDevice, RenderContext& renderContext) :
        m_valDevice(valDevice), m_renderContext(renderContext) {}

    val::Image* RequestTexture2D(const std::string& filename, bool requireMipmap = false);

private:
    TextureInfo LoadTexture2DFromFile(const std::string& filename);

    val::Device&   m_valDevice;
    RenderContext& m_renderContext;

    std::unordered_map<std::string, UniquePtr<val::Image>> m_cache;
};
} // namespace zen