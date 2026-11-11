#ifndef ZEN_LIGHT_OCCLUDERS
#define ZEN_LIGHT_OCCLUDERS
// This table stores candidate triangles, never an interpolated visibility result.
// A cache hit must intersect the receiver's actual finite segment, strictly inside an opaque triangle.
const int LIGHT_OCCLUDER_SIZE = 16;
struct LightOccluder
{
    vec4 origin; // xyz world vertex, w valid
    vec4 edge1;
    vec4 edge2;
};
ivec2 LightOccluderAxes(int axis)
{
    return axis == 0 ? ivec2(1, 2) : (axis == 1 ? ivec2(0, 2) : ivec2(0, 1));
}
uint LightOccluderIndex(int light, int face, ivec2 pixel)
{
    return uint(((light * 6 + face) * LIGHT_OCCLUDER_SIZE + pixel.y) * LIGHT_OCCLUDER_SIZE + pixel.x);
}
#ifndef BUILD_LIGHT_OCCLUDERS
layout(std430, set = 6, binding = 6) readonly buffer LightOccluders
{
    LightOccluder lightOccluders[];
};
bool CachedLightOccluded(int lightIndex, vec3 position, vec3 normal, SceneLight light)
{
    bool occluded = false;
    if (lightIndex < MAX_SCENE_LIGHTS && int(light.directionType.w) != 0)
    {
        vec3  toLight   = light.positionRange.xyz - position;
        vec3  receiver  = HybridTraceOrigin(position, dot(normal, toLight) >= 0.0 ? normal : -normal, 0.0);
        vec3  delta     = receiver - light.positionRange.xyz;
        vec3  magnitude = abs(delta);
        int   axis      = magnitude.x >= magnitude.y && magnitude.x >= magnitude.z ? 0 : (magnitude.y >= magnitude.z ? 1 : 2);
        int   face      = axis * 2 + int(delta[axis] < 0.0);
        ivec2 axes      = LightOccluderAxes(axis);
        vec2  uv        = vec2(delta[axes.x], delta[axes.y]) / max(magnitude[axis], 1e-30);
        ivec2 pixel     = clamp(ivec2((uv * 0.5 + 0.5) * float(LIGHT_OCCLUDER_SIZE)), ivec2(0), ivec2(LIGHT_OCCLUDER_SIZE - 1));
        LightOccluder triangle = lightOccluders[LightOccluderIndex(lightIndex, face, pixel)];
        if (triangle.origin.w > 0.0)
        {
            vec3  crossDirection = cross(delta, triangle.edge2.xyz);
            float determinant    = dot(triangle.edge1.xyz, crossDirection);
            float scale          = length(delta) * length(cross(triangle.edge1.xyz, triangle.edge2.xyz));
            if (abs(determinant) > max(scale * 1e-6, 1e-20))
            {
                vec3  offset      = light.positionRange.xyz - triangle.origin.xyz;
                vec3  crossOffset = cross(offset, triangle.edge1.xyz);
                float u           = dot(offset, crossDirection) / determinant;
                float v           = dot(delta, crossOffset) / determinant;
                float t           = dot(triangle.edge2.xyz, crossOffset) / determinant;
                // Borderline hits use the original HW query, including nearly parallel rays and endpoints.
                float epsilon =
                    1e-5 * max(max(length(receiver), length(light.positionRange.xyz)), 1.0) / max(length(delta), 1e-20);
                occluded = u > 1e-4 && v > 1e-4 && u + v < 1.0 - 1e-4 && t > epsilon && t < 1.0 - epsilon;
            }
        }
    }
    return occluded;
}
#endif
#endif
