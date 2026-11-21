#include "gi_denoise_common.glsl"
#ifdef DENOISE_WORLD_SEED
#    include "world_cache.glsl"
#    define DENOISE_SEED_FRAMES 4.0
#endif
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 1, binding = 6) uniform sampler2DArray preblurSignals;
layout(set = 1, binding = 7) uniform sampler2D rawReflection;
layout(set = 1, binding = 8) uniform sampler2D previousSky;
layout(set = 1, binding = 9) uniform sampler2D previousBounce;
layout(set = 1, binding = 10) uniform sampler2D previousSpecular;
layout(set = 1, binding = 11) uniform sampler2D previousFastSky;
layout(set = 1, binding = 12) uniform sampler2D previousFastBounce;
layout(set = 1, binding = 13) uniform sampler2D previousFastSpecular;
layout(set = 1, binding = 14) uniform sampler2D previousLength;
layout(set = 1, binding = 15) uniform sampler2D previousMoments;
layout(set = 1, binding = 16) uniform sampler2D previousReflection;
layout(set = 1, binding = 17) uniform sampler2D previousPosition;
layout(set = 1, binding = 18) uniform sampler2D previousNormal;
layout(set = 1, binding = 19) uniform usampler2D previousReceiver;
layout(set = 1, binding = 32, rgba32f) uniform writeonly image2D nextSky;
layout(set = 1, binding = 33, rgba32f) uniform writeonly image2D nextBounce;
layout(set = 1, binding = 34, rgba16f) uniform writeonly image2D nextSpecular;
layout(set = 1, binding = 35, rgba32f) uniform writeonly image2D nextFastSky;
layout(set = 1, binding = 36, rgba32f) uniform writeonly image2D nextFastBounce;
layout(set = 1, binding = 37, rgba32f) uniform writeonly image2D nextFastSpecular;
layout(set = 1, binding = 38, rgba32f) uniform writeonly image2D nextLength;
layout(set = 1, binding = 39, rgba32f) uniform writeonly image2D nextMoments;
layout(set = 1, binding = 40, rgba32f) uniform writeonly image2D nextReflection;
layout(set = 1, binding = 41, rgba32f) uniform writeonly image2D nextPosition;
layout(set = 1, binding = 42, rg16f) uniform writeonly image2D nextNormal;
layout(set = 1, binding = 43, rg32ui) uniform writeonly uimage2D nextReceiver;

#ifdef DENOISE_VALIDATION
layout(set = 1, binding = 44, rgba32f) uniform writeonly image2DArray temporalValidation;
#endif


#ifdef DENOISE_ANTILAG
layout(set = 1, binding = 45) uniform sampler2D previousResponseAge;
vec3 responseAge = vec3(0);
#endif

GIDenoiseGuide PreviousGuide(ivec2 p)
{
    uvec2 surface = texelFetch(previousReceiver, p, 0).rg;
    return GIDenoiseGuide(texelFetch(previousPosition, p, 0).xyz, DecodeGBufferNormal(texelFetch(previousNormal, p, 0).xy),
                          DecodeGBufferNormal(unpackUnorm2x16(surface.r)), surface.g, texelFetch(previousLength, p, 0).w);
}

float BilinearWeight(ivec2 offset, vec2 fraction)
{
    return (offset.x == 0 ? 1.0 - fraction.x : fraction.x) * (offset.y == 0 ? 1.0 - fraction.y : fraction.y);
}

vec3 PreviousView(vec3 position)
{
    vec3 direction =
        sceneUbo.viewPosition.w < 0.5 ? hybrid.previousViewDirection.xyz : hybrid.previousViewPosition.xyz - position;
    return direction / max(length(direction), 1e-20);
}

#ifdef DENOISE_BICUBIC
bool CubicFootprint(GIDenoiseGuide center,
                    vec2           pixel,
                    bool           reflection,
                    bool           parallax,
                    vec3           virtualPosition,
                    float          distance,
                    vec3           view)
{
    bool  valid = true;
    ivec2 base  = ivec2(floor(pixel));
    for (int y = -1; y <= 2; ++y)
    {
        for (int x = -1; x <= 2; ++x)
        {
            if ((x == -1 || x == 2) && (y == -1 || y == 2))
            {
                continue;
            }
            ivec2 q = base + ivec2(x, y);
            if (!HybridInView(q))
            {
                valid = false;
            }
            else
            {
                GIDenoiseGuide guide = PreviousGuide(q);
                valid                = valid && DenoiseHistoryRejection(center, guide, reflection) == 0.0;
                if (reflection)
                {
                    vec3 oldView       = PreviousView(guide.position);
                    vec4 oldReflection = texelFetch(previousReflection, q, 0);
                    bool lobe          = parallax ? oldReflection.x > 0.0
                                                        && length(guide.position - oldView * oldReflection.x - virtualPosition)
                                                               <= max(0.005, 0.1 * distance)
                                                  : dot(oldView, view) >= mix(0.9995, 0.9, center.roughness * center.roughness);
                    valid              = valid && lobe;
                }
            }
        }
    }
    return valid;
}

vec4 CubicHistory(sampler2D history, vec2 pixel)
{
    ivec2 base = ivec2(floor(pixel));
    vec4  wx = CatmullRomWeights(fract(pixel.x)), wy = CatmullRomWeights(fract(pixel.y));
    vec4  value  = vec4(0);
    float weight = 0.0;
    for (int y = -1; y <= 2; ++y)
    {
        for (int x = -1; x <= 2; ++x)
        {
            if ((x == -1 || x == 2) && (y == -1 || y == 2))
            {
                continue;
            }
            float w  = wx[x + 1] * wy[y + 1];
            value   += texelFetch(history, base + ivec2(x, y), 0) * w;
            weight  += w;
        }
    }
    return max(value / max(weight, 1e-20), vec4(0));
}

#endif

vec4 UpdateFast(vec3 current, vec4 previous, float age)
{
    float luminance = HybridLuminance(current);
    return mix(previous, vec4(current, luminance * luminance), 1.0 / min(max(age, 1.0), 4.0));
}

void main()
{
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    if (!HybridInView(p))
    {
        return;
    }
    GIDenoiseGuide   center     = DenoiseGuide(p);
    bool             receiver   = center.identity != 0u;
    bool             skyEnabled = DENOISE_SKY_ENABLED;
    GIDenoiseSignals current    = DenoisePyramidSignals(preblurSignals, p, 0);
    vec2             reflection = hybrid.reflection.x != 0u ? texelFetch(rawReflection, p, 0).xy : vec2(0);
    float            distance   = reflection.x;
    vec3             view       = DenoiseView(center.position);
    vec4             motion     = HybridMotion(p);
    // The CPU verifies unchanged projection, geometry and surfaces; avoid fractional roundoff motion in that case.
    vec2 surfacePixel = hybrid.resolution.w != 0u ? vec2(p) : vec2(p) + motion.xy * 0.5 * vec2(textureSize(receiverDepth, 0));
    bool historyValid = receiver && hybrid.sampling.w != 0u && hybrid.rejection.z > 0.0 && motion.z > 0.0;
    vec3 ages         = vec3(0);
    vec3 weights      = vec3(0);
    vec4 oldSky = vec4(0), oldBounce = vec4(0), oldSpecular = vec4(0);
    vec4 fastSky = vec4(0), fastBounce = vec4(0), fastSpecular = vec4(0);
    vec4 moments = vec4(0);
    vec2 reasons = vec2(historyValid ? 2.0 : 1.0);
    // A bilinear tap on another surface (identity or plane rejection) marks a disocclusion.
    bool foreign = false;
#ifdef DENOISE_VALIDATION
    bvec3 searched = bvec3(false);
#endif
    // A perspective rotation about a fixed eye has no parallax: the surface and its
    // virtual image project to the same pixel. Do not reject its history on a noisy
    // hit-distance change or a current miss. Geometry changes invalidate CPU history.
    bool changedView = hybrid.previousViewPosition != sceneUbo.viewPosition
                    || (sceneUbo.viewPosition.w < 0.5 && hybrid.previousViewDirection.xyz != sceneUbo.lightInfo.yzw);
    bool parallax    = changedView && center.roughness < 0.4;
    if (parallax && distance <= 0.0 && hybrid.reflection.x != 0u)
    {
        float count = 0.0;
        for (int y = -1; y <= 1; ++y)
        {
            for (int x = -1; x <= 1; ++x)
            {
                ivec2 q = p + ivec2(x, y);
                if (HybridInView(q) && DenoiseSurfaceWeight(center, DenoiseGuide(q)) > 0.5)
                {
                    float sampleDistance = texelFetch(rawReflection, q, 0).x;
                    if (sampleDistance > 0.0)
                    {
                        distance += sampleDistance;
                        count    += 1.0;
                    }
                }
            }
        }
        distance /= max(count, 1.0);
    }
    vec3 virtualPosition  = center.position - view * distance;
    vec4 clip             = hybrid.previousProjectionView * vec4(virtualPosition, 1);
    bool reflectedHistory = historyValid && hybrid.reflection.x != 0u && (!parallax || (distance > 0.0 && clip.w > 0.0));
    vec2 reflectionPixel  = surfacePixel;
    if (parallax && reflectedHistory)
    {
        float divisor   = float(max(hybrid.resolution.z, 1u));
        reflectionPixel = ((clip.xy / clip.w * 0.5 + 0.5) * vec2(hybrid.resolution.xy) - 0.5 - floor(divisor * 0.5)) / divisor;
    }
    if (historyValid)
    {
        ivec2 base     = ivec2(floor(surfacePixel));
        vec2  fraction = fract(surfacePixel);
        for (int y = 0; y < 2; ++y)
        {
            for (int x = 0; x < 2; ++x)
            {
                ivec2 q = base + ivec2(x, y);
                float w = BilinearWeight(ivec2(x, y), fraction);
                if (w > 0.0 && HybridInView(q))
                {
                    GIDenoiseGuide oldGuide  = PreviousGuide(q);
                    float          rejected  = DenoiseHistoryRejection(center, oldGuide, false);
                    reasons.x                = rejected > 0.0 ? rejected : reasons.x;
                    foreign                  = foreign || rejected == 3.0 || rejected == 4.0;
                    float reflectionRejected = DenoiseHistoryRejection(center, oldGuide, true);
                    bool  lobe =
                        dot(PreviousView(oldGuide.position), view) >= mix(0.9995, 0.9, center.roughness * center.roughness);
                    reflectionRejected = !lobe && reflectionRejected == 0.0 ? 8.0 : reflectionRejected;
                    if (reflectedHistory && !parallax)
                    {
                        reasons.y = reflectionRejected > 0.0 ? reflectionRejected : reasons.y;
                    }
                    if (rejected == 0.0)
                    {
                        if (skyEnabled)
                        {
                            oldSky  += texelFetch(previousSky, q, 0) * w;
                            fastSky += texelFetch(previousFastSky, q, 0) * w;
                        }
                        oldBounce  += texelFetch(previousBounce, q, 0) * w;
                        fastBounce += texelFetch(previousFastBounce, q, 0) * w;
                        moments.xy += texelFetch(previousMoments, q, 0).xy * w;
#ifdef DENOISE_ANTILAG
                        responseAge.xy += texelFetch(previousResponseAge, q, 0).xy * w;
#endif
                        ages.x    += texelFetch(previousLength, q, 0).x * w;
                        weights.x += w;
                        if (reflectionRejected == 0.0)
                        {
                            oldSpecular.a += texelFetch(previousSpecular, q, 0).a * w;
                            moments.zw    += texelFetch(previousMoments, q, 0).zw * w;
                            ages.z        += texelFetch(previousLength, q, 0).y * w;
                            weights.z     += w;
                            if (reflectedHistory && !parallax)
                            {
                                oldSpecular.rgb += texelFetch(previousSpecular, q, 0).rgb * w;
                                fastSpecular    += texelFetch(previousFastSpecular, q, 0) * w;
#ifdef DENOISE_ANTILAG
                                responseAge.z += texelFetch(previousResponseAge, q, 0).z * w;
#endif
                                ages.y    += texelFetch(previousReflection, q, 0).w * w;
                                weights.y += w;
                            }
                        }
                    }
                }
            }
        }
    }
    // A reduced-resolution texel holds one point sample of its surface. When the view turns, the previous
    // texels around its reprojected position sampled other points of that surface. On normal-mapped or
    // faceted surfaces every bilinear tap can then fail a normal check although matching history lies one
    // texel further. Without a foreign tap the texel is not disoccluded, so search the 3x3 previous texels
    // with the same checks before resetting. The screen-edge band keeps its reset.
    ivec2 nearest          = ivec2(floor(surfacePixel + 0.5));
    bool  diffuseSearch    = weights.x < 0.01;
    bool  visibilitySearch = weights.z < 0.01;
    bool  reflectionSearch = reflectedHistory && !parallax && weights.y < 0.01;
    if (historyValid && !foreign && HybridInView(nearest) && (diffuseSearch || visibilitySearch || reflectionSearch))
    {
        for (int y = -1; y <= 1; ++y)
        {
            for (int x = -1; x <= 1; ++x)
            {
                ivec2 q = nearest + ivec2(x, y);
                if (HybridInView(q))
                {
                    GIDenoiseGuide oldGuide = PreviousGuide(q);
                    if (DenoiseHistoryRejection(center, oldGuide, false) == 0.0)
                    {
                        if (diffuseSearch)
                        {
                            if (skyEnabled)
                            {
                                oldSky  += texelFetch(previousSky, q, 0);
                                fastSky += texelFetch(previousFastSky, q, 0);
                            }
                            oldBounce  += texelFetch(previousBounce, q, 0);
                            fastBounce += texelFetch(previousFastBounce, q, 0);
                            moments.xy += texelFetch(previousMoments, q, 0).xy;
#ifdef DENOISE_ANTILAG
                            responseAge.xy += texelFetch(previousResponseAge, q, 0).xy;
#endif
                            ages.x    += texelFetch(previousLength, q, 0).x;
                            weights.x += 1.0;
                        }
                        bool lobe =
                            dot(PreviousView(oldGuide.position), view) >= mix(0.9995, 0.9, center.roughness * center.roughness);
                        if (lobe && DenoiseHistoryRejection(center, oldGuide, true) == 0.0)
                        {
                            if (visibilitySearch)
                            {
                                oldSpecular.a += texelFetch(previousSpecular, q, 0).a;
                                moments.zw    += texelFetch(previousMoments, q, 0).zw;
                                ages.z        += texelFetch(previousLength, q, 0).y;
                                weights.z     += 1.0;
                            }
                            if (reflectionSearch)
                            {
                                oldSpecular.rgb += texelFetch(previousSpecular, q, 0).rgb;
                                fastSpecular    += texelFetch(previousFastSpecular, q, 0);
#ifdef DENOISE_ANTILAG
                                responseAge.z += texelFetch(previousResponseAge, q, 0).z;
#endif
                                ages.y    += texelFetch(previousReflection, q, 0).w;
                                weights.y += 1.0;
                            }
                        }
                    }
                }
            }
        }
#ifdef DENOISE_VALIDATION
        searched = bvec3(diffuseSearch && weights.x >= 0.01, reflectionSearch && weights.y >= 0.01,
                         visibilitySearch && weights.z >= 0.01);
#endif
    }
    if (reflectedHistory && parallax)
    {
        ivec2 base     = ivec2(floor(reflectionPixel));
        vec2  fraction = fract(reflectionPixel);
        for (int y = 0; y < 2; ++y)
        {
            for (int x = 0; x < 2; ++x)
            {
                ivec2 q = base + ivec2(x, y);
                float w = BilinearWeight(ivec2(x, y), fraction);
                if (w > 0.0 && HybridInView(q))
                {
                    GIDenoiseGuide oldGuide      = PreviousGuide(q);
                    float          rejected      = DenoiseHistoryRejection(center, oldGuide, true);
                    vec3           oldView       = PreviousView(oldGuide.position);
                    vec4           oldReflection = texelFetch(previousReflection, q, 0);
                    bool           lobe          = oldReflection.x > 0.0
                                                && length(oldGuide.position - oldView * oldReflection.x - virtualPosition)
                                                       <= max(0.005, 0.1 * distance);
                    rejected                     = !lobe && rejected == 0.0 ? 8.0 : rejected;
                    reasons.y                    = rejected > 0.0 ? rejected : reasons.y;
                    if (rejected == 0.0)
                    {
                        oldSpecular.rgb += texelFetch(previousSpecular, q, 0).rgb * w;
                        fastSpecular    += texelFetch(previousFastSpecular, q, 0) * w;
#ifdef DENOISE_ANTILAG
                        responseAge.z += texelFetch(previousResponseAge, q, 0).z * w;
#endif
                        ages.y    += oldReflection.w * w;
                        weights.y += w;
                    }
                }
            }
        }
    }
    // A sliver of bilinear support must not revive an unrelated long history.
    if (weights.x < 0.01)
    {
        oldSky = oldBounce = fastSky = fastBounce = vec4(0);
        moments.xy                                = vec2(0);
        ages.x = weights.x = 0.0;
    }
    if (weights.y < 0.01)
    {
        oldSpecular.rgb = vec3(0);
        fastSpecular    = vec4(0);
        ages.y = weights.y = 0.0;
    }
    if (weights.z < 0.01)
    {
        oldSpecular.a = 0.0;
        moments.zw    = vec2(0);
        ages.z = weights.z = 0.0;
    }
    ages /= max(weights, vec3(1e-20));
#ifdef DENOISE_ANTILAG
    responseAge /= max(vec3(weights.x, weights.x, weights.y), vec3(1e-20));
    responseAge  = mix(vec3(0), responseAge, greaterThanEqual(vec3(weights.x, weights.x, weights.y), vec3(0.01)));
    ages.x       = responseAge.y;
    ages.y       = responseAge.z;
#endif
    oldSky       /= max(weights.x, 1e-20);
    oldBounce    /= max(weights.x, 1e-20);
    fastSky      /= max(weights.x, 1e-20);
    fastBounce   /= max(weights.x, 1e-20);
    oldSpecular  /= max(vec4(vec3(weights.y), weights.z), vec4(1e-20));
    fastSpecular /= max(weights.y, 1e-20);
    moments      /= max(vec4(vec2(weights.x), vec2(weights.z)), vec4(1e-20));
#if defined(DENOISE_VALIDATION) || defined(DENOISE_BICUBIC)
    bvec3 cubic = bvec3(false);
#endif
#ifdef DENOISE_BICUBIC
    if (historyValid)
    {
        cubic.x = weights.x >= 0.01 && CubicFootprint(center, surfacePixel, false, false, virtualPosition, distance, view);
        cubic.z = weights.z >= 0.01 && CubicFootprint(center, surfacePixel, true, false, virtualPosition, distance, view);
        cubic.y = reflectedHistory && weights.y >= 0.01
               && CubicFootprint(center, reflectionPixel, true, parallax, virtualPosition, distance, view);
        if (cubic.x)
        {
            if (skyEnabled)
            {
                oldSky.rgb = CubicHistory(previousSky, surfacePixel).rgb;
                fastSky    = CubicHistory(previousFastSky, surfacePixel);
            }
            oldBounce.rgb = CubicHistory(previousBounce, surfacePixel).rgb;
            fastBounce    = CubicHistory(previousFastBounce, surfacePixel);
        }
        if (cubic.y)
        {
            oldSpecular.rgb = CubicHistory(previousSpecular, reflectionPixel).rgb;
            fastSpecular    = CubicHistory(previousFastSpecular, reflectionPixel);
        }
        if (cubic.z)
        {
            oldSpecular.a = clamp(CubicHistory(previousSpecular, surfacePixel).a, 0.0, 1.0);
        }
    }
#endif
#ifdef DENOISE_WORLD_SEED
    // A disoccluded receiver starts from its world-cache cell's indirect light, weighted as
    // DENOISE_SEED_FRAMES frames, instead of from its first noisy samples. Its own samples replace
    // the seed as they accumulate. Sky is not seeded.
    vec3 seed;
    if (receiver && weights.x < 0.01 && WorldIndirect(center.position, center.geometric, center.normal, 32u, seed))
    {
        seed            *= uintBitsToFloat(worldCache.lightCount.z);
        float luminance  = HybridLuminance(seed);
        oldBounce        = vec4(seed, 0);
        fastBounce       = vec4(seed, luminance * luminance);
        moments.xy       = vec2(luminance, luminance * luminance);
        ages.x           = DENOISE_SEED_FRAMES;
        reasons.x        = 8.0;
    }
#endif
    // No motion-dependent caps: accepted taps keep slow history and fast history limits lighting lag.
    float limit    = float(hybrid.sampling.z * max(hybrid.resolution.z * hybrid.resolution.z, 1u));
    vec3  observed = vec3(current.sky.a >= 0.0 && current.bounce.a >= 0.0,
                          hybrid.reflection.x != 0u && current.specular.r >= 0.0, current.specular.a >= 0.0);
    ages           = min(ages + observed, vec3(limit));
    vec3 alpha     = observed / max(ages, vec3(1));
#ifdef DENOISE_ANTILAG
    float skyAge   = min(responseAge.x + observed.x, limit);
    float skyAlpha = observed.x / max(skyAge, 1.0);
#    define DENOISE_SKY_AGE   skyAge
#    define DENOISE_SKY_ALPHA skyAlpha
#else
#    define DENOISE_SKY_AGE   ages.x
#    define DENOISE_SKY_ALPHA alpha.x
#endif
    fastSky                = skyEnabled && observed.x > 0.0 ? UpdateFast(current.sky.rgb, fastSky, DENOISE_SKY_AGE) : fastSky;
    fastBounce             = observed.x > 0.0 ? UpdateFast(current.bounce.rgb, fastBounce, ages.x) : fastBounce;
    fastSpecular           = observed.y > 0.0 ? UpdateFast(current.specular.rgb, fastSpecular, ages.y) : fastSpecular;
    float diffuseLuminance = HybridLuminance(current.sky.rgb + current.bounce.rgb);
    moments.xy =
        mix(moments.xy, vec2(diffuseLuminance, diffuseLuminance * diffuseLuminance), observed.x / min(max(ages.x, 1.0), 4.0));
    moments.zw    = mix(moments.zw, vec2(current.specular.a, current.specular.a * current.specular.a),
                        observed.z / min(max(ages.z, 1.0), 4.0));
    vec3 sky      = mix(oldSky.rgb, current.sky.rgb, DENOISE_SKY_ALPHA);
    vec3 bounce   = mix(oldBounce.rgb, current.bounce.rgb, alpha.x);
    vec3 specular = mix(oldSpecular.rgb, current.specular.rgb, alpha.y);
    // History fix clamps these slow signals against a spatial neighbourhood of the updated fast histories.
    float visibility      = mix(oldSpecular.a, current.specular.a, alpha.z);
    float visibilitySigma = sqrt(max(moments.w - moments.z * moments.z, 0.0));
    visibility = clamp(visibility, max(0.0, moments.z - 2.0 * visibilitySigma), min(1.0, moments.z + 2.0 * visibilitySigma));
    reasons.x  = weights.x >= 0.01 ? 0.0 : reasons.x;
    reasons.y  = weights.y >= 0.01 ? 0.0 : reasons.y;
    if (all(lessThan(p, imageSize(nextSky))))
    {
        imageStore(nextSky, p,
                   receiver ? vec4(sky,
#ifdef DENOISE_ANTILAG
                                   skyAge
#else
                                   1.0
#endif
                                   )
                            : vec4(0));
    }
    imageStore(nextBounce, p, receiver ? vec4(bounce, ages.x) : vec4(0));
    imageStore(nextSpecular, p, receiver ? vec4(specular, visibility) : vec4(0));
    if (all(lessThan(p, imageSize(nextFastSky))))
    {
        imageStore(nextFastSky, p, receiver ? fastSky : vec4(0));
    }
    imageStore(nextFastBounce, p, receiver ? fastBounce : vec4(0));
    imageStore(nextFastSpecular, p, receiver ? fastSpecular : vec4(0));
    imageStore(nextLength, p, receiver ? vec4(ages.x, ages.z, reasons.x + 16.0 * reasons.y, center.roughness) : vec4(0));
    imageStore(nextMoments, p, receiver ? moments : vec4(0));
    imageStore(nextReflection, p,
               receiver ? vec4(distance, HybridLuminance(fastSpecular.rgb), fastSpecular.a, ages.y) : vec4(0));
#ifdef DENOISE_VALIDATION
    // Signals: sky, bounce, reflection, specular visibility. Fast ages are the
    // effective accumulation ages (the fast filters share each signal's validity).
    vec4 slowAge = receiver ? vec4(skyEnabled ? DENOISE_SKY_AGE : 0.0, ages.x, ages.y, ages.z) : vec4(0);
    imageStore(temporalValidation, ivec3(p, 0), slowAge);
    imageStore(temporalValidation, ivec3(p, 1), min(slowAge, vec4(4)));
    // Low nibble: rejection; next nibble: footprint (0 none, 1 bilinear, 2 bicubic, 3 3x3 search).
    vec4 reprojection  = vec4(reasons.x, reasons.x, reasons.y, weights.z >= 0.01 ? 0.0 : reasons.y)
                       + 16.0 * vec4(weights.x >= 0.01, weights.x >= 0.01, weights.y >= 0.01, weights.z >= 0.01);
    reprojection      += 16.0 * vec4(cubic.x, cubic.x, cubic.y, cubic.z);
    reprojection      += 32.0 * vec4(searched.x, searched.x, searched.y, searched.z);
    imageStore(temporalValidation, ivec3(p, 2), receiver ? reprojection : vec4(0));
#endif
    imageStore(nextPosition, p, vec4(center.position, center.roughness));
    imageStore(nextNormal, p, vec4(EncodeGBufferNormal(center.normal), 0, 0));
    imageStore(nextReceiver, p, texelFetch(receiverSurface, p, 0));
}
