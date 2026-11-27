#ifndef ZEN_SCENE_LIGHTING_GLSL
#define ZEN_SCENE_LIGHTING_GLSL
const int MAX_SCENE_LIGHTS = 32;
#include "light_parameters.glsl"
layout(set = 2, binding = 0, std140) uniform uSceneData
{
    SceneLight lights[MAX_SCENE_LIGHTS];
    vec4       viewPosition; // Eye position, w=0 for orthographic and w=1 for perspective.
    vec4       lightInfo;    // Enabled light count, followed by world camera-backward direction.
    vec4       environment;
    vec4       environmentOrientation;
    vec4       environmentProperties;
    SceneLight cameraLight;
    uvec4      lightIds[(MAX_SCENE_LIGHTS + 1) / 2];
}
sceneUbo;

// Irradiance and prefiltered maps already use world orientation.
vec3 EnvironmentDirection(vec3 direction, float angle, vec4 q)
{
    float c       = cos(angle);
    float s       = sin(angle);
    vec3  rotated = vec3(c * direction.x - s * direction.z, direction.y, s * direction.x + c * direction.z);
    return rotated + 2.0 * cross(q.xyz, cross(q.xyz, rotated) + q.w * rotated);
}

vec3 EnvironmentDirection(vec3 direction)
{
    return EnvironmentDirection(direction, sceneUbo.environment.y, sceneUbo.environmentOrientation);
}

vec3 EnvironmentSourceDirection(vec3 direction)
{
    // Source cubemaps use inverted Y. filtercube.vert already converts the IBL maps.
    vec3 rotated = EnvironmentDirection(direction);
    return vec3(rotated.x, sceneUbo.environmentProperties.x > 0.5 ? rotated.y : -rotated.y, rotated.z);
}

#endif
