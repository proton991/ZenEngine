#ifndef ZEN_SCENE_LIGHTING_GLSL
#define ZEN_SCENE_LIGHTING_GLSL
const int MAX_SCENE_LIGHTS = 32;
struct SceneLight
{
    vec4 positionRange;
    vec4 directionType;
    vec4 colorIntensity;
    vec4 coneShadow;
};
layout(set = 2, binding = 0, std140) uniform uSceneData
{
    SceneLight lights[MAX_SCENE_LIGHTS];
    vec4 viewPosition; // Eye position, w=0 for orthographic and w=1 for perspective.
    vec4 lightInfo;    // Enabled light count, followed by world camera-backward direction.
    vec4 environment;
    vec4 environmentOrientation;
    vec4 environmentProperties;
} sceneUbo;

// Irradiance and prefiltered maps already use world orientation.
vec3 EnvironmentDirection(vec3 direction)
{
    float c = cos(sceneUbo.environment.y);
    float s = sin(sceneUbo.environment.y);
    vec3 rotated = vec3(c * direction.x - s * direction.z, direction.y, s * direction.x + c * direction.z);
    vec4 q = sceneUbo.environmentOrientation;
    return rotated + 2.0 * cross(q.xyz, cross(q.xyz, rotated) + q.w * rotated);
}

vec3 EnvironmentSourceDirection(vec3 direction)
{
    // Source cubemaps use inverted Y. filtercube.vert already converts the IBL maps.
    vec3 rotated = EnvironmentDirection(direction);
    return vec3(rotated.x, sceneUbo.environmentProperties.x > 0.5 ? rotated.y : -rotated.y, rotated.z);
}

vec3 EvaluateLight(SceneLight light, vec3 position, out vec3 direction, out float distanceToLight)
{
    int type = int(light.directionType.w);
    direction = -light.directionType.xyz;
    distanceToLight = 1e20;
    float attenuation = 1.0;
    if (type != 0)
    {
        vec3 delta = light.positionRange.xyz - position;
        float distanceSquared = dot(delta, delta);
        distanceToLight = sqrt(distanceSquared);
        direction = delta / max(distanceToLight, 1e-6);
        float relativeDistance = light.positionRange.w > 0.0 ? distanceToLight / light.positionRange.w : 0.0;
        float cutoff = clamp(1.0 - pow(relativeDistance, 4.0), 0.0, 1.0);
        attenuation = cutoff / max(distanceSquared, 1e-6);
        if (type == 2)
        {
            float angular = clamp((dot(-direction, light.directionType.xyz) - light.coneShadow.y) /
                max(light.coneShadow.x - light.coneShadow.y, 0.001), 0.0, 1.0);
            attenuation *= angular * angular;
        }
    }
    return light.colorIntensity.rgb * light.colorIntensity.w * attenuation;
}
#endif
