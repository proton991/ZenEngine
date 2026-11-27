#define DENOISE_GUIDE_ONLY
#include "gi_denoise_common.glsl"
#include "gi_common.glsl"
#include "screen_probe.glsl"
#include "world_cache_lighting.glsl"
#ifdef SCREEN_PROBE_HISTORY_SOURCES
// The environment's compact sources are exact per pixel (screen_probe_sources.comp), and the gather adds
// them to the sky. Resampling them with the history would blur their hard shadow edges during motion,
// and the edge would widen the clamp's neighbourhood variance enough to keep the blur. The history
// keeps only the probes' residual sky; screen_probe_sky_sources.comp adds the current sources again.
#    define SCREEN_PROBE_DIFFUSE_ONLY
#    include "screen_probe_source_gather.glsl"
#endif

layout(std140, set = 1, binding = 133) uniform uScreenProbeHistory
{
    mat4 previousInverseViewProjection;
    vec4 previousWorldOrigin;
}
pixelHistory;
layout(set = 1, binding = 134) uniform sampler2D inputSky;
layout(set = 1, binding = 135) uniform sampler2D inputBounce;
layout(set = 1, binding = 136) uniform sampler2D previousSky;
layout(set = 1, binding = 137) uniform sampler2D previousBounce;
layout(set = 1, binding = 138, rgba16f) writeonly uniform image2D outputSky;
layout(set = 1, binding = 139, rgba16f) writeonly uniform image2D outputBounce;
layout(std430, set = 1, binding = 87) readonly buffer FilteredScreenProbeLighting
{
    vec4 filteredProbeLighting[];
};
struct PackedProbeHistoryGuide
{
    uint depth;
    uint normals;
    uint identity;
};
layout(std430, set = 1, binding = 140) readonly buffer PreviousScreenProbeHistoryGuide
{
    PackedProbeHistoryGuide previousGuides[];
};
layout(std430, set = 1, binding = 141) writeonly buffer ScreenProbeHistoryGuide
{
    PackedProbeHistoryGuide outputGuides[];
};

layout(local_size_x = ZEN_SCREEN_PROBE_SPACING, local_size_y = ZEN_SCREEN_PROBE_SPACING) in;
const float kPixelHistoryLimit = 32.0;
const int   kTileWidth         = int(ZEN_SCREEN_PROBE_SPACING);
const int   kVarianceRadius    = 2;
const int   kNeighborhoodWidth = kTileWidth + 2 * kVarianceRadius;
const int   kHistoryLanes      = kTileWidth * kTileWidth;
shared vec2 gatherLuminance[kNeighborhoodWidth * kNeighborhoodWidth];
shared vec4 gatherRows[kTileWidth * kNeighborhoodWidth];
shared uint needsVariance;

struct HistoryProbe
{
    vec3  position;
    uint  identity;
    vec3  normal;
    float change;
    vec2  noise;
    float movingShare;
    float movingAge;
};
shared HistoryProbe historyProbes[18];

// The gathered sky at p without the diffuse light of the per-pixel sources, when the gather lights them.
vec3 PixelResidualSky(ivec2 p)
{
    vec3 sky = texelFetch(inputSky, p, 0).rgb;
#ifdef SCREEN_PROBE_HISTORY_SOURCES
    vec3 sources = vec3(0), unused;
    GatherProbeSources(p, sources, unused);
    sky = max(sky - sources, vec3(0));
#endif
    return sky;
}

uint PixelHistoryAddress(ivec2 p, ivec2 extent)
{
    return uint(p.y * extent.x + p.x);
}

bool PixelHistoryInView(ivec2 p, ivec2 extent)
{
    return all(greaterThanEqual(p, ivec2(0))) && all(lessThan(p, extent));
}

void LoadHistoryProbe(uint lane)
{
    ivec2        coordinate = ivec2(gl_WorkGroupID.xy) + ivec2(int(lane % 9u % 3u), int(lane % 9u / 3u)) - 1;
    HistoryProbe value      = HistoryProbe(vec3(0), 0u, vec3(0), 0.0, vec2(0), 0.0, kPixelHistoryLimit);
    if (PixelHistoryInView(coordinate, ivec2(screenProbe.grid.xy)))
    {
        uint        base     = uint(coordinate.y) * screenProbe.grid.x + uint(coordinate.x);
        bool        adaptive = lane >= 9u;
        uint        index    = adaptive ? screenProbe.grid.z + base / screenProbe.adaptiveTiles : base;
        ScreenProbe probe    = probes[index];
        if (probe.position.w != 0.0 && (!adaptive || all(equal(ivec2(probe.source.yz / screenProbe.spacing), coordinate))))
        {
            vec4 stats     = filteredProbeLighting[ProbeGatherStatisticsAddress(index)];
            value.position = probe.position.xyz;
            value.identity = probe.source.w;
            value.normal   = probe.normal.xyz;
            value.change   = stats.w;
            value.noise    = max(stats.xy, vec2(0));
            if (screenProbe.layoutInfo.y != 0u)
            {
                float moving      = max(probeLighting[ProbeLightingAddress(index, kProbeMovingStatistics)].x, 0.0);
                float settled     = max(probeLighting[ProbeLightingAddress(index, 9u)].x, 0.0);
                value.movingShare = moving / max(settled + moving, 1e-20);
                value.movingAge =
                    clamp(probeLighting[ProbeLightingAddress(index, kProbeMovingResponse)].z, 1.0, kPixelHistoryLimit);
            }
        }
    }
    historyProbes[lane] = value;
}

bool PixelHistoryProbeMatches(HistoryProbe probe, GIDenoiseGuide guide, float planeLimit)
{
    vec3 delta = probe.position - guide.position;
    return probe.identity == guide.identity && dot(probe.normal, guide.geometric) >= .9
        && max(abs(dot(delta, probe.normal)), abs(dot(delta, guide.geometric))) <= planeLimit;
}

// The pixel history has no same-ray prediction for moving lights. Their unexplained lag error scales
// with their share of the bounce, so the error-balanced length grows as share^(-2/3) beyond the
// moving signal's own age. A small share no longer jumps to the full 32 frames at a threshold.
float PixelHistoryMovingCap(float share, float age)
{
    return min(kPixelHistoryLimit, max(age, 1.0) * pow(max(share, 1e-3), -2.0 / 3.0));
}

void PixelHistoryStats(GIDenoiseGuide guide, ivec2 local, out vec2 noise, out float change, out float movingCap)
{
    vec2  coordinate = (vec2(local) + .5) / float(kTileWidth) + .5;
    ivec2 base       = ivec2(coordinate);
    vec2  fraction   = fract(coordinate);
    float planeLimit = DenoisePlaneLimit(guide.position);
    float weight = 0.0, movingWeight = 0.0, movingAge = 0.0;
    // A pixel with no compatible probe, such as fine detail or an edge, takes the strongest evidence of
    // the nearby probes instead of none, so a light edit still shortens its history.
    float nearbyChange = 0.0, nearbyMovingCap = kPixelHistoryLimit;
    noise  = vec2(0);
    change = 0.0;
    for (int tap = 0; tap < 4; ++tap)
    {
        ivec2 offset = ivec2(tap & 1, tap >> 1);
        ivec2 tile   = base + offset;
        int   index  = tile.y * 3 + tile.x;
        for (int kind = 0; kind < 2; ++kind)
        {
            HistoryProbe nearby = historyProbes[index + 9 * kind];
            nearbyChange        = max(nearbyChange, nearby.change);
            if (nearby.movingShare > 0.0)
            {
                nearbyMovingCap = min(nearbyMovingCap, PixelHistoryMovingCap(nearby.movingShare, nearby.movingAge));
            }
        }
        HistoryProbe probe      = historyProbes[index];
        bool         compatible = PixelHistoryProbeMatches(probe, guide, planeLimit);
        if (!compatible)
        {
            probe      = historyProbes[index + 9];
            compatible = PixelHistoryProbeMatches(probe, guide, planeLimit);
        }
        if (compatible)
        {
            vec2  axes  = mix(1.0 - fraction, fraction, vec2(offset));
            float w     = axes.x * axes.y;
            noise      += w * probe.noise;
            // A changed patch can be visible even where it is only one of the eligible probes.
            change        = max(change, probe.change);
            movingWeight += w * probe.movingShare;
            movingAge    += w * probe.movingShare * probe.movingAge;
            weight       += w;
        }
    }
    noise     /= max(weight, 1e-20);
    noise.y   *= gi.volume.w * gi.volume.w;
    movingCap  = PixelHistoryMovingCap(movingWeight / max(weight, 1e-20), movingAge / max(movingWeight, 1e-20));
    if (weight <= 0.0)
    {
        change    = nearbyChange;
        movingCap = nearbyMovingCap;
    }
}

// The previous pixel shows the same surface: identity, then position and normals from its stored depth.
bool PixelHistoryTapValid(ivec2 p, ivec2 extent, GIDenoiseGuide guide)
{
    bool valid = false;
    if (PixelHistoryInView(p, extent))
    {
        PackedProbeHistoryGuide old = previousGuides[PixelHistoryAddress(p, extent)];
        if (old.identity == guide.identity)
        {
            vec4           normals  = unpackUnorm4x8(old.normals);
            vec3           position = ReconstructGBufferPosition(vec2(p) + .5, extent, uintBitsToFloat(old.depth),
                                                                 pixelHistory.previousInverseViewProjection)
                                    + pixelHistory.previousWorldOrigin.xyz;
            GIDenoiseGuide oldGuide =
                GIDenoiseGuide(position, DecodeGBufferNormal(normals.xy), DecodeGBufferNormal(normals.zw), old.identity, 0.0);
            valid = DenoiseHistoryRejection(guide, oldGuide, false) == 0.0;
        }
    }
    return valid;
}

// Catmull-Rom over the 4 x 4 footprint without its corners, as CubicHistory in the pixel denoiser.
// Bilinear resampling every frame would soften normal-map detail cumulatively during motion.
vec4 PixelHistoryCubic(sampler2D history, ivec2 base, vec2 fraction)
{
    vec4  wx = CatmullRomWeights(fraction.x), wy = CatmullRomWeights(fraction.y);
    vec4  value  = vec4(0);
    float weight = 0.0;
    for (int y = -1; y <= 2; ++y)
    {
        for (int x = -1; x <= 2; ++x)
        {
            if ((x != -1 && x != 2) || (y != -1 && y != 2))
            {
                float w  = wx[x + 1] * wy[y + 1];
                value   += w * texelFetch(history, base + ivec2(x, y), 0);
                weight  += w;
            }
        }
    }
    return max(value / max(weight, 1e-20), vec4(0));
}

// The eight outer taps of the cubic footprint also show the same surface.
bool PixelHistoryCubicValid(ivec2 base, ivec2 extent, GIDenoiseGuide guide)
{
    const ivec2 outer[8] =
        ivec2[8](ivec2(-1, 0), ivec2(-1, 1), ivec2(2, 0), ivec2(2, 1), ivec2(0, -1), ivec2(1, -1), ivec2(0, 2), ivec2(1, 2));
    bool valid = true;
    for (int tap = 0; tap < 8 && valid; ++tap)
    {
        valid = PixelHistoryTapValid(base + outer[tap], extent, guide);
    }
    return valid;
}

void LoadPixelHistory(ivec2                   pixel,
                      ivec2                   extent,
                      GIDenoiseGuide          guide,
                      PackedProbeHistoryGuide currentGuide,
                      out vec4                sky,
                      out vec4                bounce)
{
    sky = bounce       = vec4(0);
    bool stationary    = hybrid.resolution.w != 0u;
    vec4 clip          = hybrid.previousProjectionView * vec4(guide.position, 1);
    vec2 previousPixel = stationary ? vec2(pixel) : (clip.xy / max(clip.w, 1e-20) * .5 + .5) * vec2(extent) - .5;
    if (!stationary)
    {
        // A moving object's previous clip position does not follow camera-only reprojection,
        // even if it slid in its own plane and the world-space guide still matches.
        vec2 motion = texelFetch(receiverMotion, pixel, 0).xy * .5 * vec2(extent);
        if (clip.w <= 0.0 || any(greaterThan(abs(previousPixel - vec2(pixel) - motion), vec2(1.0)))
            || any(lessThanEqual(previousPixel, vec2(-1))) || any(greaterThanEqual(previousPixel, vec2(extent))))
        {
            return;
        }
    }
    ivec2 base     = ivec2(floor(previousPixel));
    vec2  fraction = fract(previousPixel);
    float support  = 0.0;
    bool  complete = !stationary;
    for (int tap = 0; tap < (stationary ? 1 : 4); ++tap)
    {
        ivec2 offset = ivec2(tap & 1, tap >> 1);
        ivec2 p      = base + offset;
        vec2  axes   = mix(1.0 - fraction, fraction, vec2(offset));
        float weight = axes.x * axes.y;
        bool  valid  = false;
        if (stationary && PixelHistoryInView(p, extent))
        {
            PackedProbeHistoryGuide old = previousGuides[PixelHistoryAddress(p, extent)];
            valid = old.identity == guide.identity && old.depth == currentGuide.depth && old.normals == currentGuide.normals;
        }
        // The cubic footprint covers the zero-weight taps too, so every core tap is tested while moving.
        if (!valid && (weight > 0.0 || !stationary))
        {
            valid = PixelHistoryTapValid(p, extent, guide);
        }
        complete = complete && valid;
        if (valid && weight > 0.0)
        {
            sky     += weight * texelFetch(previousSky, p, 0);
            bounce  += weight * texelFetch(previousBounce, p, 0);
            support += weight;
        }
    }
    if (complete && PixelHistoryCubicValid(base, extent, guide))
    {
        sky     = PixelHistoryCubic(previousSky, base, fraction);
        bounce  = PixelHistoryCubic(previousBounce, base, fraction);
        support = 1.0;
    }
    else if (support >= .25)
    {
        sky    /= support;
        bounce /= support;
    }
    else
    {
        sky = bounce = vec4(0);
    }
}

void LoadGatherLuminance(ivec2 extent, ivec2 local, vec2 current)
{
    ivec2 origin = ivec2(gl_WorkGroupID.xy) * kTileWidth;
    gatherLuminance[(local.y + kVarianceRadius) * kNeighborhoodWidth + local.x + kVarianceRadius] = current;
    for (int i = int(gl_LocalInvocationIndex); i < kNeighborhoodWidth * kNeighborhoodWidth; i += kHistoryLanes)
    {
        ivec2 offset = ivec2(i % kNeighborhoodWidth, i / kNeighborhoodWidth) - kVarianceRadius;
        if (!PixelHistoryInView(offset, ivec2(kTileWidth)))
        {
            ivec2 p            = clamp(origin + offset, ivec2(0), extent - 1);
            gatherLuminance[i] = vec2(HybridLuminance(PixelResidualSky(p)), HybridLuminance(texelFetch(inputBounce, p, 0).rgb));
        }
    }
    barrier();
    for (int i = int(gl_LocalInvocationIndex); i < kTileWidth * kNeighborhoodWidth; i += kHistoryLanes)
    {
        int  start   = i / kTileWidth * kNeighborhoodWidth + i % kTileWidth;
        vec4 moments = vec4(0);
        for (int x = 0; x <= 2 * kVarianceRadius; ++x)
        {
            vec2 luminance  = gatherLuminance[start + x];
            moments        += vec4(luminance, luminance * luminance);
        }
        gatherRows[i] = moments;
    }
    barrier();
}

vec2 GatherVariance(ivec2 local)
{
    vec4 moments = vec4(0);
    for (int y = 0; y <= 2 * kVarianceRadius; ++y)
    {
        moments += gatherRows[(local.y + y) * kTileWidth + local.x];
    }
    moments /= 25.0;
    return max(moments.zw - moments.xy * moments.xy, vec2(0));
}

vec3 ClampPixelHistory(vec3 history, vec3 current, float difference, float tolerance)
{
    float excess   = max(difference - tolerance, 0.0);
    float pullback = excess / max(difference, 1e-20);
    return mix(history, current, pullback * pullback);
}

void main()
{
    ivec2                   pixel  = ivec2(gl_GlobalInvocationID.xy);
    ivec2                   extent = imageSize(outputSky);
    bool                    inView = PixelHistoryInView(pixel, extent);
    ivec2                   p      = min(pixel, extent - 1);
    vec3                    sky    = PixelResidualSky(p);
    vec3                    bounce = texelFetch(inputBounce, p, 0).rgb;
    vec2                    age    = vec2(0);
    PackedProbeHistoryGuide packed = PackedProbeHistoryGuide(0u, 0u, 0u);
    vec4                    oldSky = vec4(0), oldBounce = vec4(0);
    vec2                    current    = vec2(HybridLuminance(sky), HybridLuminance(bounce));
    vec2                    difference = vec2(0), noise = vec2(0);
    vec2                    floorTolerance = .2 * max(current, vec2(.005));
    bool                    historyValid   = hybrid.sampling.w != 0u;
    if (historyValid)
    {
        uint lane = gl_LocalInvocationIndex;
        if (lane == 0u)
        {
            needsVariance = 0u;
        }
        if (lane < 18u)
        {
            LoadHistoryProbe(lane);
        }
        barrier();
    }
    if (inView)
    {
        float depth   = texelFetch(receiverDepth, p, 0).r;
        uvec2 surface = texelFetch(receiverSurface, p, 0).rg;
        if (depth < 1.0 && surface.y != 0u)
        {
            vec2 normal    = texelFetch(receiverNormal, p, 0).rg;
            vec2 geometric = unpackUnorm2x16(surface.x);
            packed         = PackedProbeHistoryGuide(floatBitsToUint(depth), packUnorm4x8(vec4(normal, geometric)), surface.y);
            age            = vec2(1);
            if (historyValid)
            {
                vec3           position = ReconstructGBufferPosition(vec2(p) + .5, extent, depth, hybrid.inverseViewProjection)
                                        + hybrid.worldOrigin.xyz;
                GIDenoiseGuide guide =
                    GIDenoiseGuide(position, DecodeGBufferNormal(normal), DecodeGBufferNormal(geometric), surface.y, 0.0);
                LoadPixelHistory(p, extent, guide, packed, oldSky, oldBounce);
                if (oldSky.w > 0.0 || oldBounce.w > 0.0)
                {
                    float change, movingCap;
                    PixelHistoryStats(guide, ivec2(gl_LocalInvocationID.xy), noise, change, movingCap);
                    vec2 sigma = clamp(sqrt(noise) / max(current, vec2(.005)), vec2(.15), vec2(.75));
                    vec2 cap =
                        vec2(WorldHistoryCap(change, sigma.x, kPixelHistoryLimit), WorldHistoryCap(change, sigma.y, movingCap));
                    if (screenProbe.localResponse == 0u)
                    {
                        // Global mode records the light signature's age instead of local evidence. Its probes
                        // also keep at most 16 frames while geometry changes; the pixel history follows both.
                        cap = min(cap, vec2(max(1u, sourceSignature.y)));
                        if (sourceSignature.z < 64u)
                        {
                            cap = min(cap, vec2(16.0));
                        }
                    }
                    age        = clamp(vec2(oldSky.w, oldBounce.w) + 1.0, vec2(1), cap);
                    difference = abs(vec2(HybridLuminance(oldSky.rgb), HybridLuminance(oldBounce.rgb)) - current);
                    if (any(bvec2(age.x > 1.0 && difference.x > max(3.0 * sqrt(noise.x), floorTolerance.x),
                                  age.y > 1.0 && difference.y > max(3.0 * sqrt(noise.y), floorTolerance.y))))
                    {
                        atomicOr(needsVariance, 1u);
                    }
                }
            }
        }
    }
    if (historyValid)
    {
        barrier();
        ivec2 local = ivec2(gl_LocalInvocationID.xy);
        if (needsVariance != 0u)
        {
            LoadGatherLuminance(extent, local, current);
            if (inView)
            {
                vec2 tolerance = max(3.0 * sqrt(noise + GatherVariance(local)), floorTolerance);
                oldSky.rgb     = ClampPixelHistory(oldSky.rgb, sky, difference.x, tolerance.x);
                oldBounce.rgb  = ClampPixelHistory(oldBounce.rgb, bounce, difference.y, tolerance.y);
            }
        }
    }
    if (inView)
    {
        imageStore(outputSky, p, vec4(mix(oldSky.rgb, sky, 1.0 / max(age.x, 1.0)), age.x));
        imageStore(outputBounce, p, vec4(mix(oldBounce.rgb, bounce, 1.0 / max(age.y, 1.0)), age.y));
        outputGuides[PixelHistoryAddress(p, extent)] = packed;
    }
}
