#include "hybrid_guides.glsl"
#include "hybrid_sampling.glsl"
#define HYBRID_MIXED_PROVIDER
#include "hybrid_provider.glsl"
#define HIT_LIGHT_CACHE
#include "hit_radiance.glsl"
#include "environment_sampling.glsl"
#include "hybrid_sky_guide.glsl"
#if defined(HYBRID_TRACE_HALF)
// One 8x8 workgroup per 16x16 tile of the visibility guide (HybridGIRenderer::kGuideTile).
// Each thread owns a 2x2 receiver block, tracing diffuse rays for one rotating leader and
// specular rays for all four receivers.
layout(local_size_x = 8, local_size_y = 8) in;
const uint TRACE_THREADS = 64u;
#elif defined(HYBRID_TRACE_FALLBACK)
// One thread per receiver of the frame's fallback list, traced at full resolution with its
// tile's pooled guide as published by the half-resolution pass.
layout(local_size_x = 64) in;
const uint TRACE_THREADS = 64u;
#else
// One workgroup per screen tile of the visibility guide (HybridGIRenderer::kGuideTile).
layout(local_size_x = 16, local_size_y = 16) in;
const uint TRACE_THREADS = 256u;
#endif
layout(set = 1, binding = 6) uniform sampler3D voxelOpacity;
layout(set = 1, binding = 7) uniform samplerCube coneEnvironmentMap;
#ifndef HYBRID_TRACE_HALF
layout(set = 1, binding = 8, rgba16f) uniform writeonly image2D rawSky;
layout(set = 1, binding = 12, rgba16f) uniform writeonly image2D rawBounce;
layout(set = 1, binding = 16, rgba16f) uniform writeonly image2D rawBouncePrevious;
#endif
// Every receiver's normal-dependent reconstruction guide, also at half resolution: the upsample
// transfers leaders' sky to their neighbours as a ratio to it, as the spatial filter does.
layout(set = 1, binding = 19, rgba16f) uniform writeonly image2D skyGuide;
#ifndef HYBRID_TRACE_FALLBACK
layout(set = 1, binding = 9, rgba16f) uniform writeonly image2D rawSpecular;
layout(set = 1, binding = 17, rg32f) uniform writeonly image2D rawReflection;
layout(set = 1, binding = 18, rgba16f) uniform writeonly image2D rawReflectionPrevious;
#endif
layout(set = 1, binding = 13) uniform sampler3D hitRadiance;
layout(set = 1, binding = 14) uniform sampler3D hitNormal;
// Base radiance level from before this frame's injection, and the bounce this frame's rays read from
// it (meaningful only when the cache changed; hybrid_temporal.comp).
layout(set = 1, binding = 15) uniform sampler3D hitRadiancePrevious;
layout(set = 1, binding = 23) uniform sampler2D previousSkyMoments;
layout(set = 1, binding = 27) uniform sampler2D previousHistoryLength;
layout(std140, set = 1, binding = 28) uniform uHybridReuseData
{
    uvec4 reuseData;
};
// Whether each receiver used the reduced settled budget in the previous frame.
layout(set = 1, binding = 29) uniform usampler2D previousBudget;
layout(set = 1, binding = 30, r8ui) uniform writeonly uimage2D nextBudget;
#ifdef HYBRID_TRACE_HALF
// One texel per 2x2 block: the leader's diffuse result; guide alpha is the leader's
// phase index, or -1 when the block has no eligible receiver.
layout(set = 1, binding = 31, rgba16f) uniform writeonly image2D halfSky;
layout(set = 1, binding = 32, rgba16f) uniform writeonly image2D halfBounce;
layout(set = 1, binding = 33, rgba16f) uniform writeonly image2D halfBouncePrevious;
layout(set = 1, binding = 34, rgba16f) uniform writeonly image2D halfGuide;
// The frame's full-resolution fallback list and its dispatch, appended by hybrid_diffuse_upsample.comp.
layout(std430, set = 1, binding = 35) writeonly buffer HybridDiffuseFallbackArgs
{
    uint fallbackCount;
    uint fallbackGroupsX;
    uint fallbackGroupsY;
    uint fallbackGroupsZ;
};
// Each tile's pooled guide for the fallback pass: cell weights, row moments and total power.
layout(std430, set = 1, binding = 37) writeonly buffer HybridPooledGuide
{
    float pooledGuide[];
};
#endif
#ifdef HYBRID_TRACE_FALLBACK
layout(std430, set = 1, binding = 35) readonly buffer HybridDiffuseFallbackArgs
{
    uint fallbackCount;
    uint fallbackGroupsX;
    uint fallbackGroupsY;
    uint fallbackGroupsZ;
};
layout(std430, set = 1, binding = 36) readonly buffer HybridDiffuseFallbackPixels
{
    uint fallbackPixels[];
};
layout(std430, set = 1, binding = 37) readonly buffer HybridPooledGuide
{
    float pooledGuide[];
};
#endif
// Pooled-guide record per tile, matching HybridGIRenderer's buffer size.
const uint POOLED_GUIDE_STRIDE = ZEN_HYBRID_GUIDE_TILES + 4u * 6u * ZEN_HYBRID_GUIDE_PER_FACE + 4u;
// Visibility guide: per screen tile, the visible residual radiance (without the receiver's cosine)
// over the environment's tiles, learned from this pass's rays, followed by the frames it averages.
// Each receiver weights it by its own cosine toward the tile centres and draws guided rays from it,
// so its rays go where the sky is both bright and unblocked from that part of the screen.
layout(std430, set = 1, binding = 10) readonly buffer HybridGuidePrevious
{
    float guidePrevious[];
};
#ifndef HYBRID_TRACE_FALLBACK
layout(std430, set = 1, binding = 11) writeonly buffer HybridGuideNext
{
    float guideNext[];
};
#endif
shared float guideWeight[ZEN_HYBRID_GUIDE_TILES];
vec3         GuideCellDirection(uint t)
{
    uint perFace = ZEN_HYBRID_GUIDE_PER_FACE * ZEN_HYBRID_GUIDE_PER_FACE;
    vec2 center  = (vec2(t % ZEN_HYBRID_GUIDE_PER_FACE, (t % perFace) / ZEN_HYBRID_GUIDE_PER_FACE) + 0.5)
                     * (2.0 / float(ZEN_HYBRID_GUIDE_PER_FACE))
                 - 1.0;
    return EnvironmentWorldDirection(t / perFace, center);
}
#if defined(HYBRID_TRACE_HALF) || defined(HYBRID_TRACE_FALLBACK)
// A quarter as many threads share each tile's guide here. Recomputing the cell directions keeps
// the workgroup's shared memory small enough not to limit occupancy.
vec3 GuideDirection(uint t)
{
    return GuideCellDirection(t);
}
#else
shared vec3 guideDirection[ZEN_HYBRID_GUIDE_TILES];
vec3        GuideDirection(uint t)
{
    return guideDirection[t];
}
#endif
#define GUIDE_SUM_BINS ZEN_HYBRID_GUIDE_TILES
// Capture-only specialization. Normal rendering retains one float word per bin.
layout(constant_id = 0) const bool HYBRID_EXACT_GUIDE = false;
const uint HYBRID_GUIDE_WORDS                         = HYBRID_EXACT_GUIDE ? 10u : 1u;
#define GUIDE_SUM_STORAGE_WORDS HYBRID_GUIDE_WORDS
#include "guide_sum.glsl"
shared uint  guideReceivers;
shared float guidePower;
shared float guidePartialPower[TRACE_THREADS];
shared vec4  guideRowMoment[6u * ZEN_HYBRID_GUIDE_PER_FACE];
// Guide reads: the workgroup's shared copy, or the tile's published copy in the fallback pass.
#ifdef HYBRID_TRACE_FALLBACK
uint  pooledBase;
float GuideWeightAt(uint t)
{
    return pooledGuide[pooledBase + t];
}
vec4 GuideRowMomentAt(uint row)
{
    uint base = pooledBase + ZEN_HYBRID_GUIDE_TILES + row * 4u;
    return vec4(pooledGuide[base], pooledGuide[base + 1u], pooledGuide[base + 2u], pooledGuide[base + 3u]);
}
float GuidePowerValue()
{
    return pooledGuide[pooledBase + ZEN_HYBRID_GUIDE_TILES + 4u * 6u * ZEN_HYBRID_GUIDE_PER_FACE];
}
#else
float GuideWeightAt(uint t)
{
    return guideWeight[t];
}
vec4 GuideRowMomentAt(uint row)
{
    return guideRowMoment[row];
}
float GuidePowerValue()
{
    return guidePower;
}
#endif
#ifdef HYBRID_COUNT_RAYS
// Full and half passes write each tile's first two entries and clear the second two, to which
// the fallback pass adds.
#    ifdef HYBRID_TRACE_FALLBACK
layout(std430, set = 1, binding = 26) buffer HybridRayCounts
{
    uvec4 rayCounts[];
};
#    else
layout(std430, set = 1, binding = 26) writeonly buffer HybridRayCounts
{
    uvec4 rayCounts[];
};
#    endif
shared uvec4 computeRaySums[TRACE_THREADS], hardwareRaySums[TRACE_THREADS];
#endif
#ifdef HYBRID_INLINE_RETRACE
// Reference accumulation re-traces near-lamp hits in the trace itself (hybrid_retrace.glsl); this
// marks the hit being shaded as re-traced, so its near-light visibility traces triangles as well.
bool retracedHit = false;
#elif defined(HYBRID_BRICK_TRAVERSAL)
#    include "hybrid_retrace.glsl"
// This invocation's last queued ray; one invocation traces one receiver's diffuse rays.
uint retraceHead = HYBRID_RETRACE_NONE;

// Queues a diffuse ray whose brick hit lies next to a cached point or spot light
// (HitNearLocalLight). Returns false when the queue is full; the ray is then shaded from its brick hit.
bool QueueRetrace(vec3 hitPosition, vec3 direction, float weight, bool source)
{
    uint ray    = atomicAdd(retraceHeader.claimedRays, 1u);
    bool queued = ray < uint(retraceRays.length()) / 2u;
    if (queued)
    {
        // Bricks are conservative, so no triangle lies closer than one fine voxel before the hit.
        retraceRays[ray * 2u]      = uvec4(floatBitsToUint(hitPosition - direction * brick.voxel.x), retraceHead);
        retraceRays[ray * 2u + 1u] = uvec4(floatBitsToUint(direction), floatBitsToUint(source ? -weight : weight));
        retraceHead                = ray;
    }
    return queued;
}

// Lists this invocation's receiver for the re-trace pass when it queued rays; target is its texel
// in the image the pass adds to.
void PublishRetrace(ivec2 target)
{
    if (retraceHead != HYBRID_RETRACE_NONE)
    {
        // Each listed receiver queued at least one ray, so the list fits the ray capacity.
        uint receiver = atomicAdd(retraceHeader.receivers, 1u);
        if (receiver % 64u == 0u)
        {
            atomicAdd(retraceHeader.groups, 1u);
        }
        retraceReceivers[receiver] = uvec2(uint(target.x) | uint(target.y) << 16u, retraceHead);
    }
}
#endif
// Learned frames are averaged equally up to this count, then exponentially.
const float HYBRID_GUIDE_FRAMES = 64.0;

// Bits match VoxelGISettings::Feature. The hardware variant also supports software queries
// so each feature can select its own provider without tracing every signal twice.
const uint SKY_PROVIDER        = 1u;
const uint SPECULAR_PROVIDER   = 2u;
const uint BOUNCE_PROVIDER     = 8u;
const uint REFLECTION_PROVIDER = 16u;
bool       FeatureHardware(uint feature)
{
    return (hybrid.provider.z & feature) != 0u;
}
HybridRayResult TraceFeature(uint feature, vec3 position, vec3 normal, vec3 direction, bool closest)
{
#ifdef HYBRID_COUNT_RAYS
    issuedRayFeature = feature == SKY_PROVIDER ? 0u : feature == SPECULAR_PROVIDER ? 1u : feature == BOUNCE_PROVIDER ? 2u : 3u;
#endif
#ifdef HYBRID_HARDWARE_PROVIDER
    if (FeatureHardware(feature))
    {
        return HardwareTraceRay(HybridTraceOrigin(position, normal, 0.0), direction, 1e20, closest);
    }
#endif
#ifdef HYBRID_BRICK_TRAVERSAL
    // Diffuse hits next to a lamp are re-traced against triangles (TraceDiffuse).
    return BrickTraceRay(BrickTraceOrigin(position, normal), direction, 1e20);
#else
    return SoftwareTraceRay(HybridTraceOrigin(position, normal, 0.0), direction, 1e20, closest);
#endif
}
// Visibility of a nearby point or spot light from a ray hit (hit_radiance.glsl), with the bounce
// provider. Under brick traversal, hits next to a lamp are re-traced with triangle visibility,
// by hybrid_retrace_bricks.comp or, in reference variants, inline.
bool HitLightVisible(HybridRayResult hit, SceneLight light)
{
#ifdef HYBRID_COUNT_RAYS
    issuedRayFeature = 2u;
#endif
#if defined(HYBRID_INLINE_RETRACE)
    vec3 origin = retracedHit ? HybridTraceOrigin(hit.position, hit.normal, 0.0) : BrickTraceOrigin(hit.position, hit.normal);
#elif defined(HYBRID_BRICK_TRAVERSAL)
    vec3 origin = BrickTraceOrigin(hit.position, hit.normal);
#else
    vec3 origin = HybridTraceOrigin(hit.position, hit.normal, 0.0);
#endif
    vec3  delta           = light.positionRange.xyz - origin;
    float distanceToLight = length(delta);
    vec3  direction       = delta / max(distanceToLight, 1e-20);
    // Exclude the endpoint itself, without dropping a finite interval before it.
    distanceToLight = uintBitsToFloat(floatBitsToUint(distanceToLight) - uint(distanceToLight > 0.0));
    bool blocked    = false;
#ifdef HYBRID_HARDWARE_PROVIDER
    if (FeatureHardware(BOUNCE_PROVIDER))
    {
        blocked = distanceToLight > 0.0 && HardwareTraceRay(origin, direction, distanceToLight, false).hit;
    }
    else
#endif
    {
#if defined(HYBRID_INLINE_RETRACE)
        blocked = distanceToLight > 0.0
               && (retracedHit ? SoftwareTraceOpaqueRay(origin, direction, distanceToLight, false).hit
                               : BrickTraceRay(origin, direction, distanceToLight).hit);
#elif defined(HYBRID_BRICK_TRAVERSAL)
        blocked = distanceToLight > 0.0 && BrickTraceRay(origin, direction, distanceToLight).hit;
#else
        blocked = distanceToLight > 0.0 && SoftwareTraceRay(origin, direction, distanceToLight, false).hit;
#endif
    }
    return !blocked;
}

void GuideLearn(uint tile, float value)
{
    if (HYBRID_EXACT_GUIDE)
    {
        GuideSumAdd(tile, value);
    }
    else
    {
        uint previous = guideSumWords[tile], expected;
        do
        {
            expected = previous;
            previous = atomicCompSwap(guideSumWords[tile], expected, floatBitsToUint(uintBitsToFloat(expected) + value));
        } while (previous != expected);
    }
}

float GuideHorizonSupport()
{
    // Triangle visibility resolves grazing openings inside a guide cell. The
    // voxel provider retains its qualified cosine proposal for coarser blockers.
    return 1.0 / float(ZEN_HYBRID_GUIDE_PER_FACE);
}

float GuideCosineWeight(uint tile, vec3 normal)
{
    // Keep a radiance-only part in the proposal. A coarse angular cell can
    // contain directions above the horizon while its centre is below it.
    // The same floor is included in the row mass and the evaluated density.
    return max(GuideWeightAt(tile), 0.0) * (max(dot(normal, GuideDirection(tile)), 0.0) + GuideHorizonSupport());
}

// Guided directions are one two-dimensional warp of the ray's stratified sample, like the
// environment's own row and column sampler, so neighbouring pixels' guided rays stay stratified:
// the environment tiles are laid out as an atlas of the six faces (three by two), u.y draws an atlas
// row, u.x a tile within it, and the remaining fractions place the direction within the tile.
const uint GUIDE_ATLAS_COLUMNS = 3u * ZEN_HYBRID_GUIDE_PER_FACE;
const uint GUIDE_ATLAS_ROWS    = 2u * ZEN_HYBRID_GUIDE_PER_FACE;
uint       GuideAtlasTile(uint x, uint y)
{
    uint face = (y / ZEN_HYBRID_GUIDE_PER_FACE) * 3u + x / ZEN_HYBRID_GUIDE_PER_FACE;
    return (face * ZEN_HYBRID_GUIDE_PER_FACE + y % ZEN_HYBRID_GUIDE_PER_FACE) * ZEN_HYBRID_GUIDE_PER_FACE
         + x % ZEN_HYBRID_GUIDE_PER_FACE;
}

float GuideFaceRow(uint first, vec3 normal)
{
    float left  = dot(normal, GuideDirection(first));
    float right = dot(normal, GuideDirection(first + ZEN_HYBRID_GUIDE_PER_FACE - 1u));
    // Along a cube-face row the unnormalized direction is affine in x. Its
    // cosine therefore changes sign at most once. Whole visible rows reduce
    // exactly to a dot product; only rows crossing the horizon need each bin.
    float weight = 0.0;
    if (left >= 0.0 && right >= 0.0)
    {
        weight = max(dot(normal, GuideRowMomentAt(first / ZEN_HYBRID_GUIDE_PER_FACE).xyz), 0.0);
    }
    else if (left > 0.0 || right > 0.0)
    {
        for (uint x = 0u; x < ZEN_HYBRID_GUIDE_PER_FACE; ++x)
        {
            weight += max(GuideWeightAt(first + x), 0.0) * max(dot(normal, GuideDirection(first + x)), 0.0);
        }
    }
    return weight;
}

// Total of the cosine-weighted guide used by GuideSample and GuidePdf.
// Cache each row once per receiver: all guided samples use the same distribution.
float GuideTotal(vec3 normal, out float fullTotal, out float rows[GUIDE_ATLAS_ROWS])
{
    float total = 0.0;
    fullTotal   = 0.0;
    for (uint y = 0u; y < GUIDE_ATLAS_ROWS; ++y)
    {
        float row = 0.0, mass = 0.0;
        for (uint face = 0u; face < 3u; ++face)
        {
            row  += GuideFaceRow(GuideAtlasTile(face * ZEN_HYBRID_GUIDE_PER_FACE, y), normal);
            mass += GuideRowMomentAt(GuideAtlasTile(face * ZEN_HYBRID_GUIDE_PER_FACE, y) / ZEN_HYBRID_GUIDE_PER_FACE).w;
        }
        fullTotal += row;
        row       += mass * GuideHorizonSupport();
        total     += row;
        rows[y]    = row;
    }
    const uint ratio = ZEN_HYBRID_GUIDE_PER_FACE / ZEN_ENVIRONMENT_TILES_PER_FACE;
    for (uint source = 0u; source < environmentSourceCount; ++source)
    {
        uint tile = environmentSourceTiles[source];
        uint first =
            (tile / ZEN_ENVIRONMENT_TILES_PER_FACE * ZEN_HYBRID_GUIDE_PER_FACE + tile % ZEN_ENVIRONMENT_TILES_PER_FACE) * ratio;
        for (uint y = 0u; y < ratio; ++y)
        {
            for (uint x = 0u; x < ratio; ++x)
            {
                uint t     = first + y * ZEN_HYBRID_GUIDE_PER_FACE + x;
                fullTotal += abs(GuideWeightAt(t)) * max(dot(normal, GuideDirection(t)), 0.0);
            }
        }
    }
    return total;
}

vec3 GuideSample(vec3 normal, float total, float rows[GUIDE_ATLAS_ROWS], vec2 u, out float pdf)
{
    float target = min(u.y * total, uintBitsToFloat(floatBitsToUint(total) - 1u)), running = 0.0, row = 0.0;
    uint  y = 0u;
    for (; y + 1u < GUIDE_ATLAS_ROWS; ++y)
    {
        row = rows[y];
        if (running + row > target)
        {
            break;
        }
        running += row;
    }
    if (y + 1u == GUIDE_ATLAS_ROWS)
    {
        row = rows[y];
    }
    u.y          = clamp((target - running) / max(row, 1e-30), 0.0, 0.99999994);
    target       = min(u.x * row, uintBitsToFloat(floatBitsToUint(max(row, 1e-30)) - 1u));
    running      = 0.0;
    float weight = 0.0;
    uint  x      = 0u;
    for (; x + 1u < GUIDE_ATLAS_COLUMNS; ++x)
    {
        weight = GuideCosineWeight(GuideAtlasTile(x, y), normal);
        if (running + weight > target)
        {
            break;
        }
        running += weight;
    }
    if (x + 1u == GUIDE_ATLAS_COLUMNS)
    {
        weight = GuideCosineWeight(GuideAtlasTile(x, y), normal);
    }
    u.x = clamp((target - running) / max(weight, 1e-30), 0.0, 0.99999994);
    return SampleEnvironmentTileGrid(GuideAtlasTile(x, y), u, ZEN_HYBRID_GUIDE_PER_FACE, ZEN_HYBRID_GUIDE_CDF_OFFSET, pdf);
}

// The guide survives resets that keep the screen and environment (HybridGIRenderer); source
// tiles have their own rays.
bool guideValid;
uint guideBase;

void BuildGuide(uint lane)
{
    guideBase   = (gl_WorkGroupID.y * gl_NumWorkGroups.x + gl_WorkGroupID.x) * (ZEN_HYBRID_GUIDE_TILES + 1u);
    guideValid  = hybrid.provider.y != 0u;
    float power = 0.0;
    for (uint t = lane; t < ZEN_HYBRID_GUIDE_TILES; t += TRACE_THREADS)
    {
        uint perFace = ZEN_HYBRID_GUIDE_PER_FACE * ZEN_HYBRID_GUIDE_PER_FACE;
#if !defined(HYBRID_TRACE_HALF) && !defined(HYBRID_TRACE_FALLBACK)
        guideDirection[t] = GuideCellDirection(t);
#endif
        // Rays are drawn from the guide smoothed over the neighbouring screen tiles (a 5x5 binomial
        // kernel): each tile learns from its own receivers, and sampling borrows their neighbours'
        // statistics, which are noisier per tile than they are different between nearby tiles.
        float weight = 0.0;
        if (guideValid)
        {
            for (int dy = -2; dy <= 2; ++dy)
            {
                for (int dx = -2; dx <= 2; ++dx)
                {
                    ivec2 neighbour = clamp(ivec2(gl_WorkGroupID.xy) + ivec2(dx, dy), ivec2(0), ivec2(gl_NumWorkGroups.xy) - 1);
                    uint  base = (uint(neighbour.y) * gl_NumWorkGroups.x + uint(neighbour.x)) * (ZEN_HYBRID_GUIDE_TILES + 1u);
                    const float poolKernel[5]  = float[5](1, 4, 6, 4, 1);
                    weight                    += poolKernel[dx + 2] * poolKernel[dy + 2] * guidePrevious[base + t];
                }
            }
        }
        // A cosine ray can learn radiance in a dim cell whose CDF mass rounded to zero.
        // Such a cell cannot be sampled by the guide; the cosine proposal retains its support.
        uint cdfEnd      = ZEN_HYBRID_GUIDE_CDF_OFFSET + t * (ZEN_HYBRID_GUIDE_CELL_SIZE + 1u) + ZEN_HYBRID_GUIDE_CELL_SIZE;
        guideWeight[t]   = environmentTiles[cdfEnd] > 0.0 ? weight / 256.0 : 0.0;
        power           += guideWeight[t];
        uint ratio       = ZEN_HYBRID_GUIDE_PER_FACE / ZEN_ENVIRONMENT_TILES_PER_FACE;
        uint sourceTile  = ((t / perFace) * ZEN_ENVIRONMENT_TILES_PER_FACE + (t % perFace) / ZEN_HYBRID_GUIDE_PER_FACE / ratio)
                             * ZEN_ENVIRONMENT_TILES_PER_FACE
                         + (t % ZEN_HYBRID_GUIDE_PER_FACE) / ratio;
        if (EnvironmentSourceTile(sourceTile))
        {
            guideWeight[t] = -guideWeight[t];
        }
        if (HYBRID_EXACT_GUIDE)
        {
            GuideSumClear(t);
        }
        else
        {
            guideSumWords[t] = 0u;
        }
    }
    // Separate power scratch lets the learning bins initialize with the guide,
    // removing the barrier previously needed before clearing reused storage.
    guidePartialPower[lane] = power;
    if (lane == 0u)
    {
        guideReceivers = 0u;
    }
    barrier();
    if (lane == 0u)
    {
        guidePower = 0.0;
        for (uint i = 0u; i < TRACE_THREADS; ++i)
        {
            guidePower += guidePartialPower[i];
        }
    }
    if (lane < 6u * ZEN_HYBRID_GUIDE_PER_FACE)
    {
        vec4 moment = vec4(0);
        for (uint x = 0u; x < ZEN_HYBRID_GUIDE_PER_FACE; ++x)
        {
            uint t  = lane * ZEN_HYBRID_GUIDE_PER_FACE + x;
            moment += max(guideWeight[t], 0.0) * vec4(GuideDirection(t), 1);
        }
        guideRowMoment[lane] = moment;
    }
    barrier();
#ifdef HYBRID_TRACE_HALF
    uint published = (gl_WorkGroupID.y * gl_NumWorkGroups.x + gl_WorkGroupID.x) * POOLED_GUIDE_STRIDE;
    for (uint t = lane; t < ZEN_HYBRID_GUIDE_TILES; t += TRACE_THREADS)
    {
        pooledGuide[published + t] = guideWeight[t];
    }
    if (lane < 6u * ZEN_HYBRID_GUIDE_PER_FACE)
    {
        for (uint c = 0u; c < 4u; ++c)
        {
            pooledGuide[published + ZEN_HYBRID_GUIDE_TILES + lane * 4u + c] = guideRowMoment[lane][c];
        }
    }
    if (lane == 0u)
    {
        pooledGuide[published + ZEN_HYBRID_GUIDE_TILES + 4u * 6u * ZEN_HYBRID_GUIDE_PER_FACE] = guidePower;
    }
#endif
}

bool IsReceiver(ivec2 p)
{
    return HybridInView(p) && texelFetch(receiverDepth, p, 0).r < 1.0 && texelFetch(receiverSurface, p, 0).g != 0u;
}

struct DiffuseResult
{
    vec4 sky;
    vec4 bounce;
    vec4 previous;
    vec3 guide;
    uint reducedBudget;
};

// Sky, source-shadow and ray-hit bounce estimate of one receiver. Its rays learn the guide
// with the given weight: the number of receivers the result represents, or zero.
// Normal-dependent sky guide of a receiver: the environment's colour with the learned visible
// power toward the normal (hybrid_filter.glsl divides sky by it).
vec3 ReconstructionGuide(vec3 n, float fullGuideTotal)
{
    vec3 guide = HybridSkyGuide(n);
    if (guideValid && GuidePowerValue() > 0.0 && fullGuideTotal > 0.0)
    {
        guide = max(guide / HybridLuminance(guide) * (fullGuideTotal / GuidePowerValue()), vec3(1e-3));
    }
    return guide;
}

DiffuseResult TraceDiffuse(ivec2 p, vec3 position, vec3 n, vec3 ng, uint sequence, uint seed, float learning)
{
    vec4  sky = vec4(0), bounce = vec4(0), previous = vec4(0);
    uint  reducedBudget = 0u;
    float scale         = sceneUbo.environment.x * sceneUbo.environment.z * gi.lighting.y;
    float fullGuideTotal;
    float guideRows[GUIDE_ATLAS_ROWS];
    float guideTotal          = GuideTotal(n, fullGuideTotal, guideRows);
    vec3  reconstructionGuide = ReconstructionGuide(n, fullGuideTotal);
    // Shares of guided, residual-environment and cosine rays; without a guide its share goes to
    // the environment. Every density below is the mixture of these three proposals.
    vec3 share = guideTotal > 0.0 ? vec3(0.75, 0.0, 0.25) : vec3(0.0, HYBRID_ENVIRONMENT_SHARE, 1.0 - HYBRID_ENVIRONMENT_SHARE);
    bool residualEnvironment = environmentResidualRows[ZEN_ENVIRONMENT_IMPORTANCE_ROWS] > 0.0
                            && gi.lighting.y * sceneUbo.environment.x * sceneUbo.environment.z > 0.0;
    if (!residualEnvironment)
    {
        share = vec3(0, 0, 1);
    }
    // The base rays remain everywhere. Two extra guided rays reduce the
    // compact guide's variance where previous sky samples found energy.
    // Receivers with a positive guide also need exploration: an
    // all-zero history can otherwise miss a narrow opening after a reset.
    // Reset frames use the full budget, without consulting invalid history.
    bool  adaptive   = hybrid.provider.w != 0u && residualEnvironment;
    bool  explore    = fullGuideTotal > 0.0;
    vec2  oldMoments = hybrid.sampling.w != 0u ? texelFetch(previousSkyMoments, p, 0).rg : vec2(0);
    float variance   = max(oldMoments.y - oldMoments.x * oldMoments.x, 0.0);
    // Retain all six rays on reset, dark/uncertain receivers and disocclusion.
    // Same-pixel history is only meaningful when the camera and surfaces
    // are unchanged; moving frames retain the original exploration policy.
    // Ray-hit bounce shares these rays, so its responsive window after a
    // lighting change also keeps the full budget.
    // Fewer rays raise the per-frame variance by 6/4. A receiver already on
    // the reduced budget therefore leaves it only above 1.5x the entry
    // threshold, so the reduction alone cannot switch it back and forth.
    bool  wasReduced = reuseData.y != 0u && texelFetch(previousBudget, p, 0).r != 0u;
    float relativeVariance =
        (FeatureHardware(SKY_PROVIDER) ? 0.01 : 0.0625) * (wasReduced ? 6.0 / float(hybrid.sampling.y) : 1.0);
    bool responsive = hybrid.bounce.x != 0u && hybrid.bounce.z != 0u;
    bool settled    = reuseData.y != 0u && !responsive && texelFetch(previousHistoryLength, p, 0).r >= float(hybrid.sampling.z)
                   && oldMoments.x > 0.0 && variance <= relativeVariance * oldMoments.x * oldMoments.x;
    if (adaptive && settled)
    {
        reducedBudget = 1u;
    }
    uint diffuseSamples =
        adaptive && !settled && (hybrid.sampling.w == 0u || oldMoments.x > 0.0 || explore) ? 6u : hybrid.sampling.y;
    if (diffuseSamples == 6u && residualEnvironment)
    {
        share = guideTotal > 0.0 ? vec3(5.0 / 6.0, 0, 1.0 / 6.0) : vec3(0, 5.0 / 6.0, 1.0 / 6.0);
    }
    for (uint i = 0u; i < diffuseSamples; ++i)
    {
        // Rays from the same proposal share one temporal Sobol net. The pixel's digital shift
        // preserves spatial stratification while each proposal advances by its own ray count.
        vec2 u = HybridSkySample(uvec2(p), sequence, seed + 0x1000u + i);
        // Four-ray budgets split 3:1; six-ray budgets split 5:1. Other
        // configured counts use a stratified proposal choice per ray.
        float choice   = (diffuseSamples % 4u == 0u || diffuseSamples == 6u) ? (float(i) + 0.5) / float(diffuseSamples) : u.x;
        uint  proposal = choice < share.x ? 0u : choice < share.x + share.y ? 1u : 2u;
        if (diffuseSamples % 4u == 0u || diffuseSamples == 6u)
        {
            uint first = uint(float(diffuseSamples) * (proposal == 0u ? 0.0 : proposal == 1u ? share.x : share.x + share.y));
            uint count = uint(float(diffuseSamples) * share[proposal]);
            // Reserve the maximum sequence stride even when using fewer
            // rays, so budget changes cannot repeat an earlier frame's samples.
            uint stride = adaptive ? (proposal == 2u ? 1u : 5u) : count;
            u           = HybridSkySample(uvec2(p), sequence * stride + i - first, seed + 0x1000u + proposal);
        }
        if (diffuseSamples % 4u != 0u && diffuseSamples != 6u)
        {
            u.x = min(proposal == 0u   ? u.x / share.x
                      : proposal == 1u ? (u.x - share.x) / share.y
                                       : (u.x - share.x - share.y) / share.z,
                      0.99999994);
        }
        vec3  w;
        float pdf;
        if (proposal == 0u)
        {
            w = GuideSample(n, guideTotal, guideRows, u, pdf);
        }
        else if (proposal == 1u)
        {
            w = SampleEnvironmentDistribution(u, true, pdf);
        }
        else
        {
            w = HybridCosineSample(n, u, pdf);
        }
        // Mixture density of the direction under all three proposals.
        uint  face;
        vec2  uv, st;
        uvec2 cell;
        EnvironmentLocate(w, face, uv, cell, st);
        uint  size   = ZEN_ENVIRONMENT_IMPORTANCE_SIZE / ZEN_HYBRID_GUIDE_PER_FACE;
        uint  tile   = (face * ZEN_HYBRID_GUIDE_PER_FACE + cell.y / size) * ZEN_HYBRID_GUIDE_PER_FACE + cell.x / size;
        bool  source = EnvironmentSourceTile(EnvironmentTileOf(face, cell));
        float guided = 0.0, residual = 0.0;
        if (share.x > 0.0)
        {
            float tilePDF = proposal == 0u
                              ? pdf
                              : EnvironmentCellInTilePDF(tile, face * ZEN_ENVIRONMENT_IMPORTANCE_SIZE + cell.y, cell.x, uv, st,
                                                         ZEN_HYBRID_GUIDE_CELL_SIZE, ZEN_HYBRID_GUIDE_CDF_OFFSET);
            guided        = GuideCosineWeight(tile, n) / guideTotal * tilePDF;
        }
        if (share.y > 0.0 && !source && residualEnvironment)
        {
            residual = proposal == 1u ? pdf : EnvironmentDistributionPDF(w, true);
        }
        pdf                          = share.x * guided + share.y * residual + share.z * max(dot(n, w), 0.0) / HYBRID_PI;
        bool            inDomain     = pdf > 0.0 && dot(n, w) > 0.0 && dot(ng, w) > 0.0;
        bool            sharedBounce = FeatureHardware(SKY_PROVIDER) == FeatureHardware(BOUNCE_PROVIDER);
        HybridRayResult hit          = HybridRayResult(false, position, vec3(0), ivec3(-1));
        if (inDomain)
        {
            hit = TraceFeature(SKY_PROVIDER, position, ng, w, hybrid.bounce.x != 0u && sharedBounce);
        }
        bool escaped = inDomain && !hit.hit;
        if (inDomain && hybrid.bounce.x != 0u && !sharedBounce)
        {
            hit = TraceFeature(BOUNCE_PROVIDER, position, ng, w, true);
        }
        bool queued = false;
#if defined(HYBRID_INLINE_RETRACE)
        // Reference accumulation re-traces a compute bounce ray whose brick hit lies next to a lamp
        // here, as hybrid_retrace_bricks.comp does for queued rays. When the sky ray is the bounce
        // ray, an escape adds sky and teaches the guide like any other.
        if (inDomain && hit.hit && hybrid.bounce.x != 0u && !FeatureHardware(BOUNCE_PROVIDER)
            && HitNearLocalLight(hit.position))
        {
#    ifdef HYBRID_COUNT_RAYS
            issuedRayFeature = 2u;
#    endif
            hit         = BrickRetraceTriangles(hit.position - w * brick.voxel.x, w);
            retracedHit = true;
            if (sharedBounce)
            {
                escaped = !hit.hit;
            }
        }
#elif defined(HYBRID_BRICK_TRAVERSAL)
        // A compute bounce ray whose brick hit lies next to a lamp: hybrid_retrace_bricks.comp adds
        // its bounce, and its sky when the sky ray is the bounce ray. Escapes found by the re-trace
        // do not teach the guide.
        if (inDomain && hit.hit && hybrid.bounce.x != 0u && !FeatureHardware(BOUNCE_PROVIDER)
            && HitNearLocalLight(hit.position))
        {
            queued = QueueRetrace(hit.position, w, max(dot(n, w), 0.0) / (HYBRID_PI * pdf * float(diffuseSamples)), source);
        }
#endif
        if (inDomain && hit.hit && hybrid.bounce.x != 0u && !queued)
        {
            bool  rejected;
            float weight  = max(dot(n, w), 0.0) / (HYBRID_PI * pdf);
            vec3  value   = HitRadiance(hitRadiance, hitNormal, hit, w, false, rejected) * weight;
            bounce.rgb   += value;
            bounce.a     += rejected ? 1.0 : 0.0;
            // The same hit in the previous cache: these correlated lookups measure the change
            // with little noise when lighting changes a little.
            if (hybrid.bounce.w != 0u)
            {
                previous.rgb += HitRadiance(hitRadiancePrevious, hitNormal, hit, w, true, rejected) * weight;
            }
        }
#ifdef HYBRID_INLINE_RETRACE
        retracedHit = false;
#endif
        if (escaped)
        {
            float weight = max(dot(n, w), 0.0) / (HYBRID_PI * pdf);
            if (!source)
            {
                vec3 radiance  = textureLod(coneEnvironmentMap, EnvironmentDirection(w), 0).rgb;
                sky.rgb       += radiance * weight * scale;
                // The guide learns visible radiance without this receiver's cosine.
                if (learning > 0.0)
                {
                    GuideLearn(tile, learning * HybridLuminance(radiance) / (HYBRID_PI * pdf * float(diffuseSamples)));
                }
            }
            // Alpha estimates the cosine-weighted visible fraction over all directions.
            sky.a += weight;
        }
    }
    sky          /= float(diffuseSamples);
    bounce.rgb   /= float(diffuseSamples);
    previous.rgb /= float(diffuseSamples);
    // Each source tile gets its own rays, half the residual count (two at the shipping preset),
    // so sources are integrated without selection noise between them. Their directions are
    // coherent across pixels, so they cost far less than residual rays (P4Execution.md).
    uint sourceRays = max(hybrid.sampling.y / 2u, 1u);
    for (uint k = 0u; k < environmentSourceCount; ++k)
    {
        for (uint j = 0u; j < sourceRays; ++j)
        {
            float pdf;
            vec2  u = HybridSkySample(uvec2(p), sequence * sourceRays + j, seed + 0x8000u + k * 0x1000u);
            vec3  w = SampleEnvironmentTile(environmentSourceTiles[k], u, pdf);
            if (pdf > 0.0 && dot(n, w) > 0.0 && dot(ng, w) > 0.0 && !TraceFeature(SKY_PROVIDER, position, ng, w, false).hit)
            {
                vec3 radiance  = textureLod(coneEnvironmentMap, EnvironmentDirection(w), 0).rgb;
                sky.rgb       += radiance * max(dot(n, w), 0.0) / (HYBRID_PI * pdf * float(sourceRays)) * scale;
                uint  face;
                vec2  uv, st;
                uvec2 cell;
                EnvironmentLocate(w, face, uv, cell, st);
                uint tile = (face * ZEN_HYBRID_GUIDE_PER_FACE + cell.y / ZEN_HYBRID_GUIDE_CELL_SIZE) * ZEN_HYBRID_GUIDE_PER_FACE
                          + cell.x / ZEN_HYBRID_GUIDE_CELL_SIZE;
                if (learning > 0.0)
                {
                    GuideLearn(tile, learning * HybridLuminance(radiance) / (HYBRID_PI * pdf * float(sourceRays)));
                }
            }
        }
    }
    return DiffuseResult(sky, bounce, previous, reconstructionGuide, reducedBudget);
}

#ifndef HYBRID_TRACE_FALLBACK
// Specular occlusion and radiance-cache reflection of one receiver, always at full resolution.
void TraceSpecular(ivec2      p,
                   vec3       position,
                   vec3       n,
                   vec3       ng,
                   inout vec4 specular,
                   inout vec2 reflection,
                   inout vec3 reflectionPrevious)
{
    if (hybrid.rejection.y > 0.0)
    {
        vec3 view =
            sceneUbo.viewPosition.w < 0.5 ? normalize(sceneUbo.lightInfo.yzw) : normalize(sceneUbo.viewPosition.xyz - position);
        float roughness       = clamp(texelFetch(receiverRoughness, p, 0).g, 0.04, 1.0);
        uint  specularSamples = hybrid.sampling.y > 4u ? hybrid.sampling.y : 1u;
        specular.a            = 0.0;
        float hitCount        = 0.0;
        for (uint i = 0u; i < specularSamples; ++i)
        {
            vec3 w = HybridSpecularDirection(n, view, roughness, uvec2(p), hybrid.sampling.x * specularSamples + i, 0x1234u);
            if (dot(ng, w) > 0.0)
            {
                bool            sharedReflection = FeatureHardware(SPECULAR_PROVIDER) == FeatureHardware(REFLECTION_PROVIDER);
                HybridRayResult hit =
                    TraceFeature(SPECULAR_PROVIDER, position, ng, w, hybrid.reflection.x != 0u && sharedReflection);
                if (!hit.hit)
                {
                    specular.a += 1.0;
                }
                if (hybrid.reflection.x != 0u && !sharedReflection)
                {
                    hit = TraceFeature(REFLECTION_PROVIDER, position, ng, w, true);
                }
                if (hit.hit && hybrid.reflection.x != 0u)
                {
                    bool rejected;
                    // Store the unconditional hit integral C=(1-S)*H. Averaging conditional H
                    // separately from the hit fraction would multiply two noisy estimates.
                    specular.rgb += HitRadiance(hitRadiance, hitNormal, hit, w, false, rejected);
                    reflection.y += rejected ? 1.0 : 0.0;
                    reflection.x += length(hit.position - position);
                    hitCount     += 1.0;
                    if (hybrid.reflection.w != 0u)
                    {
                        reflectionPrevious += HitRadiance(hitRadiancePrevious, hitNormal, hit, w, true, rejected);
                    }
                }
            }
        }
        specular           /= float(specularSamples);
        reflectionPrevious /= float(specularSamples);
        reflection.x       /= max(hitCount, 1.0);
    }
}

void StoreSpecular(ivec2 p, vec4 specular, vec2 reflection, vec3 reflectionPrevious)
{
    imageStore(rawSpecular, p, specular);
    if (hybrid.reflection.x != 0u)
    {
        imageStore(rawReflection, p, vec4(reflection, 0, 0));
        imageStore(rawReflectionPrevious, p, vec4(reflectionPrevious, 0));
    }
}
#endif

#ifndef HYBRID_TRACE_HALF
void StoreDiffuse(ivec2 p, DiffuseResult d)
{
    imageStore(rawSky, p, d.sky);
    imageStore(skyGuide, p, vec4(d.guide, 0.0));
    if (hybrid.bounce.x != 0u) // 1x1 placeholders for cone bounce
    {
        imageStore(rawBounce, p, d.bounce);
        imageStore(rawBouncePrevious, p, d.previous);
    }
    if (hybrid.provider.w != 0u) // 1x1 placeholder without adaptive sampling
    {
        imageStore(nextBudget, p, uvec4(d.reducedBudget));
    }
}
#endif

#ifdef HYBRID_COUNT_RAYS
void StoreRayCounts(uint lane, uint slot)
{
    computeRaySums[lane]  = issuedComputeRays;
    hardwareRaySums[lane] = issuedHardwareRays;
    barrier();
    for (uint stride = TRACE_THREADS / 2u; stride > 0u; stride /= 2u)
    {
        if (lane < stride)
        {
            computeRaySums[lane]  += computeRaySums[lane + stride];
            hardwareRaySums[lane] += hardwareRaySums[lane + stride];
        }
        barrier();
    }
    if (lane == 0u)
    {
        rayCounts[slot]      = computeRaySums[0];
        rayCounts[slot + 1u] = hardwareRaySums[0];
    }
}
#endif

#ifndef HYBRID_TRACE_FALLBACK
void FinishGuide(uint lane)
{
    barrier();
    // Average this frame into the guide: equally over the first frames, then exponentially.
    float frames = guideValid ? guidePrevious[guideBase + ZEN_HYBRID_GUIDE_TILES] : 0.0;
    float alpha  = 1.0 / min(frames + 1.0, HYBRID_GUIDE_FRAMES);
    for (uint t = lane; t < ZEN_HYBRID_GUIDE_TILES; t += TRACE_THREADS)
    {
        float sum                = HYBRID_EXACT_GUIDE ? GuideSumValue(t) : uintBitsToFloat(guideSumWords[t]);
        float learned            = guideReceivers > 0u ? sum / float(guideReceivers) : 0.0;
        float previous           = guideValid ? guidePrevious[guideBase + t] : 0.0;
        guideNext[guideBase + t] = mix(previous, learned, alpha);
    }
    if (lane == 0u)
    {
        guideNext[guideBase + ZEN_HYBRID_GUIDE_TILES] = min(frames + 1.0, HYBRID_GUIDE_FRAMES);
    }
#    ifdef HYBRID_COUNT_RAYS
    uint tile = gl_WorkGroupID.y * gl_NumWorkGroups.x + gl_WorkGroupID.x;
    // The fallback pass adds to these entries (half resolution) or none runs (full resolution).
    if (lane == 0u)
    {
        uint fallbackSlot            = (gl_NumWorkGroups.x * gl_NumWorkGroups.y + tile) * 2u;
        rayCounts[fallbackSlot]      = uvec4(0);
        rayCounts[fallbackSlot + 1u] = uvec4(0);
    }
    StoreRayCounts(lane, tile * 2u);
#    endif
}
#endif

#if defined(HYBRID_TRACE_HALF)
// Leader order inside a 2x2 block: diagonal first, so consecutive frames alternate rows and columns.
const ivec2 HALF_PHASES[4] = ivec2[4](ivec2(0, 0), ivec2(1, 1), ivec2(1, 0), ivec2(0, 1));
// Receivers at least this rough share their block leader's specular rays when their roughness is
// within 15%: textured roughness varies per pixel, but lobes this broad are alike. The spatial
// filter already spreads them over up to roughness^2*128 pixels (hybrid_filter.glsl).
const float HALF_SPECULAR_ROUGHNESS = 0.3;
// Specular rays of receivers that cannot share their leader's, distributed over the workgroup
// so no thread traces a whole block's rays alone (tile-local pixel indices).
shared uint specularJobs[192];
shared uint specularJobCount;

void main()
{
    uint  lane  = gl_LocalInvocationIndex;
    ivec2 block = ivec2(gl_GlobalInvocationID.xy), base = block * 2;
    if (lane == 0u)
    {
        specularJobCount = 0u;
    }
    if (lane == 0u && all(equal(gl_WorkGroupID.xy, uvec2(0))))
    {
        fallbackCount   = 0u;
        fallbackGroupsX = 0u;
        fallbackGroupsY = 1u;
        fallbackGroupsZ = 1u;
    }
    BuildGuide(lane);
    // Each receiver leads its block once every four frames. Its own sample index then advances
    // by one per lead, so it draws a complete stratified sequence. A substitute leader (when the
    // phase pixel is not a receiver) uses this phase's seed, so it cannot repeat its own samples.
    uint phase = hybrid.sampling.x % 4u;
    // The leader's rays stand for every receiver of its block in the guide, as at full resolution.
    int  leader    = -1;
    uint receivers = 0u;
    for (uint i = 0u; i < 4u; ++i)
    {
        uint candidate = (phase + i) % 4u;
        if (IsReceiver(base + HALF_PHASES[candidate]))
        {
            if (leader < 0)
            {
                leader = int(candidate);
            }
            ++receivers;
        }
    }
    if (receivers > 0u)
    {
        atomicAdd(guideReceivers, receivers);
    }
    DiffuseResult d = DiffuseResult(vec4(0), vec4(0), vec4(0), vec3(1), 0u);
    if (leader >= 0)
    {
        ivec2 q = base + HALF_PHASES[leader];
        d       = TraceDiffuse(q, HybridPosition(q), HybridNormal(q), HybridGeometricNormal(q), hybrid.sampling.x / 4u,
                               0x10000u * (1u + phase), float(receivers));
    }
#    if defined(HYBRID_BRICK_TRAVERSAL) && !defined(HYBRID_INLINE_RETRACE)
    PublishRetrace(block);
#    endif
    if (all(lessThan(block, imageSize(halfGuide))))
    {
        imageStore(halfSky, block, d.sky);
        imageStore(halfGuide, block, vec4(d.guide, leader >= 0 ? float(leader) : -1.0));
        if (hybrid.bounce.x != 0u)
        {
            imageStore(halfBounce, block, d.bounce);
            imageStore(halfBouncePrevious, block, d.previous);
        }
    }
    // Rough lobes vary slowly, and the spatial filter spreads them over many pixels: compatible
    // rough receivers share the leader's specular rays. Glossy and dissimilar receivers, whose
    // reflections and occlusion need their own lobes, trace at full resolution.
    vec4  leaderSpecular           = vec4(0, 0, 0, 1);
    vec2  leaderReflection         = vec2(0);
    vec3  leaderReflectionPrevious = vec3(0);
    ivec2 leaderPixel              = base + HALF_PHASES[max(leader, 0)];
    vec3  leaderPosition = vec3(0), leaderNormal = vec3(0), leaderGeometric = vec3(0);
    float leaderRoughness = -1.0;
    uint  leaderIdentity  = 0u;
    if (leader >= 0)
    {
        leaderPosition  = HybridPosition(leaderPixel);
        leaderNormal    = HybridNormal(leaderPixel);
        leaderGeometric = HybridGeometricNormal(leaderPixel);
        leaderRoughness = texelFetch(receiverRoughness, leaderPixel, 0).g;
        leaderIdentity  = texelFetch(receiverSurface, leaderPixel, 0).g;
        TraceSpecular(leaderPixel, leaderPosition, leaderNormal, leaderGeometric, leaderSpecular, leaderReflection,
                      leaderReflectionPrevious);
    }
    for (uint i = 0u; i < 4u; ++i)
    {
        ivec2 p                  = base + ivec2(i & 1u, i >> 1u);
        vec4  specular           = vec4(0, 0, 0, 1);
        vec2  reflection         = vec2(0);
        vec3  reflectionPrevious = vec3(0);
        // The block shares its leader's budget; fallback receivers overwrite their own.
        if (HybridInView(p) && hybrid.provider.w != 0u)
        {
            imageStore(nextBudget, p, uvec4(d.reducedBudget));
        }
        if (HybridInView(p))
        {
            vec3 guide = vec3(1);
            if (leader >= 0 && all(equal(p, leaderPixel)))
            {
                guide = d.guide;
            }
            else if (IsReceiver(p))
            {
                float fullGuideTotal;
                float guideRows[GUIDE_ATLAS_ROWS];
                vec3  n = HybridNormal(p);
                GuideTotal(n, fullGuideTotal, guideRows);
                guide = ReconstructionGuide(n, fullGuideTotal);
            }
            imageStore(skyGuide, p, vec4(guide, 0.0));
        }
        if (leader >= 0 && all(equal(p, leaderPixel)))
        {
            specular           = leaderSpecular;
            reflection         = leaderReflection;
            reflectionPrevious = leaderReflectionPrevious;
        }
        else if (IsReceiver(p))
        {
            vec3  position = HybridPosition(p), n = HybridNormal(p), ng = HybridGeometricNormal(p);
            float roughness   = texelFetch(receiverRoughness, p, 0).g;
            vec3  delta       = leaderPosition - position;
            bool  shareLeader = leader >= 0 && roughness >= HALF_SPECULAR_ROUGHNESS
                             && abs(roughness - leaderRoughness) <= max(0.02, 0.15 * max(roughness, leaderRoughness))
                             && texelFetch(receiverSurface, p, 0).g == leaderIdentity && dot(ng, leaderGeometric) >= 0.99
                             && dot(n, leaderNormal) >= 0.9
                             && abs(dot(ng, delta)) <= hybrid.rejection.x * max(1.0, length(delta));
            if (shareLeader)
            {
                specular           = leaderSpecular;
                reflection         = leaderReflection;
                reflectionPrevious = leaderReflectionPrevious;
            }
            else
            {
                ivec2 local                                   = p - ivec2(gl_WorkGroupID.xy) * 16;
                specularJobs[atomicAdd(specularJobCount, 1u)] = uint(local.y) * 16u + uint(local.x);
                continue;
            }
        }
        if (HybridInView(p))
        {
            StoreSpecular(p, specular, reflection, reflectionPrevious);
        }
    }
    barrier();
    for (uint job = lane; job < specularJobCount; job += TRACE_THREADS)
    {
        ivec2 p                  = ivec2(gl_WorkGroupID.xy) * 16 + ivec2(specularJobs[job] % 16u, specularJobs[job] / 16u);
        vec4  specular           = vec4(0, 0, 0, 1);
        vec2  reflection         = vec2(0);
        vec3  reflectionPrevious = vec3(0);
        TraceSpecular(p, HybridPosition(p), HybridNormal(p), HybridGeometricNormal(p), specular, reflection,
                      reflectionPrevious);
        StoreSpecular(p, specular, reflection, reflectionPrevious);
    }
    FinishGuide(lane);
}
#elif defined(HYBRID_TRACE_FALLBACK)
void main()
{
    uint index = gl_GlobalInvocationID.x;
    if (index >= fallbackCount)
    {
        return;
    }
    uint  width = uint(textureSize(receiverDepth, 0).x), pixel = fallbackPixels[index];
    ivec2 p = ivec2(pixel % width, pixel / width);
    // The same pooled guide as this tile's half-resolution pass; fallback rays do not learn.
    pooledBase = (uint(p.y / 16) * ((width + 15u) / 16u) + uint(p.x / 16)) * POOLED_GUIDE_STRIDE;
    guideValid = hybrid.provider.y != 0u;
    StoreDiffuse(p, TraceDiffuse(p, HybridPosition(p), HybridNormal(p), HybridGeometricNormal(p), hybrid.sampling.x, 0u, 0.0));
#    if defined(HYBRID_BRICK_TRAVERSAL) && !defined(HYBRID_INLINE_RETRACE)
    PublishRetrace(p);
#    endif
#    ifdef HYBRID_COUNT_RAYS
    // Every fallback ray adds to the first tile's fallback entries, which the half pass cleared.
    uint slot = (((uint(textureSize(receiverDepth, 0).y) + 15u) / 16u) * ((width + 15u) / 16u)) * 2u;
    for (uint c = 0u; c < 4u; ++c)
    {
        if (issuedComputeRays[c] != 0u)
        {
            atomicAdd(rayCounts[slot][c], issuedComputeRays[c]);
        }
        if (issuedHardwareRays[c] != 0u)
        {
            atomicAdd(rayCounts[slot + 1u][c], issuedHardwareRays[c]);
        }
    }
#    endif
}
#else
void main()
{
    ivec2 p    = ivec2(gl_GlobalInvocationID.xy);
    uint  lane = gl_LocalInvocationIndex;
    BuildGuide(lane);
    DiffuseResult d                  = DiffuseResult(vec4(0), vec4(0), vec4(0), vec3(1), 0u);
    vec4          specular           = vec4(0, 0, 0, 1);
    vec2          reflection         = vec2(0);
    vec3          reflectionPrevious = vec3(0);
    if (IsReceiver(p))
    {
        atomicAdd(guideReceivers, 1u);
        vec3 position = HybridPosition(p), n = HybridNormal(p), ng = HybridGeometricNormal(p);
        d = TraceDiffuse(p, position, n, ng, hybrid.sampling.x, 0u, 1.0);
        TraceSpecular(p, position, n, ng, specular, reflection, reflectionPrevious);
    }
    if (HybridInView(p))
    {
        StoreDiffuse(p, d);
        StoreSpecular(p, specular, reflection, reflectionPrevious);
    }
#    if defined(HYBRID_BRICK_TRAVERSAL) && !defined(HYBRID_INLINE_RETRACE)
    PublishRetrace(p);
#    endif
    FinishGuide(lane);
}
#endif
