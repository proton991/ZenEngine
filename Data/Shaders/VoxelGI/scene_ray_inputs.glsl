#ifndef ZEN_SCENE_RAY_INPUTS
#define ZEN_SCENE_RAY_INPUTS
#include "scene_ray_geometry.glsl"
layout(set = 6, binding = 0) uniform accelerationStructureEXT sceneAccelerationStructure;
layout(std430, set = 6, binding = 1) readonly buffer RayGeometryMetadata
{
    uvec4 rayGeometries[];
};
layout(std430, set = 6, binding = 2) readonly buffer RayInstanceMetadata
{
    uvec4 rayInstances[];
};
// A retained geometry snapshot must not multiply descriptors by the uniform ring slot.
layout(std140, set = 2, binding = 3) uniform uRayQueryData
{
    vec4  minimum;
    vec4  maximum;
    uvec4 generation;
}
rayScene;
// Every ray-query pass binds the secondary-hit streams (SceneRayQuery::BindInputs), so each
// ray-query shader declares them, including those that never shade a hit.
#include "ray_hit_material.glsl"

uvec4 RayTriangle(uint instance, uint geometry, uint primitive)
{
    uvec4 object = rayInstances[instance];
    uvec4 mesh   = rayGeometries[object.y + geometry];
    return uvec4(mesh.x + 3u * primitive, object.x, mesh.y, instance);
}

#endif
