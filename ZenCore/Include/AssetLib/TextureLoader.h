#pragma once
#include <string>
#include <vector>
#include "Types.h"

namespace zen::asset
{
class TextureLoader
{
public:
    static TextureInfo LoadTexture2DFromFile(const std::string& filename);
    static void LoadTexture2DFromFile(const std::string& filename, TextureInfo* outTexInfo);
};
} // namespace zen::asset