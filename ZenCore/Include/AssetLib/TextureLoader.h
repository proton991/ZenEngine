#pragma once
#include <string>
#include <vector>
#include "Common/Types.h"


namespace zen
{
struct TextureInfo
{
    uint32_t width{0};
    uint32_t height{0};
    Format format{Format::UNDEFINED};
    bool hasMipMap{false};
    // level 0 byte data
    std::vector<uint8_t> baseLevelData;
    // other levels
    std::vector<std::vector<uint8_t>> otherLeveData;
};

class TextureLoader
{
public:
    static TextureInfo LoadTexture2DFromFile(const std::string& filename);
};
} // namespace zen