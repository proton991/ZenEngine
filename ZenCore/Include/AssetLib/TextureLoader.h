#pragma once
#include <string>
#include <vector>
#include "Types.h"
#include "Templates/HeapVector.h"

namespace zen::asset
{
class TextureLoader
{
public:
    static TextureInfo LoadTexture2DFromFile(const std::string& filename);

    static void LoadTexture2DFromFile(const std::string& filename, TextureInfo* pOutTexInfo);

    // Linear RGBA pixels in +X, -X, +Y, -Y, +Z, -Z face order, using the engine's
    // inverted-Y source-cubemap convention (also used by skybox/IBL filtering). Accepts 2:1
    // Radiance HDR panoramas; output faces are capped at 512 pixels per side.
    static bool LoadHDRCubemap(const std::string& path, uint32_t& size, HeapVector<Vec4>& pixels, std::string& error);
};
} // namespace zen::asset
