#pragma once
#include "Math/Math.h"
#include "Graphics/RenderCore/V2/SceneLighting.h"
#include <string>

namespace zen::runtime
{
enum class PathTarget
{
    LightPosition,
    Light,
    CameraLight,
    Environment,
    Instance,
    Material,
    Animation
};
struct LightPathKey
{
    uint64_t frame{0};
    Vec3     position{0};
    Vec3     color{1}, emission{0};
    float    intensity{1}, rotationDegrees{0}, seconds{0};
    bool     enabled{true};
};
struct LightPath
{
    PathTarget               target{PathTarget::LightPosition};
    uint32_t                 index{0};
    rc::LightId              id{0}; // Authored ID in the launch setup, not the runtime scene's ID.
    HeapVector<LightPathKey> keys;
};
// Scene paths select a target and absolute poses. Legacy position-only light paths select by lightId.
// Hold each key until the next one. Per-frame keys permit arbitrary deterministic motion.
bool                ReadLightPath(const std::string& path, LightPath& result, std::string& error);
const LightPathKey& LightPathPose(const LightPath& path, uint64_t frame);
} // namespace zen::runtime
