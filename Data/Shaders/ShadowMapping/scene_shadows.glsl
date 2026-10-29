#ifndef ZEN_SCENE_SHADOWS
#define ZEN_SCENE_SHADOWS
#include "../Common/scene_lighting.glsl"
#if defined(HARDWARE_SHADOWS)
#    include "../VoxelGI/hybrid_provider.glsl"
#    include "../VoxelGI/light_fixture_mask.glsl"
// Per local light: how far every sampled ray of a direction travels before meeting geometry
// (LightFixtureMasks; light_fixture_mask_build.comp).
layout(set = 4, binding = 29) uniform sampler2DArray lightFixtureMask;

float SceneLightVisibility(int lightIndex, vec3 position, vec3 normal)
{
    SceneLight light      = lightIndex == MAX_SCENE_LIGHTS ? sceneUbo.cameraLight : sceneUbo.lights[lightIndex];
    float      visibility = 1.0;
    if (gi.limits.z > 0.5 && (lightIndex == MAX_SCENE_LIGHTS || light.coneShadow.z > 0.0))
    {
        vec3  direction;
        float distanceToLight;
        vec3  incoming = EvaluateLight(light, position, direction, distanceToLight);
        if (any(greaterThan(incoming, vec3(0))))
        {
            // Offset toward the ray's hemisphere, also for diffuse-transmission receivers.
            vec3 origin = HybridTraceOrigin(position, dot(normal, direction) >= 0.0 ? normal : -normal, 0.0);
            if (int(light.directionType.w) != 0)
            {
                // Segment visibility is symmetric, so trace from the light. A fixture around the
                // light (bulb, lantern glass) then ends the any-hit traversal at once instead of
                // after crossing the scene from the receiver (New Sponza: 98% of lamp rays).
                // The offset origin keeps the receiver itself beyond the segment's end; exclude
                // that endpoint, without dropping a finite interval before it.
                vec3 delta      = origin - light.positionRange.xyz;
                distanceToLight = length(delta);
                direction       = delta / max(distanceToLight, 1e-20);
                distanceToLight = uintBitsToFloat(floatBitsToUint(distanceToLight) - uint(distanceToLight > 0.0));
                if (distanceToLight > 0.0)
                {
                    // Beyond the fixture mask's distance, every sampled ray of this direction already
                    // met geometry: the segment is occluded without a ray. The camera light has no mask.
                    bool  enclosed = false;
                    int   face;
                    ivec2 texel = LightFixtureMaskTexel(delta, face);
                    if (lightIndex != MAX_SCENE_LIGHTS)
                    {
                        float shell = texelFetch(lightFixtureMask, ivec3(texel, lightIndex * 6 + face), 0).r;
                        enclosed    = distanceToLight > shell * 1.01 + 1e-4;
                    }
                    visibility =
                        enclosed || HybridTraceRay(0, light.positionRange.xyz, direction, distanceToLight).hit ? 0.0 : 1.0;
                }
            }
            else if (distanceToLight > 0.0)
            {
                visibility = HybridTraceRay(0, origin, direction, distanceToLight).hit ? 0.0 : 1.0;
            }
        }
    }
    return visibility;
}
#else
layout(std140, set = 4, binding = 0) uniform uSceneShadows
{
    mat4 viewProjection[(MAX_SCENE_LIGHTS + 1) * 6];
    vec4 lights[MAX_SCENE_LIGHTS + 1];
    vec4 settings;
}
shadows;
layout(set = 4, binding = 1) uniform sampler2DArray sceneShadowMaps;
// Deepest depth of each 4x4 block of sceneShadowMaps (scene_shadow_max_depth.comp).
layout(set = 4, binding = 29) uniform sampler2DArray sceneShadowMaxDepth;

int PointShadowFace(vec3 direction)
{
    vec3 a    = abs(direction);
    int  axis = a.x >= a.y && a.x >= a.z ? 0 : (a.y >= a.z ? 1 : 2);
    return axis * 2 + (direction[axis] < 0 ? 1 : 0);
}

float SceneLightVisibility(int lightIndex, vec3 position, vec3 normal)
{
    vec4  info       = shadows.lights[lightIndex];
    float visibility = 1.0;
    if (info.y > 0)
    {
        SceneLight light       = lightIndex == MAX_SCENE_LIGHTS ? sceneUbo.cameraLight : sceneUbo.lights[lightIndex];
        bool       directional = int(light.directionType.w) == 0;
        vec3       receiver    = position + normal * shadows.settings.x;
        int        layer       = int(info.x);
        vec3       forward     = light.directionType.xyz;
        if (info.y == 6)
        {
            int face           = PointShadowFace(receiver - light.positionRange.xyz);
            layer             += face;
            forward            = vec3(0);
            forward[face / 2]  = (face % 2) == 0 ? 1.0 : -1.0;
        }
        vec3 helper = abs(forward.y) < 0.99 ? vec3(0, 1, 0) : vec3(0, 0, 1);
        vec3 right  = normalize(cross(forward, helper));
        vec3 up     = cross(right, forward);
        vec4 clip   = shadows.viewProjection[layer] * vec4(receiver, 1);
        vec3 ndc    = clip.xyz / max(clip.w, 1e-8);
        // Normal bias can move a lit receiver outside the spotlight's finite
        // projection. Retry at the surface instead of treating that edge as lit;
        // the per-tap depth bias below still prevents self-shadowing.
        if (!directional && info.y == 1 && (clip.w <= 0 || any(greaterThan(abs(ndc.xy), vec2(1)))))
        {
            receiver = position;
            clip     = shadows.viewProjection[layer] * vec4(receiver, 1);
            ndc      = clip.xyz / max(clip.w, 1e-8);
        }
        if (clip.w > 0 && all(lessThanEqual(abs(ndc.xy), vec2(1))) && ndc.z >= 0 && ndc.z <= 1)
        {
            float receiverDepth = directional ? ndc.z : length(receiver - light.positionRange.xyz) * info.z;
            vec2  pixel         = (ndc.xy * 0.5 + 0.5) * shadows.settings.y - 0.5;
            ivec2 base          = ivec2(floor(pixel));
            visibility          = 0;
            bool shadowed       = false;
            if (!directional)
            {
                // Every tap's plane-corrected reference below is a distance to a point of the receiver
                // plane, so none is nearer than the plane itself. Away from grazing incidence every tap
                // ray meets the plane ahead of the light; if even the plane lies beyond the deepest
                // stored depth under the taps, each tap is shadowed and the filter is exactly 0. A
                // lamp's own fixture stores depths far nearer than its receivers.
                vec3  toReceiver = receiver - light.positionRange.xyz;
                vec3  unitNormal = normalize(normal);
                float facing     = abs(dot(normalize(toReceiver), unitNormal));
                ivec2 last       = ivec2(int(shadows.settings.y) - 1);
                ivec2 low        = clamp(base - 1, ivec2(0), last) >> 2;
                ivec2 high       = clamp(base + 2, ivec2(0), last) >> 2;
                float deepest    = max(max(texelFetch(sceneShadowMaxDepth, ivec3(low, layer), 0).r,
                                           texelFetch(sceneShadowMaxDepth, ivec3(high.x, low.y, layer), 0).r),
                                       max(texelFetch(sceneShadowMaxDepth, ivec3(low.x, high.y, layer), 0).r,
                                           texelFetch(sceneShadowMaxDepth, ivec3(high, layer), 0).r));
                float nearest    = (abs(dot(toReceiver, unitNormal)) - shadows.settings.x) * info.z;
                shadowed         = facing > 8.0 * info.w && nearest * (1.0 - 1e-4) > deepest;
            }
            // Separable tent PCF: smooth subtexel transitions, without filtering depth itself.
            for (int y = -1; y <= 2 && !shadowed; ++y)
            {
                for (int x = -1; x <= 2; ++x)
                {
                    ivec2 samplePixel = base + ivec2(x, y);
                    vec2  weight      = max(vec2(2) - abs(vec2(samplePixel) - pixel), vec2(0));
                    samplePixel       = clamp(samplePixel, ivec2(0), ivec2(int(shadows.settings.y) - 1));
                    // Compare against the receiver plane at this tap, not the center depth.
                    // This prevents PCF from shadowing a sloped surface with its own neighbors.
                    float reference = receiverDepth;
                    if (directional)
                    {
                        vec2  offset      = (vec2(samplePixel) - pixel) * info.w;
                        float denominator = dot(forward, normal);
                        if (abs(denominator) > 1e-4)
                        {
                            reference -= dot(right * offset.x - up * offset.y, normal) / denominator * info.z;
                        }
                    }
                    else
                    {
                        vec2  sampleNDC   = (vec2(samplePixel) + 0.5) / shadows.settings.y * 2.0 - 1.0;
                        float tangent     = info.w * shadows.settings.y * 0.5;
                        vec3  ray         = forward + (right * sampleNDC.x - up * sampleNDC.y) * tangent;
                        float denominator = dot(ray, normal);
                        if (abs(denominator) > 1e-4)
                        {
                            reference = dot(receiver - light.positionRange.xyz, normal) / denominator * length(ray) * info.z;
                        }
                    }
                    reference   -= shadows.settings.x * info.z;
                    float depth  = texelFetch(sceneShadowMaps, ivec3(samplePixel, layer), 0).r;
                    visibility  += weight.x * weight.y * float(reference <= depth) / 16.0;
                }
            }
        }
    }
    return visibility;
}
#endif
#endif
