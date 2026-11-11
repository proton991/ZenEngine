#include "../Common/opacity_micromap.glsl"
#define HYBRID_HARDWARE_PROVIDER
#include "../VoxelGI/hybrid_provider.glsl"
#define BUILD_LIGHT_OCCLUDERS
#include "light_occluders.glsl"
#include "../VoxelGI/gi_ray_totals.glsl"
layout(local_size_x = 8, local_size_y = 8) in;
layout(std430, set = 6, binding = 6) writeonly buffer LightOccluders
{
    LightOccluder lightOccluders[];
};
void main()
{
    ivec2         pixel      = ivec2(gl_GlobalInvocationID.xy);
    int           face       = int(gl_GlobalInvocationID.z) % 6;
    int           lightIndex = int(gl_GlobalInvocationID.z) / 6;
    LightOccluder result     = LightOccluder(vec4(0), vec4(0), vec4(0));
    SceneLight    light      = sceneUbo.lights[lightIndex];
    if (lightIndex < int(sceneUbo.lightInfo.x) && int(light.directionType.w) != 0 && light.coneShadow.z > 0.0)
    {
        int   axis         = face / 2;
        ivec2 axes         = LightOccluderAxes(axis);
        vec2  uv           = (vec2(pixel) + 0.5) / float(LIGHT_OCCLUDER_SIZE) * 2.0 - 1.0;
        vec3  direction    = vec3(0);
        direction[axis]    = face % 2 == 0 ? 1.0 : -1.0;
        direction[axes.x]  = uv.x;
        direction[axes.y]  = uv.y;
        HardwareRayHit hit = HardwareQuery(light.positionRange.xyz, normalize(direction), 1e20, true);
        if (hit.ray.hit && materialData[hit.material].surfaceProperties.y == 0.0)
        {
            vec3 a        = RaySurfacePosition(hit.ray.triangle, vec2(0));
            result.origin = vec4(a, 1);
            result.edge1  = vec4(RaySurfacePosition(hit.ray.triangle, vec2(1, 0)) - a, 0);
            result.edge2  = vec4(RaySurfacePosition(hit.ray.triangle, vec2(0, 1)) - a, 0);
        }
    }
    lightOccluders[LightOccluderIndex(lightIndex, face, pixel)] = result;
#ifdef GI_COUNT_RAY_SLOT
    RecordGIRays();
#endif
}
