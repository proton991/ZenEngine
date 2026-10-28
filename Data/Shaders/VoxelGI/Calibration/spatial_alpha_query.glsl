#include "../hybrid_provider.glsl"
layout(std430, set = 4, binding = 0) readonly buffer Rays
{
    vec4 rays[];
};
layout(std430, set = 4, binding = 1) writeonly buffer Results
{
    vec4 values[];
};
void TraceSpatialAlpha(uint index)
{
    vec4           start     = rays[2u * index];
    vec4           direction = rays[2u * index + 1u];
    bool           closest   = direction.w > 0.5;
    HardwareRayHit hit       = HardwareQuery(start.xyz, direction.xyz, start.w, closest);
    uint           material  = hit.material;
    uint           instance  = hit.instance;
    // Any-hit queries promise visibility, not which accepted intersection wins.
    values[index] = hit.ray.hit
                      ? (closest ? vec4(1, length(hit.ray.position - start.xyz), float(material + 1u), float(instance + 1u))
                                 : vec4(1, 0, 0, 0))
                      : vec4(0);
}
