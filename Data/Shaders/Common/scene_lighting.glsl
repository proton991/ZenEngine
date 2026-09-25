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
    vec4 viewPosition;
    vec4 lightInfo;
    vec4 environment;
} sceneUbo;

// Irradiance and prefiltered maps already use world orientation.
vec3 EnvironmentDirection(vec3 direction)
{
    float c = cos(sceneUbo.environment.y);
    float s = sin(sceneUbo.environment.y);
    return vec3(c * direction.x - s * direction.z, direction.y, s * direction.x + c * direction.z);
}

vec3 EnvironmentSourceDirection(vec3 direction)
{
    // Source cubemaps use inverted Y. filtercube.vert already converts the IBL maps.
    vec3 rotated = EnvironmentDirection(direction);
    return vec3(rotated.x, -rotated.y, rotated.z);
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
        float relativeDistance = distanceToLight / light.positionRange.w;
        float cutoff = clamp(1.0 - pow(relativeDistance, 4.0), 0.0, 1.0);
        attenuation = cutoff * cutoff / (distanceSquared + 0.01);
        if (type == 2)
        {
            attenuation *= smoothstep(light.coneShadow.y, light.coneShadow.x,
                                      dot(-direction, light.directionType.xyz));
        }
    }
    return light.colorIntensity.rgb * light.colorIntensity.w * attenuation;
}
#endif
