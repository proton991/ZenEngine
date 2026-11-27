#ifndef ZEN_LIGHT_PARAMETERS
#define ZEN_LIGHT_PARAMETERS
struct SceneLight
{
    vec4 positionRange;
    vec4 directionType;
    vec4 colorIntensity;
    vec4 coneShadow;
};
vec3 EvaluateLight(SceneLight light, vec3 position, out vec3 direction, out float distanceToLight)
{
    int type          = int(light.directionType.w);
    direction         = -light.directionType.xyz;
    distanceToLight   = 1e20;
    float attenuation = 1.0;
    if (type != 0)
    {
        vec3  delta            = light.positionRange.xyz - position;
        float distanceSquared  = dot(delta, delta);
        distanceToLight        = sqrt(distanceSquared);
        direction              = delta / max(distanceToLight, 1e-6);
        float relativeDistance = light.positionRange.w > 0.0 ? distanceToLight / light.positionRange.w : 0.0;
        float cutoff           = clamp(1.0 - pow(relativeDistance, 4.0), 0.0, 1.0);
        attenuation            = cutoff / max(distanceSquared, 1e-6);
        if (type == 2)
        {
            float angular  = clamp((dot(-direction, light.directionType.xyz) - light.coneShadow.y)
                                       / max(light.coneShadow.x - light.coneShadow.y, 0.001),
                                   0.0, 1.0);
            attenuation   *= angular * angular;
        }
    }
    return light.colorIntensity.rgb * light.colorIntensity.w * attenuation;
}
#endif
