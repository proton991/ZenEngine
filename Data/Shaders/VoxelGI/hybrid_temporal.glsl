#include "hybrid_guides.glsl"
#include "../Common/scene_lighting.glsl"
#ifdef HYBRID_CACHE_HISTORY_GUIDES
layout(local_size_x = 16, local_size_y = 16) in;
#else
layout(local_size_x = 8, local_size_y = 8) in;
#endif
layout(set = 1, binding = 6) uniform sampler2D rawSky;
layout(set = 1, binding = 7) uniform sampler2D rawSpecular;
layout(set = 1, binding = 8) uniform sampler2D previousSky;
layout(set = 1, binding = 9) uniform sampler2D previousSpecular;
layout(set = 1, binding = 10) uniform sampler2D previousMoments;
layout(set = 1, binding = 11) uniform sampler2D previousLength;
layout(set = 1, binding = 12) uniform sampler2D previousPosition;
layout(set = 1, binding = 13) uniform sampler2D previousNormal;
layout(set = 1, binding = 14) uniform usampler2D previousReceiver;
layout(set = 1, binding = 15, rgba32f) uniform writeonly image2D nextSky;
#ifdef HYBRID_REFERENCE_HISTORY
layout(set = 1, binding = 16, rgba32f) uniform writeonly image2D nextSpecular;
#else
layout(set = 1, binding = 16, rgba16f) uniform writeonly image2D nextSpecular;
#endif
layout(set = 1, binding = 17, rgba32f) uniform writeonly image2D nextMoments;
layout(set = 1, binding = 18, rgba16f) uniform writeonly image2D nextLength;
layout(set = 1, binding = 19, rgba32f) uniform writeonly image2D nextPosition;
layout(set = 1, binding = 20, rg16f) uniform writeonly image2D nextNormal;
layout(set = 1, binding = 21, rg32ui) uniform writeonly uimage2D nextReceiver;
layout(set = 1, binding = 25) uniform sampler2D rawBounce;
layout(set = 1, binding = 26) uniform sampler2D previousBounce;
layout(set = 1, binding = 27) uniform sampler2D previousBounceMoments;
layout(set = 1, binding = 28, rgba32f) uniform writeonly image2D nextBounce;
layout(set = 1, binding = 29, rg32f) uniform writeonly image2D nextBounceMoments;
layout(set = 1, binding = 30) uniform sampler2D rawBouncePrevious;
layout(set = 1, binding = 31) uniform sampler2D rawReflection;
layout(set = 1, binding = 32) uniform sampler2D rawReflectionPrevious;
layout(set = 1, binding = 33) uniform sampler2D previousReflection;
layout(set = 1, binding = 34, rgba32f) uniform writeonly image2D nextReflection;
float HybridHistoryLimit(float frames)
{
    // Fewer receiver samples need more observations, including during camera motion. Reprojection
    // validates each history tap; geometry/light/material changes still reject stale history.
    float divisor = float(max(hybrid.resolution.z, 1u));
    float scale   = divisor * divisor;
    return frames > 1.0 && scale > 1.0 ? min(frames * scale, 256.0) : frames;
}
vec4 PreviousNormals(ivec2 p)
{
    // Receiver already preserves the geometric normal. Reconstruct its encoded
    // FP16 precision, avoiding a duplicate pair of history channels.
    vec2 geometric = EncodeGBufferNormal(DecodeGBufferNormal(unpackUnorm2x16(texelFetch(previousReceiver, p, 0).r)));
    return vec4(texelFetch(previousNormal, p, 0).rg, unpackHalf2x16(packHalf2x16(geometric)));
}
#ifdef HYBRID_CACHE_HISTORY_GUIDES
// A 16x16 group reuses the exact receiver guides in its 9x9 responsive-history
// neighbourhood. Two or three loads per lane replace up to 81 position reconstructions.
shared float historyGuidePositionX[576];
shared float historyGuidePositionY[576];
shared float historyGuidePositionZ[576];
shared float historyGuideNormalX[576];
shared float historyGuideNormalY[576];
shared float historyGuideNormalZ[576];
shared uint  historyGuideIdentity[576];
uint         HybridHistoryGuideIndex(ivec2 p)
{
    ivec2 q = p - ivec2(gl_WorkGroupID.xy) * 16 + 4;
    return uint(q.y * 24 + q.x);
}
vec3 HybridHistoryPosition(ivec2 p)
{
    uint index = HybridHistoryGuideIndex(p);
    return vec3(historyGuidePositionX[index], historyGuidePositionY[index], historyGuidePositionZ[index]);
}
vec3 HybridHistoryNormal(ivec2 p)
{
    uint index = HybridHistoryGuideIndex(p);
    return vec3(historyGuideNormalX[index], historyGuideNormalY[index], historyGuideNormalZ[index]);
}
uint HybridHistoryIdentity(ivec2 p)
{
    return historyGuideIdentity[HybridHistoryGuideIndex(p)];
}
void LoadHistoryGuides()
{
    ivec2 origin = ivec2(gl_WorkGroupID.xy) * 16 - 4;
    for (uint index = gl_LocalInvocationIndex; index < 576u; index += 256u)
    {
        ivec2 q                      = origin + ivec2(index % 24u, index / 24u);
        bool  valid                  = HybridInView(q);
        vec3  position               = valid ? HybridPosition(q) : vec3(0);
        vec3  normal                 = valid ? HybridGeometricNormal(q) : vec3(0);
        historyGuidePositionX[index] = position.x;
        historyGuidePositionY[index] = position.y;
        historyGuidePositionZ[index] = position.z;
        historyGuideNormalX[index]   = normal.x;
        historyGuideNormalY[index]   = normal.y;
        historyGuideNormalZ[index]   = normal.z;
        historyGuideIdentity[index]  = valid ? texelFetch(receiverSurface, q, 0).g : 0u;
    }
    barrier();
}
#else
#    define HybridHistoryPosition HybridPosition
#    define HybridHistoryNormal   HybridGeometricNormal
uint HybridHistoryIdentity(ivec2 p)
{
    return texelFetch(receiverSurface, p, 0).g;
}
#endif
#include "hybrid_radiance_history.glsl"

// Rejection code of a previous reflection texel for this receiver, zero when it continues the
// same lobe. Codes match the diffuse history's, plus 7 roughness and 8 view or reflection parallax.
float ReflectionTapRejection(ivec2 q,
                             uint  identity,
                             vec3  position,
                             vec3  n,
                             vec3  ng,
                             vec3  view,
                             float roughness,
                             float normalThreshold,
                             bool  parallax,
                             vec3  virtualPosition,
                             float hitDistance)
{
    vec3  oldPosition = texelFetch(previousPosition, q, 0).xyz;
    vec4  normals     = PreviousNormals(q);
    vec3  oldView     = sceneUbo.viewPosition.w < 0.5 ? hybrid.previousViewDirection.xyz
                                                      : normalize(hybrid.previousViewPosition.xyz - oldPosition);
    float oldDistance = texelFetch(previousReflection, q, 0).x;
    // Parallax rejects another reflected surface, including disocclusions, rather than reusing its colour.
    bool  viewChanged = parallax
                          ? oldDistance <= 0.0
                                || length(oldPosition - oldView * oldDistance - virtualPosition) > max(0.005, 0.1 * hitDistance)
                          : dot(oldView, view) < mix(0.9995, 0.9, roughness * roughness);
    float planeLimit  = hybrid.rejection.x * max(1.0, length(position - sceneUbo.viewPosition.xyz));
    return texelFetch(previousReceiver, q, 0).g != identity                                                ? 3.0
         : abs(dot(oldPosition - position, ng)) > planeLimit                                               ? 4.0
         : dot(DecodeGBufferNormal(normals.rg), n) < normalThreshold                                       ? 5.0
         : dot(DecodeGBufferNormal(normals.ba), ng) < 0.99                                                 ? 6.0
         : abs(texelFetch(previousLength, q, 0).a - roughness) > HybridRoughnessTolerance(roughness, true) ? 7.0
         : viewChanged                                                                                     ? 8.0
                                                                                                           : 0.0;
}

// Rejection code of a previous diffuse texel for this receiver, zero when it continues the surface.
float DiffuseTapRejection(ivec2 q, uint identity, vec3 position, vec3 n, vec3 ng)
{
    vec3  oldPosition = texelFetch(previousPosition, q, 0).xyz;
    vec4  oldNormals  = PreviousNormals(q);
    float planeLimit  = hybrid.rejection.x * max(1.0, length(position - sceneUbo.viewPosition.xyz));
    return texelFetch(previousReceiver, q, 0).g != identity  ? 3.0
         : abs(dot(oldPosition - position, ng)) > planeLimit ? 4.0
         : dot(DecodeGBufferNormal(oldNormals.rg), n) < 0.9  ? 5.0
         : dot(DecodeGBufferNormal(oldNormals.ba), ng) < 0.9 ? 6.0
                                                             : 0.0;
}

// ivec2(-1000) marks that no history tap was selected yet.
bool HybridCloserTap(ivec2 q, vec2 previousPixel, ivec2 nearest)
{
    vec2 offset = vec2(q) - previousPixel;
    vec2 best   = vec2(nearest) - previousPixel;
    return nearest.x == -1000 || dot(offset, offset) < dot(best, best);
}

// A sample whose history is shorter than ZEN_HYBRID_BOUNDED_HISTORY frames reaches the screen almost
// unfiltered. Its luminance is bounded by ZEN_HYBRID_SAMPLE_BOUND times a local reference; receivers
// with longer histories are never bounded. The reference is the mean luminance of (sky, bounce,
// reflection) over the previous texels around the reprojected position whose history is long
// enough and valid this frame. A texel revealed at the screen border reprojects off-screen; it
// uses the current raw samples around p instead. Otherwise (first frames, resets) there is no
// reference (negative) and the sample stays unbounded: bounding every history's first samples
// against raw neighbours biased whole histories low (Doc/HybridGI/SparkFixPlan.md).
vec3 HybridHistoryReference(ivec2 p, ivec2 base)
{
    // A reset (invalid previous frame, bounce or reflection restart) leaves stale lengths.
    vec3 current = vec3(hybrid.sampling.w != 0u, hybrid.sampling.w != 0u && hybrid.bounce.y != 0u,
                        hybrid.sampling.w != 0u && hybrid.reflection.y != 0u);
    vec3 total = vec3(0), count = vec3(0);
    bool inView = false;
    for (int y = -1; y <= 2; ++y)
    {
        for (int x = -1; x <= 2; ++x)
        {
            ivec2 q = base + ivec2(x, y);
            inView  = inView || HybridInView(q);
            if (HybridInView(q) && texelFetch(previousReceiver, q, 0).g != 0u)
            {
                vec3 frames =
                    vec3(texelFetch(previousLength, q, 0).r, hybrid.bounce.x != 0u ? texelFetch(previousBounce, q, 0).a : 0.0,
                         hybrid.reflection.x != 0u ? texelFetch(previousReflection, q, 0).w : 0.0);
                vec3 value  = vec3(HybridLuminance(texelFetch(previousSky, q, 0).rgb),
                                   hybrid.bounce.x != 0u ? HybridLuminance(texelFetch(previousBounce, q, 0).rgb) : 0.0,
                                   HybridLuminance(texelFetch(previousSpecular, q, 0).rgb));
                vec3 valid  = current * vec3(greaterThanEqual(frames, vec3(ZEN_HYBRID_BOUNDED_HISTORY)));
                total      += value * valid;
                count      += valid;
            }
        }
    }
    for (int y = -2; y <= 2 && !inView; ++y)
    {
        for (int x = -2; x <= 2; ++x)
        {
            ivec2 q = p + ivec2(x, y);
            if ((x != 0 || y != 0) && HybridInView(q) && texelFetch(receiverSurface, q, 0).g != 0u
                && texelFetch(receiverDepth, q, 0).r < 1.0)
            {
                vec4 sky        = texelFetch(rawSky, q, 0);
                vec4 bounce     = hybrid.bounce.x != 0u ? texelFetch(rawBounce, q, 0) : vec4(0, 0, 0, -1);
                bool reflected  = hybrid.reflection.x != 0u && texelFetch(rawReflection, q, 0).y >= 0.0;
                vec3 valid      = current * vec3(sky.a >= 0.0, bounce.a >= 0.0, reflected);
                total          += vec3(HybridLuminance(sky.rgb), HybridLuminance(bounce.rgb),
                                       HybridLuminance(texelFetch(rawSpecular, q, 0).rgb))
                                * valid;
                count          += valid;
            }
        }
    }
    return mix(vec3(-1), total / max(count, vec3(1)), greaterThan(count, vec3(0)));
}

vec3 HybridBoundSample(vec3 value, float reference)
{
    return value * HybridSampleBound(HybridLuminance(value), reference);
}

// C=(1-S)*H uses its own reprojection, luminance moments and history length. S keeps surface motion.
// For a locally planar mirror, x-view*hitDistance is the virtual image behind the receiver.
// Projecting that point accounts for reflection parallax as the camera moves along the surface.
vec4 ReconstructReflection(ivec2      p,
                           vec3       position,
                           vec3       n,
                           vec3       ng,
                           vec3       view,
                           uint       identity,
                           float      roughness,
                           vec2       surfacePixel,
                           inout vec3 radiance,
                           out float  reason)
{
    vec2  rawGuide = texelFetch(rawReflection, p, 0).xy;
    float distance = rawGuide.x;
    bool  observed = rawGuide.y >= 0.0;
    // A stochastic miss must still average its zero radiance with preceding hits. Borrow a current
    // same-surface distance for parallax when a neighbour hit; never infer radiance from this guide.
    if (distance <= 0.0 && roughness < 0.4)
    {
        float count = 0.0;
        for (int y = -2; y <= 2; ++y)
        {
            for (int x = -2; x <= 2; ++x)
            {
                ivec2 q = p + ivec2(x, y);
                if (!HybridInView(q) || texelFetch(receiverSurface, q, 0).g != identity
                    || dot(HybridGeometricNormal(q), ng) < 0.99
                    || abs(dot(HybridPosition(q) - position, ng)) > hybrid.rejection.x)
                {
                    continue;
                }
                float sampleDistance = texelFetch(rawReflection, q, 0).x;
                if (sampleDistance > 0.0)
                {
                    distance += sampleDistance;
                    count    += 1.0;
                }
            }
        }
        distance /= max(count, 1.0);
    }
    float luminance       = HybridLuminance(radiance);
    vec2  moments         = vec2(luminance, luminance * luminance);
    vec4  oldValue        = vec4(0);
    vec2  oldMoments      = vec2(0);
    float weight          = 0.0;
    bool  reference       = hybrid.rejection.w > 0.5;
    bool  moving          = length(surfacePixel - vec2(p)) > 0.01 || hybrid.previousViewPosition != sceneUbo.viewPosition;
    bool  parallax        = roughness < 0.4 && !reference && moving;
    vec2  previousPixel   = surfacePixel;
    vec3  virtualPosition = position - view * distance;
    vec4  virtualClip     = hybrid.previousProjectionView * vec4(virtualPosition, 1);
    bool  valid           = hybrid.reflection.y != 0u && hybrid.rejection.z > 0.0 && identity != 0u && HybridMotion(p).z > 0.0;
    // Rejection codes match the diffuse history's, plus 7 roughness and 8 view or reflection parallax.
    reason = valid ? 2.0 : 1.0;
    if (parallax)
    {
        reason        = valid && !(distance > 0.0 && virtualClip.w > 0.0) ? 8.0 : reason;
        valid         = valid && distance > 0.0 && virtualClip.w > 0.0;
        float divisor = float(max(hybrid.resolution.z, 1u));
        vec2  extent  = hybrid.resolution.x != 0u ? vec2(hybrid.resolution.xy) : vec2(textureSize(receiverDepth, 0));
        previousPixel =
            ((virtualClip.xy / max(virtualClip.w, 1e-20) * 0.5 + 0.5) * extent - 0.5 - vec2(floor(divisor * 0.5))) / divisor;
    }
    if (valid)
    {
        // During camera motion the reprojected taps land on neighbouring normal-map and roughness
        // texels, at reduced and native resolution alike (native moving sparks were mostly this,
        // Doc/HybridGI/SparkFixPlan.md). Rough lobes tolerate the shading-normal variation; glossy
        // lobes keep 0.99 and geometric-normal rejection stays strict.
        float normalThreshold = mix(0.99, 0.9, roughness * roughness);
        ivec2 base            = ivec2(floor(previousPixel));
        vec2  f               = fract(previousPixel);
        for (int y = 0; y < 2; ++y)
        {
            for (int x = 0; x < 2; ++x)
            {
                ivec2 q = base + ivec2(x, y);
                if (HybridInView(q))
                {
                    float rejected = ReflectionTapRejection(q, identity, position, n, ng, view, roughness, normalThreshold,
                                                            parallax, virtualPosition, distance);
                    reason         = rejected > 0.0 ? rejected : reason;
                    if (rejected == 0.0)
                    {
                        float w     = (x == 0 ? 1.0 - f.x : f.x) * (y == 0 ? 1.0 - f.y : f.y);
                        vec4  data  = texelFetch(previousReflection, q, 0);
                        oldValue   += vec4(texelFetch(previousSpecular, q, 0).rgb, data.w) * w;
                        oldMoments += data.yz * w;
                        weight     += w;
                    }
                }
            }
        }
        // Thin and normal-mapped receivers often reproject next to, not onto, their previous
        // texels. When every bilinear tap fails, continue from the nearest previous texel of the
        // surrounding 4x4 window that passes the same tests.
        ivec2 nearest = ivec2(-1000);
        for (int y = -1; y <= 2 && weight < 0.01; ++y)
        {
            for (int x = -1; x <= 2; ++x)
            {
                ivec2 q = base + ivec2(x, y);
                if ((x < 0 || x > 1 || y < 0 || y > 1) && HybridInView(q)
                    && ReflectionTapRejection(q, identity, position, n, ng, view, roughness, normalThreshold, parallax,
                                              virtualPosition, distance)
                           == 0.0
                    && HybridCloserTap(q, previousPixel, nearest))
                {
                    nearest = q;
                }
            }
        }
        if (weight < 0.01 && nearest.x != -1000)
        {
            vec4 data  = texelFetch(previousReflection, nearest, 0);
            oldValue   = vec4(texelFetch(previousSpecular, nearest, 0).rgb, data.w);
            oldMoments = data.yz;
            weight     = 1.0;
        }
    }
    float frames = weight >= 0.01 ? oldValue.a / weight + 1.0 : 1.0;
    if (observed && frames < ZEN_HYBRID_BOUNDED_HISTORY && !reference)
    {
        radiance  = HybridBoundSample(radiance, HybridHistoryReference(p, ivec2(floor(surfacePixel))).z);
        luminance = HybridLuminance(radiance);
        moments   = vec2(luminance, luminance * luminance);
    }
    float length = observed ? 1.0 : 0.0;
    if (weight >= 0.01)
    {
        reason       = 0.0;
        oldValue    /= weight;
        oldMoments  /= weight;
        float limit  = reference ? float(hybrid.sampling.z)
                                 : HybridHistoryLimit(min(float(hybrid.sampling.z), roughness < 0.2 ? 4.0 : 16.0));
        if (observed && !reference && (hybrid.reflection.z != 0u || moving))
        {
            // The box guards camera motion only. With one ray per pixel, dim reflections often leave a
            // whole 9x9 neighbourhood without a hit; after a cache change the box then emptied valid
            // history frame after frame (dark floor C fell 85% by frame 8 with C unchanged, P6 review).
            HybridUpdateRadianceHistory(p, identity, position, ng, rawSpecular, rawReflectionPrevious,
                                        hybrid.reflection.w != 0u, moving, oldValue, oldMoments);
            // Camera motion alone does not invalidate a matched rough reflection. Keep the fast
            // response for glossy parallax and lighting changes, without discarding coarse-GI history.
            limit = min(limit, hybrid.reflection.z != 0u || roughness < 0.4 ? 8.0 : HybridHistoryLimit(8.0));
        }
        length      = min(oldValue.a + (observed ? 1.0 : 0.0), limit);
        float alpha = observed ? 1.0 / max(length, 1.0) : 0.0;
        radiance    = mix(oldValue.rgb, radiance, alpha);
        moments     = mix(oldMoments, moments, alpha);
    }
    if (!observed && weight < 0.01)
    {
        radiance = vec3(0);
        moments  = vec2(0);
    }
    if (identity == 0u)
    {
        radiance = vec3(0);
        moments  = vec2(0);
        length   = 0.0;
    }
    return vec4(distance, moments, length);
}

struct DiffuseHistory
{
    vec4  sky;
    vec4  spec;
    vec4  moments;
    vec4  bounce;
    vec2  frames;
    vec2  bounceMoments;
    float weight;
    float specWeight;
};

void AccumulateDiffuseTap(ivec2 q, float w, bool rays, float roughness, vec3 view, inout DiffuseHistory history)
{
    vec3 oldPosition    = texelFetch(previousPosition, q, 0).xyz;
    vec4 moments        = texelFetch(previousMoments, q, 0);
    vec4 lengths        = texelFetch(previousLength, q, 0);
    history.weight     += w;
    history.sky        += texelFetch(previousSky, q, 0) * w;
    history.moments.rg += moments.rg * w;
    history.frames.x   += lengths.x * w;
    if (rays)
    {
        history.bounce        += texelFetch(previousBounce, q, 0) * w;
        history.bounceMoments += texelFetch(previousBounceMoments, q, 0).rg * w;
    }
    vec3 previousView = sceneUbo.viewPosition.w < 0.5 ? hybrid.previousViewDirection.xyz
                                                      : normalize(hybrid.previousViewPosition.xyz - oldPosition);
    if (abs(lengths.a - roughness) < HybridReflectionRoughnessTolerance(roughness)
        && dot(previousView, view) > mix(0.9995, 0.9, roughness * roughness))
    {
        history.specWeight += w;
        history.spec       += texelFetch(previousSpecular, q, 0) * w;
        history.moments.ba += moments.ba * w;
        history.frames.y   += lengths.y * w;
    }
}

void main()
{
#ifdef HYBRID_CACHE_HISTORY_GUIDES
    LoadHistoryGuides();
#endif
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    if (!HybridInView(p))
    {
        return;
    }
    vec4  sky = texelFetch(rawSky, p, 0), spec = texelFetch(rawSpecular, p, 0);
    bool  skySample      = sky.a >= 0.0;
    bool  specularSample = spec.a >= 0.0;
    vec3  position = HybridPosition(p), n = HybridNormal(p), ng = HybridGeometricNormal(p);
    uvec2 identity = texelFetch(receiverSurface, p, 0).rg;
    if (texelFetch(receiverDepth, p, 0).r >= 1.0)
    {
        identity = uvec2(0);
    }
    vec4 motion        = HybridMotion(p);
    vec2 previousPixel = vec2(p) + motion.xy * 0.5 * vec2(textureSize(receiverDepth, 0));
    if (hybrid.rejection.w > 0.5)
    {
        previousPixel = vec2(p);
    }
    ivec2          base    = ivec2(floor(previousPixel));
    vec2           f       = fract(previousPixel);
    DiffuseHistory history = DiffuseHistory(vec4(0), vec4(0), vec4(0), vec4(0), vec2(0), vec2(0), 0.0, 0.0);
    // Bounce histories are allocated at full size only while ray-hit bounce is selected.
    bool  rays         = hybrid.bounce.x != 0u;
    vec4  bounce       = rays ? vec4(texelFetch(rawBounce, p, 0).rgb, 1.0) : vec4(0);
    bool  bounceSample = !rays || texelFetch(rawBounce, p, 0).a >= 0.0;
    float reason       = hybrid.sampling.w == 0u ? 1.0 : 2.0;
    float roughness    = texelFetch(receiverRoughness, p, 0).g;
    vec3  view =
        sceneUbo.viewPosition.w < 0.5 ? normalize(sceneUbo.lightInfo.yzw) : normalize(sceneUbo.viewPosition.xyz - position);
    if (hybrid.sampling.w != 0u && hybrid.rejection.z > 0.0 && motion.z > 0.0 && identity.g != 0u)
    {
        for (int y = 0; y < 2; ++y)
        {
            for (int x = 0; x < 2; ++x)
            {
                ivec2 q = base + ivec2(x, y);
                if (HybridInView(q))
                {
                    float rejected = DiffuseTapRejection(q, identity.g, position, n, ng);
                    reason         = rejected > 0.0 ? rejected : reason;
                    if (rejected == 0.0)
                    {
                        AccumulateDiffuseTap(q, (x == 0 ? 1.0 - f.x : f.x) * (y == 0 ? 1.0 - f.y : f.y), rays, roughness, view,
                                             history);
                    }
                }
            }
        }
        // As for reflections: continue from the nearest passing texel of the 4x4 window when
        // every bilinear tap fails.
        ivec2 nearest = ivec2(-1000);
        for (int y = -1; y <= 2 && history.weight < 0.01; ++y)
        {
            for (int x = -1; x <= 2; ++x)
            {
                ivec2 q = base + ivec2(x, y);
                if ((x < 0 || x > 1 || y < 0 || y > 1) && HybridInView(q)
                    && DiffuseTapRejection(q, identity.g, position, n, ng) == 0.0 && HybridCloserTap(q, previousPixel, nearest))
                {
                    nearest = q;
                }
            }
        }
        if (history.weight < 0.01 && nearest.x != -1000)
        {
            AccumulateDiffuseTap(nearest, 1.0, rays, roughness, view, history);
        }
    }
    vec4  oldSky = history.sky, oldSpec = history.spec, oldMoments = history.moments, oldBounce = history.bounce;
    vec2  oldLength = history.frames, oldBounceMoments = history.bounceMoments;
    float weight = history.weight, specWeight = history.specWeight;
    // Bound sky and bounce samples that start or nearly start a history (HybridHistoryReference).
    float skyFrames    = weight >= 0.01 ? oldLength.x / weight + 1.0 : 1.0;
    float bounceFrames = rays && hybrid.bounce.y != 0u && weight >= 0.01 ? oldBounce.a / weight + 1.0 : 1.0;
    bool  boundSky     = skySample && skyFrames < ZEN_HYBRID_BOUNDED_HISTORY;
    bool  boundBounce  = rays && bounceSample && bounceFrames < ZEN_HYBRID_BOUNDED_HISTORY;
    if ((boundSky || boundBounce) && identity.g != 0u && hybrid.rejection.w < 0.5)
    {
        vec3 reference = HybridHistoryReference(p, base);
        sky.rgb        = boundSky ? HybridBoundSample(sky.rgb, reference.x) : sky.rgb;
        bounce.rgb     = boundBounce ? HybridBoundSample(bounce.rgb, reference.y) : bounce.rgb;
    }
    float luminance = HybridLuminance(sky.rgb);
    vec4  moments   = vec4(luminance, luminance * luminance, spec.a, spec.a * spec.a);
    vec2  lengths   = vec2(skySample ? 1.0 : 0.0, specularSample ? 1.0 : 0.0);
    if (weight >= 0.01)
    {
        lengths.x   = min(oldLength.x / weight + (skySample ? 1.0 : 0.0), HybridHistoryLimit(float(hybrid.sampling.z)));
        float alpha = skySample ? 1.0 / max(lengths.x, 1.0) : 0.0;
        sky         = mix(oldSky / weight, sky, alpha);
        moments.rg  = mix(oldMoments.rg / weight, moments.rg, alpha);
        reason      = 0.0;
    }
    if (!skySample && weight < 0.01)
    {
        sky        = vec4(0);
        moments.rg = vec2(0);
    }
    if (specWeight >= 0.01)
    {
        lengths.y   = min(oldLength.y / specWeight + (specularSample ? 1.0 : 0.0),
                          (hybrid.rejection.w > 0.5
                               ? float(hybrid.sampling.z)
                               : HybridHistoryLimit(mix(4.0, float(hybrid.sampling.z), smoothstep(0.04, 0.4, roughness)))));
        float alpha = specularSample ? 1.0 / max(lengths.y, 1.0) : 0.0;
        spec.a      = mix(oldSpec.a / specWeight, spec.a, alpha);
        moments.ba  = mix(oldMoments.ba / specWeight, moments.ba, alpha);
    }
    if (!specularSample && specWeight < 0.01)
    {
        spec.a     = 0.0;
        moments.ba = vec2(0);
    }
    if (identity.g == 0u)
    {
        lengths = vec2(0);
        sky     = vec4(0);
        spec    = vec4(0);
        moments = vec4(0);
    }
    float bounceLuminance = HybridLuminance(bounce.rgb);
    vec2  bounceMoments   = vec2(bounceLuminance, bounceLuminance * bounceLuminance);
    if (rays && hybrid.bounce.y != 0u && weight >= 0.01)
    {
        oldBounce        /= weight;
        oldBounceMoments /= weight;
        // Steady diffuse bounce uses the configured history; cache changes still respond within eight frames.
        float limit = HybridHistoryLimit(float(hybrid.sampling.z));
        if (bounceSample && hybrid.bounce.z != 0u && hybrid.rejection.w < 0.5)
        {
            HybridUpdateRadianceHistory(p, identity.g, position, ng, rawBounce, rawBouncePrevious, hybrid.bounce.w != 0u, true,
                                        oldBounce, oldBounceMoments);
            limit = min(limit, 8.0);
        }
        bounce.a      = min(oldBounce.a + (bounceSample ? 1.0 : 0.0), limit);
        float alpha   = bounceSample ? 1.0 / max(bounce.a, 1.0) : 0.0;
        bounce.rgb    = mix(oldBounce.rgb, bounce.rgb, alpha);
        bounceMoments = mix(oldBounceMoments, bounceMoments, alpha);
    }
    else if (!bounceSample)
    {
        bounce        = vec4(0);
        bounceMoments = vec2(0);
    }
    if (identity.g == 0u)
    {
        bounce        = vec4(0);
        bounceMoments = vec2(0);
    }
    if (rays)
    {
        imageStore(nextBounce, p, bounce);
        imageStore(nextBounceMoments, p, vec4(bounceMoments, 0, 0));
    }
    if (hybrid.reflection.x != 0u)
    {
        float reflectionReason = 0.0;
        vec4  reflection =
            ReconstructReflection(p, position, n, ng, view, identity.g, roughness, previousPixel, spec.rgb, reflectionReason);
        imageStore(nextReflection, p, reflection);
        // Diffuse and reflection rejection reasons share the channel (debug_hybrid.frag).
        reason += 16.0 * reflectionReason;
    }
    imageStore(nextSky, p, sky);
    imageStore(nextSpecular, p, spec);
    imageStore(nextMoments, p, moments);
    imageStore(nextLength, p, vec4(lengths, reason, roughness));
    imageStore(nextPosition, p, vec4(position, roughness));
    imageStore(nextNormal, p, vec4(EncodeGBufferNormal(n), 0, 0));
    imageStore(nextReceiver, p, uvec4(identity, 0, 0));
}
