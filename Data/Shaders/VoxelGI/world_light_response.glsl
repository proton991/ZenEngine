#ifndef ZEN_WORLD_LIGHT_RESPONSE
#define ZEN_WORLD_LIGHT_RESPONSE
#include "hybrid_common.glsl"
vec3 ChangedLightIrradiance(SceneLight light, vec3 position, vec3 geometric, out vec3 axis)
{
    float distance;
    vec3  power = EvaluateLight(light, position, axis, distance);
    return power * (max(dot(geometric, axis), 0.0) / HYBRID_PI);
}

// Compare visibility on the same surface under both light states, only across edits since this cell was sampled.
float WorldLightResponse(uint elapsed, vec3 position, vec3 geometric, vec3 previous, uint slot, out vec3 delta)
{
    delta = vec3(0);
    if (editLog.w == 0u || responseLog.x == 0u || gi.lighting.x == 0.0 || worldCache.schedule.x - responseLog.x >= elapsed)
    {
        return 0.0;
    }
    float change = 0.0;
    if (editLog.z != 0u && worldCache.schedule.x - editLog.z < elapsed)
    {
        return 1.0;
    }
    for (uint i = 0u; i < editLog.y; ++i)
    {
        WorldLightEdit edit = lightEdits[(editLog.x + kWorldEditCapacity - 1u - i) % kWorldEditCapacity];
        if (worldCache.schedule.x - edit.stamp.x >= elapsed)
        {
            break;
        }
        if (worldCache.lightCount.w != 0u)
        {
            // A class change removes a light from one estimator and adds it to the other.
            if (edit.before.coneShadow.w != 0.0)
            {
                edit.before.colorIntensity.w = 0.0;
            }
            if (edit.after.coneShadow.w != 0.0)
            {
                edit.after.colorIntensity.w = 0.0;
            }
        }
        vec3 oldAxis, newAxis;
        vec3 oldPower = ChangedLightIrradiance(edit.before, position, geometric, oldAxis);
        vec3 newPower = ChangedLightIrradiance(edit.after, position, geometric, newAxis);
        if (max(HybridLuminance(oldPower), HybridLuminance(newPower)) > 0.0)
        {
            bool  oldShadow  = gi.limits.z > .5 && edit.before.coneShadow.z > 0.0 && any(greaterThan(oldPower, vec3(0)));
            bool  newShadow  = gi.limits.z > .5 && edit.after.coneShadow.z > 0.0 && any(greaterThan(newPower, vec3(0)));
            float before     = oldShadow ? HardwareLightVisibility(position, geometric, edit.before) : 1.0;
            bool  sameRay    = oldShadow && edit.before.positionRange.xyz == edit.after.positionRange.xyz && oldAxis == newAxis
                            && edit.before.directionType.w == edit.after.directionType.w;
            float after      = newShadow ? (sameRay ? before : HardwareLightVisibility(position, geometric, edit.after)) : 1.0;
            oldPower        *= before;
            newPower        *= after;
#ifdef WORLD_PROJECT_CORRECTION
            WorldProjectCorrection(slot, oldAxis, oldPower / max(dot(geometric, oldAxis), 1e-8), newAxis,
                                   newPower / max(dot(geometric, newAxis), 1e-8));
#endif
            // Normalize by the cell's previous illumination as well as the changed source.
            delta           += newPower - oldPower;
            float reference  = max(HybridLuminance(previous), max(HybridLuminance(newPower), HybridLuminance(oldPower)));
            change          += HybridLuminance(abs(newPower - oldPower)) / max(reference, 1e-6);
        }
    }
    return min(change, 1.0);
}
#endif
