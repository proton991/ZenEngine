#ifndef ZEN_HARDWARE_VISIBILITY
#define ZEN_HARDWARE_VISIBILITY
#include "hardware_ray_interval.glsl"
bool HardwareOccluded(vec3 origin, vec3 direction, float maximumDistance);
// Local rays start at the light so enclosing fixtures terminate traversal early.
float HardwareLightVisibility(vec3 position, vec3 normal, SceneLight light)
{
    vec3  direction;
    float distanceToLight;
    EvaluateLight(light, position, direction, distanceToLight);
    vec3 origin = HybridTraceOrigin(position, dot(normal, direction) >= 0.0 ? normal : -normal, 0.0);
    if (int(light.directionType.w) != 0)
    {
        vec3 delta      = origin - light.positionRange.xyz;
        distanceToLight = length(delta);
        direction       = delta / max(distanceToLight, 1e-20);
        distanceToLight = uintBitsToFloat(floatBitsToUint(distanceToLight) - uint(distanceToLight > 0.0));
        origin          = light.positionRange.xyz;
    }
    return distanceToLight > 0.0 && HardwareOccluded(origin, direction, distanceToLight) ? 0.0 : 1.0;
}
#endif
