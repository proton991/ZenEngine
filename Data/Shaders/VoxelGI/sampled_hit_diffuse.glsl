#ifndef ZEN_SAMPLED_HIT_DIFFUSE
#define ZEN_SAMPLED_HIT_DIFFUSE
float HitDiffuseVisibility(HybridRayResult hit, SceneLight light, int index)
{
    float visibility = 1.0;
#ifndef HYBRID_ABLATE_SHADOW
    bool occluded = false;
#    ifdef ZEN_LIGHT_OCCLUDERS
    occluded =
        CachedLightOccluded(index == int(sceneUbo.lightInfo.x) ? MAX_SCENE_LIGHTS : index, hit.position, hit.normal, light);
#    endif
#    ifdef HYBRID_FIELD_TRAVERSAL
    visibility = occluded ? 0.0 : NativeFieldVisibility(hit.position, hit.normal, light);
#    else
    visibility = occluded ? 0.0 : HardwareLightVisibility(hit.position, hit.normal, light);
#    endif
#endif
    return visibility;
}
// Irradiance/pi from analytic lights and, at a path endpoint, visible sky.
vec3 SampledHitDiffuse(HybridRayResult hit, vec3 normal, uint seed, bool includeSky, bool exactLights)
{
    vec3 diffuse = vec3(0);
    // Lights without a visibility query are summed exactly. Lights that need one share a single
    // RT shadow query: a weighted reservoir picks one in proportion to its unshadowed luminance,
    // and its visible contribution is divided by that probability. The estimate is unbiased,
    // costs one query whatever the light count, and is exact when a single light reaches the hit.
    uint  stream         = HybridHash(seed ^ 0x2c1b3c6du);
    float shadowedWeight = 0.0, chosenWeight = 0.0;
    int   chosen         = -1;
    vec3  chosenRadiance = vec3(0);
    int   lightCount     = (int(sceneUbo.lightInfo.x) + 1) * int(gi.lighting.x);
#ifdef HYBRID_ABLATE_LIGHTS
    lightCount = min(lightCount, 1);
#endif
    for (int i = 0; i < lightCount; ++i)
    {
        SceneLight light = i == int(sceneUbo.lightInfo.x) ? sceneUbo.cameraLight : sceneUbo.lights[i];
        vec3       direction;
        float      distanceToLight;
        vec3       incoming = EvaluateLight(light, hit.position, direction, distanceToLight);
        float      cosine   = max(dot(normal, direction), 0.0);
        if (cosine > 0.0 && dot(hit.normal, direction) > 0.0 && any(greaterThan(incoming, vec3(0))))
        {
            vec3 radiance = incoming * cosine / HYBRID_PI;
            if (gi.limits.z > 0.5 && (i == int(sceneUbo.lightInfo.x) || light.coneShadow.z > 0.0))
            {
                if (exactLights)
                {
                    diffuse += radiance * HitDiffuseVisibility(hit, light, i);
                }
                else
                {
                    float weight    = dot(radiance, vec3(0.2126, 0.7152, 0.0722));
                    float u         = float(HybridHash(stream + uint(i)) >> 8) / 16777216.0;
                    shadowedWeight += weight;
                    if (weight > 0.0 && u * shadowedWeight < weight)
                    {
                        chosen         = i;
                        chosenWeight   = weight;
                        chosenRadiance = radiance;
                    }
                }
            }
            else
            {
                diffuse += radiance;
            }
        }
    }
    if (chosen >= 0)
    {
        SceneLight light  = chosen == int(sceneUbo.lightInfo.x) ? sceneUbo.cameraLight : sceneUbo.lights[chosen];
        diffuse          += chosenRadiance * (shadowedWeight / chosenWeight) * HitDiffuseVisibility(hit, light, chosen);
    }
#ifndef HYBRID_NO_GUIDE
#    ifdef HYBRID_COUNT_RAYS
    issuedRayPurpose = 2u;
#    endif
    float skyScale = sceneUbo.environment.x * sceneUbo.environment.z * gi.lighting.y;
    if (includeSky && skyScale > 0.0)
    {
        // Independent counter dimensions avoid correlating the secondary light direction with
        // the primary bounce direction. The seed changes for each ray and successful frame.
        vec3 u =
            vec3(uvec3(HybridHash(seed ^ 0xa511e9b3u), HybridHash(seed ^ 0x63d83595u), HybridHash(seed ^ 0x9e3779b9u)) >> 8)
            / 16777216.0;
        float pdf;
#    ifdef HYBRID_FIELD_TRAVERSAL
        vec3 direction = HybridCosineSample(normal, u.xy, pdf);
#    else
        vec3 direction = HybridSkySample(normal, u.xy, u.z < HYBRID_ENVIRONMENT_SHARE, pdf);
#    endif
        float cosine = max(dot(normal, direction), 0.0);
        if (cosine > 0.0 && dot(hit.normal, direction) > 0.0 && pdf > 0.0)
        {
            vec3 origin = HybridTraceOrigin(hit.position, hit.normal, 0.0);
#    ifdef HYBRID_FIELD_TRAVERSAL
            HybridRayResult skyHit  = NativeTraceRay(hit.position, hit.normal, direction, 1e20, false);
            bool            visible = skyHit.valid && !skyHit.hit;
#    else
            bool visible = !HardwareOccluded(origin, direction, 1e20);
#    endif
            if (visible)
            {
                diffuse += textureLod(coneEnvironmentMap, EnvironmentDirection(direction), 0).rgb
                         * (skyScale * cosine / (HYBRID_PI * pdf));
            }
        }
    }
#endif
    return diffuse;
}
#endif
