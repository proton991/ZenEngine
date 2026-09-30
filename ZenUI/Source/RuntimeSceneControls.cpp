#include "UI/RuntimeSceneControls.h"
#include <cmath>

namespace zen::ui
{
namespace
{
bool Finite(const Vec3& value)
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}
} // namespace

bool ValidateRuntimeSceneSettings(const RuntimeSceneSettings& settings)
{
    bool valid = Finite(settings.cameraPosition) && Finite(settings.orbitCenter) &&
        std::isfinite(settings.environmentIntensity) && settings.environmentIntensity >= 0 &&
        std::isfinite(settings.environmentRotation) && std::isfinite(settings.markerSize) &&
        settings.markerSize > 0 && settings.lightCount <= rc::MaxSceneLights &&
        settings.animatedLight < rc::MaxSceneLights && std::isfinite(settings.orbitRadius) &&
        settings.orbitRadius >= 0 && std::isfinite(settings.orbitSpeed) &&
        std::abs(settings.orbitSpeed) <= 3600;

    for (uint32_t index = 0; valid && index < settings.lightCount; ++index)
    {
        valid = rc::SceneLights::Validate(settings.lights[index]);
    }

    if (valid && settings.animationEnabled)
    {
        valid = settings.animatedLight < settings.lightCount &&
            settings.lights[settings.animatedLight].type != rc::SceneLightType::eDirectional;
    }

    return valid;
}

rc::SceneLight MergeRuntimeLightEdit(const rc::SceneLight& current,
                                     const rc::SceneLight& previous,
                                     const rc::SceneLight& requested)
{
    rc::SceneLight result = requested;

    // An orbit may have advanced while a manual draft or slider edit was pending.
    if (requested.position == previous.position)
    {
        result.position = current.position;
    }

    return result;
}
} // namespace zen::ui
