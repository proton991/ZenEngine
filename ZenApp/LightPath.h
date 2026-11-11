#pragma once
#include "Graphics/RenderCore/V2/SceneLighting.h"
#include <string>

namespace zen::runtime
{
struct LightPathKey
{
    uint64_t frame{0};
    Vec3     position{0};
};
struct LightPath
{
    rc::LightId              id{0}; // Authored ID in the launch setup, not the runtime scene's ID.
    HeapVector<LightPathKey> keys;
};
// {"lightId": N, "keys": [{"frame": N, "position": [x,y,z]}, ...]}.
// Hold each key until the next one. Per-frame keys permit arbitrary deterministic motion.
bool ReadLightPath(const std::string& path, LightPath& result, std::string& error);
Vec3 LightPathPosition(const LightPath& path, uint64_t frame);
} // namespace zen::runtime
