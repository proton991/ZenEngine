#include "gi_denoise_common.glsl"
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 1, binding = 6) uniform sampler2DArray preblurSignals;
layout(set = 1, binding = 7) uniform usampler2DArray preblurGuides;
layout(set = 1, binding = 8) uniform sampler2D inputSky;
layout(set = 1, binding = 9) uniform sampler2D inputBounce;
layout(set = 1, binding = 10) uniform sampler2D inputSpecular;
layout(set = 1, binding = 11) uniform sampler2D historyLength;
layout(set = 1, binding = 12) uniform sampler2D reflectionHistory;
layout(set = 1, binding = 13, rgba32f) uniform writeonly image2D outputSky;
layout(set = 1, binding = 14, rgba32f) uniform writeonly image2D outputBounce;
layout(set = 1, binding = 15, rgba16f) uniform writeonly image2D outputSpecular;
layout(set = 1, binding = 16) uniform sampler2D inputFastSky;
layout(set = 1, binding = 17) uniform sampler2D inputFastBounce;
layout(set = 1, binding = 18) uniform sampler2D inputFastSpecular;

#ifdef DENOISE_VALIDATION
layout(set = 1, binding = 19, rgba32f) uniform writeonly image2DArray repairValidation;
vec4 clampMovement = vec4(0);
vec4 repairSource  = vec4(0);
#endif

#ifdef DENOISE_ANTILAG
layout(set = 1, binding = 20, rgba32f) uniform writeonly image2D outputResponseAge;
vec3 responseAge = vec3(0);
#endif

// An 8x8 group plus a two-texel border. Shared storage remains below the 16 KiB RHI minimum.
shared vec4  fastPosition[144];
shared uvec4 fastNormal[144];
shared vec3  fastGeometric[144];
shared vec3  fastSky[DENOISE_SKY_ENABLED ? 144 : 1];
shared float fastAge[144];
shared vec3  fastBounce[144];
shared vec4  fastSpecular[144];

vec3 ToYCoCg(vec3 color)
{
    return vec3(dot(color, vec3(.25, .5, .25)), .5 * (color.r - color.b), dot(color, vec3(-.25, .5, -.25)));
}

vec3 FromYCoCg(vec3 color)
{
    return max(vec3(color.x + color.y - color.z, color.x + color.z, color.x - color.y - color.z), vec3(0));
}

void LoadFastTile()
{
    for (uint lane = gl_LocalInvocationIndex; lane < 144u; lane += 64u)
    {
        ivec2 q             = ivec2(gl_WorkGroupID.xy) * 8 + ivec2(lane % 12u, lane / 12u) - 2;
        fastPosition[lane]  = vec4(0);
        fastGeometric[lane] = vec3(0);
        fastNormal[lane]    = uvec4(0);
        fastSpecular[lane]  = vec4(0);
        fastAge[lane]       = 0.0;
        if (DENOISE_SKY_ENABLED)
        {
            fastSky[lane] = vec3(0);
        }
        fastBounce[lane] = vec3(0);
        if (HybridInView(q))
        {
            GIDenoiseGuide guide = DenoiseGuide(q);
            fastPosition[lane]   = vec4(guide.position, guide.roughness);
            fastNormal[lane]     = uvec4(floatBitsToUint(guide.normal), guide.identity);
            fastGeometric[lane]  = guide.geometric;
            fastAge[lane]        = texelFetch(historyLength, q, 0).x;
            if (DENOISE_SKY_ENABLED)
            {
                fastSky[lane] = ToYCoCg(texelFetch(inputFastSky, q, 0).rgb);
            }
            fastBounce[lane] = ToYCoCg(texelFetch(inputFastBounce, q, 0).rgb);
            if (hybrid.reflection.x != 0u)
            {
                fastSpecular[lane] =
                    vec4(ToYCoCg(texelFetch(inputFastSpecular, q, 0).rgb), texelFetch(reflectionHistory, q, 0).w);
            }
        }
    }
    barrier();
}

#if defined(DENOISE_VALIDATION) || defined(DENOISE_ANTILAG)
#    define DENOISE_SIGNAL(x) , x
#else
#    define DENOISE_SIGNAL(x)
#endif

vec3 ClampSlowColor(vec3 slow, vec3 center, vec3 meanDelta, vec3 secondMoment, float sigmaScale DENOISE_SIGNAL(uint signal))
{
    vec3 mean  = center + meanDelta;
    vec3 sigma = sqrt(max(secondMoment - meanDelta * meanDelta, vec3(0)));
#ifdef DENOISE_VALIDATION
    vec3 moved =
        ToYCoCg(slow) - clamp(ToYCoCg(slow), min(mean - sigmaScale * sigma, center), max(mean + sigmaScale * sigma, center));
    // Absolute floor keeps a zero-variance clamp finite; it is diagnostic only.
    clampMovement[signal] =
        max(max(abs(moved.x) / max(sigma.x, 1e-6), abs(moved.y) / max(sigma.y, 1e-6)), abs(moved.z) / max(sigma.z, 1e-6));
#endif
    // Keep the center fast estimate inside the box to avoid shifting thin features toward their neighbours.
#ifdef DENOISE_ANTILAG
    vec3  clamped       = clamp(ToYCoCg(slow), min(mean - sigmaScale * sigma, center), max(mean + sigmaScale * sigma, center));
    ivec2 p             = ivec2(gl_GlobalInvocationID.xy);
    vec4  fast          = signal == 0u ? texelFetch(inputFastSky, p, 0)
                        : signal == 1u ? texelFetch(inputFastBounce, p, 0)
                                       : texelFetch(inputFastSpecular, p, 0);
    float luminance     = HybridLuminance(fast.rgb);
    float temporalSigma = sqrt(max(fast.a - luminance * luminance, 0.0));
    // Convert the YCoCg deviation to the same luminance units as fast.a. The sum
    // of component sigmas bounds luminance sigma without storing covariance.
    float spatialSigma = dot(sigma, vec3(1.0, 0.1404, 0.4304));
    float displacement = abs(HybridLuminance(slow) - HybridLuminance(FromYCoCg(clamped)));
    if (displacement > 2.0 * max(spatialSigma + temporalSigma, 1e-6))
    {
        responseAge[signal] = max(1.0, responseAge[signal] * 0.25);
        clamped             = mix(clamped, center, 0.25);
    }
    return FromYCoCg(clamped);
#else
    return FromYCoCg(clamp(ToYCoCg(slow), min(mean - sigmaScale * sigma, center), max(mean + sigmaScale * sigma, center)));
#endif
}

#ifdef DENOISE_ANTI_FIREFLY
// Scales a colour down so its YCoCg luma does not exceed limit; hue is kept.
vec3 LimitLuma(vec3 color, float limit)
{
    float luma = ToYCoCg(color).x;
    return luma > limit ? color * (limit / luma) : color;
}
#endif

void ClampSlowHistory(inout GIDenoiseSignals signals, GIDenoiseGuide center, vec3 age)
{
    mat3 first = mat3(0), second = mat3(0);
    vec3 weightSum = vec3(0);
#ifdef DENOISE_ANTI_FIREFLY
    // Brightest fast luma of matching neighbours per signal (sky, bounce, specular); -1 when none match.
    vec3 brightest = vec3(-1.0);
#endif
    ivec2 local      = ivec2(gl_LocalInvocationID.xy) + 2;
    uint  centerLane = uint(local.y * 12 + local.x);
    mat3  centerValues =
        mat3(DENOISE_SKY_ENABLED ? fastSky[centerLane] : vec3(0), fastBounce[centerLane], fastSpecular[centerLane].rgb);
    for (int y = -2; y <= 2; ++y)
    {
        for (int x = -2; x <= 2; ++x)
        {
            uint           lane           = uint((local.y + y) * 12 + local.x + x);
            GIDenoiseGuide guide          = GIDenoiseGuide(fastPosition[lane].xyz, uintBitsToFloat(fastNormal[lane].xyz),
                                                           fastGeometric[lane], fastNormal[lane].w, fastPosition[lane].w);
            vec2           spatialWeights = DenoiseSpatialWeights(center, guide, vec2(0));
            float          diffuseWeight  = fastAge[lane] > 0.0 ? spatialWeights.x : 0.0;
            float          specularWeight = fastSpecular[lane].w > 0.0 ? spatialWeights.y : 0.0;
            vec3           weight         = vec3(diffuseWeight, diffuseWeight, specularWeight);
            mat3 values = mat3(DENOISE_SKY_ENABLED ? fastSky[lane] : vec3(0), fastBounce[lane], fastSpecular[lane].rgb);
            for (int signal = DENOISE_SKY_ENABLED ? 0 : 1; signal < 3; ++signal)
            {
                vec3 delta      = values[signal] - centerValues[signal];
                first[signal]  += delta * weight[signal];
                second[signal] += delta * delta * weight[signal];
            }
            weightSum += weight;
#ifdef DENOISE_ANTI_FIREFLY
            if (x != 0 || y != 0)
            {
                vec3 luma = vec3(values[0].x, values[1].x, values[2].x);
                brightest = max(brightest, mix(vec3(-1.0), luma, greaterThan(weight, vec3(0.0))));
            }
#endif
        }
    }
    for (int signal = DENOISE_SKY_ENABLED ? 0 : 1; signal < 3; ++signal)
    {
        first[signal]  /= max(weightSum[signal], 1e-20);
        second[signal] /= max(weightSum[signal], 1e-20);
    }
#ifdef DENOISE_ANTILAG
    if (DENOISE_SKY_ENABLED && responseAge.x >= 4.0 && weightSum.x > 0.0)
    {
        signals.sky.rgb = ClampSlowColor(signals.sky.rgb, fastSky[centerLane], first[0], second[0], 4.0 DENOISE_SIGNAL(0u));
    }
#endif
    if (age.x >= 4.0 && weightSum.x > 0.0)
    {
#ifndef DENOISE_ANTILAG
        if (DENOISE_SKY_ENABLED)
        {
            signals.sky.rgb = ClampSlowColor(signals.sky.rgb, fastSky[centerLane], first[0], second[0], 4.0 DENOISE_SIGNAL(0u));
        }
#endif
        // Lighting changes tighten the fast-history bounds without discarding mature
        // estimates that still fit them. Sky lighting has its own invalidation path.
        signals.bounce.rgb = ClampSlowColor(signals.bounce.rgb, fastBounce[centerLane], first[1], second[1],
                                            hybrid.bounce.z != 0u ? 1.0 : 4.0 DENOISE_SIGNAL(1u));
    }
    if (age.z >= 4.0 && weightSum.z > 0.0)
    {
        signals.specular.rgb = ClampSlowColor(signals.specular.rgb, fastSpecular[centerLane].rgb, first[2], second[2],
                                              hybrid.reflection.z != 0u ? 1.0 : 4.0 DENOISE_SIGNAL(2u));
    }
#ifdef DENOISE_ANTI_FIREFLY
    // Younger texels have no slow history to clamp, so one bright trace or ReSTIR sample would reach
    // the screen and the next frame's history. Limit it to the brightest matching neighbour. The texel
    // itself is excluded, so a lone outlier cannot widen its own bound.
#    ifdef DENOISE_ANTILAG
    float skyAge = responseAge.x;
#    else
    float skyAge = age.x;
#    endif
    if (DENOISE_SKY_ENABLED && skyAge > 0.0 && skyAge < 4.0 && brightest.x >= 0.0)
    {
        signals.sky.rgb = LimitLuma(signals.sky.rgb, brightest.x);
    }
    if (age.x > 0.0 && age.x < 4.0 && brightest.y >= 0.0)
    {
        signals.bounce.rgb = LimitLuma(signals.bounce.rgb, brightest.y);
    }
    if (age.z > 0.0 && age.z < 4.0 && brightest.z >= 0.0)
    {
        signals.specular.rgb = LimitLuma(signals.specular.rgb, brightest.z);
    }
#endif
}

void main()
{
    LoadFastTile();
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    if (!HybridInView(p))
    {
        return;
    }
    GIDenoiseGuide   center = DenoiseGuide(p);
    GIDenoiseSignals result =
        GIDenoiseSignals(DenoiseSkySignal(inputSky, p), texelFetch(inputBounce, p, 0), texelFetch(inputSpecular, p, 0));
    vec3 age =
        vec3(texelFetch(historyLength, p, 0).xy, hybrid.reflection.x != 0u ? texelFetch(reflectionHistory, p, 0).w : 0.0);
#ifdef DENOISE_ANTILAG
    responseAge = vec3(DENOISE_SKY_ENABLED ? result.sky.a : 0.0, age.x, age.z);
#endif
    if (center.identity != 0u)
    {
        ClampSlowHistory(result, center, age);
    }
    // Sky rays are not reservoir-reused. Preblur and the output spatial filter provide
    // their local support; broad pyramid repair would mix lit and shadowed parts of
    // the same surface into valid dark estimates, then feed that light back as history.
    // Only a missing sky estimate needs this fallback. Anti-lag tracks its own age.
#ifdef DENOISE_ANTILAG
    float skyAge = responseAge.x;
#else
    float skyAge = age.x;
#endif
    bool missingSky = DENOISE_SKY_ENABLED && skyAge == 0.0;
    if (center.identity != 0u && (missingSky || any(lessThan(age, vec3(8)))))
    {
        vec4 weights     = vec4(skyAge > 0.0, age.x > 0.0, age.z > 0.0, age.y > 0.0);
        result.sky      *= weights.x;
        result.bounce   *= weights.y;
        result.specular *= vec4(vec3(weights.z), weights.w);
        // Reused bounce/reflection samples remain correlated over tens of GI texels. Spread the mip
        // taps over that footprint while the first eight frames accumulate.
        // Each signal selects its own mip and retains geometric/lobe rejection.
        for (int level = 1; level < 4; ++level)
        {
            bvec3 repair    = bvec3(age.x < 8.0 && level == clamp(4 - int(age.x / 2.0), 1, 3),
                                    age.y < 8.0 && level == clamp(4 - int(age.y / 2.0), 1, 3),
                                    age.z < 8.0 && level == clamp(4 - int(age.z / 2.0), 1, 3));
            bool  repairSky = missingSky && level == 3;
            if (repairSky || any(repair))
            {
                vec2  source = (vec2(p) + 0.5) / float(1 << level) - 0.5;
                ivec2 base   = ivec2(floor(source));
                ivec2 extent = textureSize(preblurSignals, level).xy;
                for (int y = -1; y <= 2; ++y)
                {
                    for (int x = -1; x <= 2; ++x)
                    {
                        // Clamping keeps support at viewport edges and in small mips.
                        ivec2            q              = clamp(base + ivec2(x, y) * 8, ivec2(0), extent - 1);
                        GIDenoiseGuide   guide          = DenoisePyramidGuide(preblurGuides, q, level);
                        GIDenoiseSignals sampleSignals  = DenoisePyramidSignals(preblurSignals, q, level);
                        vec2             offset         = (vec2(q) - source) / 8.0;
                        vec2             spatialWeights = DenoiseSpatialWeights(center, guide, vec2(0.5 * dot(offset, offset)));
                        float            weight         = spatialWeights.x;
                        float            specularWeight = spatialWeights.y;
                        vec4             selected  = vec4(repairSky && sampleSignals.sky.a >= 0.0 ? weight : 0.0,
                                                          repair.x && sampleSignals.bounce.a >= 0.0 ? weight : 0.0,
                                                          repair.z && sampleSignals.specular.r >= 0.0 ? specularWeight : 0.0,
                                                          repair.y && sampleSignals.specular.a >= 0.0 ? specularWeight : 0.0);
                        result.sky                += sampleSignals.sky * selected.x;
                        result.bounce             += sampleSignals.bounce * selected.y;
                        result.specular           += sampleSignals.specular * vec4(vec3(selected.z), selected.w);
                        weights                   += selected;
#ifdef DENOISE_VALIDATION
                        repairSource = mix(repairSource, vec4(level), greaterThan(selected, vec4(0)));
#endif
                    }
                }
            }
        }
        result.sky      /= max(weights.x, 1e-20);
        result.bounce   /= max(weights.y, 1e-20);
        result.specular /= max(vec4(vec3(weights.z), weights.w), vec4(1e-20));
    }
#ifdef DENOISE_ANTILAG
    imageStore(outputResponseAge, p, center.identity != 0u ? vec4(responseAge, 0) : vec4(0));
#endif
#ifdef DENOISE_VALIDATION
    imageStore(repairValidation, ivec3(p, 0), center.identity != 0u ? clampMovement : vec4(0));
    // 0 own history, 1..3 pyramid mip, 4 reserved for cache seed.
    imageStore(repairValidation, ivec3(p, 1), center.identity != 0u ? repairSource : vec4(0));
#endif
    if (all(lessThan(p, imageSize(outputSky))))
    {
        imageStore(outputSky, p, center.identity != 0u ? result.sky : vec4(0));
    }
    imageStore(outputBounce, p, center.identity != 0u ? result.bounce : vec4(0));
    imageStore(outputSpecular, p, center.identity != 0u ? result.specular : vec4(0));
}
