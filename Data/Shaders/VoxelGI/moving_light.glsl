#ifndef ZEN_GI_MOVING_LIGHT
#define ZEN_GI_MOVING_LIGHT
// Shading and prediction evaluate the same secondary surface. The ball has a finite radius.
vec3 MovingLightDiffuse(SceneLight light, vec3 position, vec3 geometric, vec3 normal)
{
    vec3  direction;
    float distance;
    vec3  power = EvaluateLight(light, position, direction, distance);
    if (light.coneShadow.w > 0.0)
    {
        float radius2  = light.coneShadow.w * light.coneShadow.w;
        power         *= min(1.0, max(distance * distance, 1e-6) / radius2);
    }
    vec3 diffuse = power * (max(dot(normal, direction), 0.0) / HYBRID_PI);
    if (dot(geometric, direction) <= 0.0)
    {
        diffuse = vec3(0);
    }
    if (any(greaterThan(diffuse, vec3(0))) && gi.limits.z > .5 && light.coneShadow.z > 0.0)
    {
        diffuse *= HardwareLightVisibility(position, geometric, light);
    }
    return diffuse;
}
uint MovingEditIndex(uint offset)
{
    return (movingLog.x + kWorldEditCapacity - 1u - offset) % kWorldEditCapacity;
}
vec3 MovingLighting(vec3 position, vec3 geometric, vec3 normal, uint elapsed, out vec3 delta)
{
    uint edits = 0u;
    while (elapsed != 0u && edits < movingLog.y && WorldMovingHistoryComplete(elapsed)
           && worldCache.schedule.x - movingEdits[MovingEditIndex(edits)].stamp.x < elapsed)
    {
        ++edits;
    }
    delta      = vec3(0);
    vec3 value = vec3(0);
    for (uint entry = 0u; gi.lighting.x != 0.0 && entry < worldCache.surfaceLog.w; ++entry)
    {
        uint       i        = worldCache.movingLightIndices[entry / 4u][entry % 4u];
        bool       camera   = i == uint(sceneUbo.lightInfo.x);
        SceneLight light    = camera ? sceneUbo.cameraLight : sceneUbo.lights[i];
        vec3       current  = MovingLightDiffuse(light, position, geometric, normal);
        value              += current;
        if (edits != 0u && any(greaterThan(current, vec3(0))))
        {
            uvec2 id = uvec2(0);
            if (!camera)
            {
                uvec4 packed = sceneUbo.lightIds[i / 2u];
                id           = (i & 1u) == 0u ? packed.xy : packed.zw;
            }
            for (uint j = 0u; j < edits; ++j)
            {
                if (movingEdits[MovingEditIndex(j)].stamp.yz == id)
                {
                    delta += current;
                    break;
                }
            }
        }
    }
    // Intermediate poses telescope on the same surface. Reuse the live shadow result above
    // and evaluate only the oldest pose of each changed mover, including disabled lights.
    for (uint j = 0u; gi.lighting.x != 0.0 && j < edits; ++j)
    {
        WorldLightEdit edit = movingEdits[MovingEditIndex(j)];
        if (edit.before.coneShadow.w != 0.0 && edit.before.colorIntensity.w > 0.0)
        {
            bool oldest = true;
            for (uint k = j + 1u; k < edits && oldest; ++k)
            {
                oldest = movingEdits[MovingEditIndex(k)].stamp.yz != edit.stamp.yz;
            }
            if (oldest)
            {
                delta -= MovingLightDiffuse(edit.before, position, geometric, normal);
            }
        }
    }
    return value;
}
vec3 MovingDirect(vec3 position, vec3 geometric, vec3 normal)
{
    vec3 unused;
    return MovingLighting(position, geometric, normal, 0u, unused);
}
#endif
