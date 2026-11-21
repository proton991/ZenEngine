#include "hybrid_guides.glsl"
#include "hybrid_sampling.glsl"
#ifdef HYBRID_YOUNG_RECEIVERS
#    define DENOISE_GUIDE_ONLY
#    include "gi_denoise_common.glsl"
#endif
#include "hybrid_provider.glsl"
#if defined(HYBRID_RESTIR)
#    include "restir_target.glsl"
layout(std430, set = 1, binding = 38) writeonly buffer GIReservoirCurrent
{
    PackedGIReservoir currentReservoirs[];
};
layout(std430, set = 1, binding = 44) writeonly buffer ReflectionReservoirCurrent
{
    PackedGIReservoir currentReflections[];
};
GIReservoir initialGI;
GIReservoir initialReflection;
// Receivers at least this rough estimate their reflection from their diffuse rays instead of
// tracing a reflection ray: each diffuse hit is weighted by the normalized GGX lobe over its
// proposal density, for radiance, specular occlusion and the reflection reservoir alike
// (HybridTraceProfile.md, stage 4).
const float HYBRID_DIFFUSE_REFLECTION_ROUGHNESS = 0.5;
bool        reflectFromDiffuse                  = false;
vec3        diffuseReflectionView               = vec3(0, 0, 1);
float       diffuseReflectionRoughness          = 1.0;
vec4        diffuseReflectionSum                = vec4(0); // Lobe-weighted hit radiance, lobe-weighted escapes.
vec2        diffuseReflectionHits               = vec2(0); // Lobe-weighted hit distance and rejected hits.
float       diffuseReflectionWeight             = 0.0;     // Sum of the hits' lobe weights.
vec2        diffuseReflectionCompleted          = vec2(0);
#endif
#if !defined(HYBRID_FIELD_WORLD_CACHE) && (!defined(HYBRID_HARDWARE_PROVIDER) || defined(HYBRID_FIELD_TRAVERSAL))
#    define HIT_LIGHT_CACHE
#    include "hit_radiance.glsl"
#endif
#include "environment_sampling.glsl"
#include "environment_distribution.glsl"
#if defined(HYBRID_RAY_PIPELINE)
#elif defined(HYBRID_NO_GUIDE)
layout(local_size_x = 8, local_size_y = 8) in;
const uint TRACE_THREADS = 64u;
#else
// One workgroup per screen tile of the visibility guide (HybridGIRenderer::kGuideTile).
layout(local_size_x = 16, local_size_y = 16) in;
const uint TRACE_THREADS = 256u;
#endif
layout(set = 1, binding = 54, r32f) uniform writeonly image2D rawDiffuseDistance;
#ifndef HYBRID_FIELD_WORLD_CACHE
layout(set = 1, binding = 6) uniform sampler3D voxelOpacity;
#endif
layout(set = 1, binding = 7) uniform samplerCube coneEnvironmentMap;
layout(set = 1, binding = 8, rgba16f) uniform writeonly image2D rawSky;
layout(set = 1, binding = 12, rgba16f) uniform writeonly image2D rawBounce;
layout(set = 1, binding = 16, rgba16f) uniform writeonly image2D rawBouncePrevious;
layout(set = 1, binding = 9, rgba16f) uniform writeonly image2D rawSpecular;
layout(set = 1, binding = 17, rg32f) uniform writeonly image2D rawReflection;
layout(set = 1, binding = 18, rgba16f) uniform writeonly image2D rawReflectionPrevious;
#ifdef HIT_LIGHT_CACHE
layout(set = 1, binding = 13) uniform sampler3D hitRadiance;
layout(set = 1, binding = 14) uniform sampler3D hitNormal;
// Base radiance level from before this frame's injection, and the bounce this frame's rays read from
// it (meaningful only when the cache changed; hybrid_temporal.comp).
layout(set = 1, binding = 15) uniform sampler3D hitRadiancePrevious;
#endif
#ifdef HYBRID_FIELD_WORLD_CACHE
#    define NATIVE_FIELD_ENDPOINT
#    include "native_field.glsl"
#endif
#include "ray_hit_lighting.glsl"
layout(std140, set = 1, binding = 28) uniform uHybridReuseData
{
    uvec4 reuseData;
};

#ifdef HYBRID_YOUNG_RECEIVERS
layout(set = 1, binding = 60) uniform sampler2D previousLength;
layout(set = 1, binding = 61) uniform sampler2D previousPosition;
layout(set = 1, binding = 62) uniform sampler2D previousNormal;
layout(set = 1, binding = 63) uniform usampler2D previousReceiver;
layout(set = 1, binding = 64) uniform sampler2D previousReflection;
#    include "gi_previous_age.glsl"
#endif

#include "hybrid_guide_storage.glsl"
#if defined(HYBRID_COUNT_RAYS)
// One tile stores four feature counters for each measured category.
layout(std430, set = 1, binding = 26) buffer HybridRayCounts
{
    uvec4 rayCounts[];
};
#    ifndef HYBRID_RAY_PIPELINE
shared uvec4 computeRaySums[TRACE_THREADS], hardwareRaySums[TRACE_THREADS];
shared uvec4 fieldWorkSums[TRACE_THREADS];
shared uvec4 candidateSums[TRACE_THREADS];
#    endif
#endif

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
#if defined(HYBRID_COUNT_RAYS)
    issuedRayFeature = feature == SKY_PROVIDER ? 0u : feature == SPECULAR_PROVIDER ? 1u : feature == BOUNCE_PROVIDER ? 2u : 3u;
#endif
    // A HW-only variant has no Compute provider: a feature it does not trace has no result.
    HybridRayResult result = MakeHybridRayResult(false, position, vec3(0), ivec3(-1), false);
#if defined(HYBRID_HARDWARE_PROVIDER)
    if (FeatureHardware(feature))
    {
        vec3 origin = HybridTraceOrigin(position, normal, 0.0);
        if (closest)
        {
            result = HardwareTraceRay(origin, direction, 1e20, true);
        }
        else
        {
            result.valid = true;
            result.hit   = HardwareOccluded(origin, direction, 1e20);
        }
    }
#endif
#if defined(HYBRID_FIELD_TRAVERSAL)
    if (!FeatureHardware(feature))
    {
#    ifdef HYBRID_FIELD_WORLD_CACHE
        result = NativeTraceRay(position, normal, direction, 1e20, closest);
#    else
        result = FieldTraceRay(FieldTraceOrigin(position, normal), direction, 1e20, closest);
#    endif
    }
#endif
    return result;
}
// Visibility of a nearby point or spot light from a shading hit (hit_radiance.glsl), with the bounce
// provider, over the finite segment to the light. Only a covered miss is visible.
float HitLightVisibility(HybridRayResult hit, SceneLight light, int lightIndex)
{
#if defined(HYBRID_COUNT_RAYS)
    uint previousPurpose = issuedRayPurpose;
    issuedRayPurpose     = 1u;
    issuedRayFeature     = 2u;
#endif
    float visibility = 1.0;
#if defined(HYBRID_HARDWARE_PROVIDER)
    if (FeatureHardware(BOUNCE_PROVIDER))
    {
        vec3  origin          = HybridTraceOrigin(hit.position, hit.normal, 0.0);
        vec3  delta           = light.positionRange.xyz - origin;
        float distanceToLight = length(delta);
        vec3  direction       = delta / max(distanceToLight, 1e-20);
        // Exclude the endpoint itself, without dropping a finite interval before it.
        distanceToLight = uintBitsToFloat(floatBitsToUint(distanceToLight) - uint(distanceToLight > 0.0));
        visibility      = distanceToLight > 0.0 && HardwareOccluded(origin, direction, distanceToLight) ? 0.0 : 1.0;
    }
    else
#endif
    {
#if defined(HYBRID_FIELD_TRAVERSAL)
        visibility = FieldLightVisibility(hit, light.positionRange.xyz);
#endif
    }
#if defined(HYBRID_COUNT_RAYS)
    issuedRayPurpose = previousPurpose;
#endif
    return visibility;
}

void GuideLearn(uint tile, float value)
{
#if defined(HYBRID_NO_GUIDE)
    // This program is selected only when environment lighting contributes nothing.
#else
#    ifdef HYBRID_RAY_PIPELINE
    tile += guideBase;
#    endif
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
#endif
}

#if !defined(HYBRID_NO_GUIDE)
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

#endif

// The guide survives resets that keep the screen and environment (HybridGIRenderer); source
// tiles have their own rays.

#include "hybrid_guide_build.glsl"

bool IsReceiver(ivec2 p)
{
    return HybridInView(p) && texelFetch(receiverDepth, p, 0).r < 1.0 && texelFetch(receiverSurface, p, 0).g != 0u;
}

struct DiffuseResult
{
    float distance;
    vec4  sky;
    vec4  bounce;
    vec4  previous;
};

void AccumulateReflection(HybridRayResult hit,
                          vec3            direction,
                          vec3            position,
                          vec3            radiance,
                          uint            hitSeed,
                          float           roughness,
                          bool            rejected,
                          inout vec4      specular,
                          inout vec2      reflection,
                          inout vec3      reflectionPrevious,
                          inout float     hitCount)
{
    specular.rgb += radiance;
#if defined(HYBRID_RESTIR)
    if (restirOptions.y != 0u && roughness >= 0.3)
    {
        GIReservoir candidate = GIReservoir(hit.position, hit.normal, radiance, 0.0, 1u);
        float       random    = float(HybridHash(hitSeed ^ 0x87f19ab3u) >> 8u) / 16777216.0;
        // Target/pdf cancels the normalized lobe used by HybridSpecularDirection.
        GIStream(initialReflection, candidate, GITarget(radiance), 0u, random);
    }
#endif
    reflection.y += rejected ? 1.0 : 0.0;
    reflection.x += length(hit.position - position);
    hitCount     += 1.0;
#ifdef HIT_LIGHT_CACHE
    if (hybrid.reflection.w != 0u)
    {
        reflectionPrevious += HitRadiance(hitRadiancePrevious, hitNormal, hit, direction, true, rejected);
    }
#endif
}

// Sky, source-shadow and ray-hit bounce estimate of one receiver. Its rays learn the guide
// with the given weight: the number of receivers the result represents, or zero.
DiffuseResult TraceDiffuse(ivec2 p, vec3 position, vec3 n, vec3 ng, uint sequence, uint seed, float learning)
{
    vec4  sky = vec4(0), bounce = vec4(0), previous = vec4(0);
    uvec2 unavailable = uvec2(0); // Sky and bounce traces that could not complete.
    float scale       = sceneUbo.environment.x * sceneUbo.environment.z * gi.lighting.y;
    float fullGuideTotal;
#if !defined(HYBRID_NO_GUIDE)
    float guideRows[GUIDE_ATLAS_ROWS];
#endif
#if defined(HYBRID_NO_GUIDE)
    fullGuideTotal   = 0.0;
    float guideTotal = 0.0;
#else
    float guideTotal = 0.0;
    fullGuideTotal   = 0.0;
    {
        guideTotal = GuideTotal(n, fullGuideTotal, guideRows);
    }
#endif
    // Shares of guided, residual-environment and cosine rays; without a guide its share goes to
    // the environment. Every density below is the mixture of these three proposals.
#if defined(HYBRID_NO_GUIDE)
    vec3       share               = vec3(0, 0, 1);
    const bool residualEnvironment = false;
#else
    vec3 share = guideTotal > 0.0 ? vec3(0.75, 0.0, 0.25) : vec3(0.0, HYBRID_ENVIRONMENT_SHARE, 1.0 - HYBRID_ENVIRONMENT_SHARE);
    bool residualEnvironment = environmentResidualRows[ZEN_ENVIRONMENT_IMPORTANCE_ROWS] > 0.0
                            && gi.lighting.y * sceneUbo.environment.x * sceneUbo.environment.z > 0.0;
    if (!residualEnvironment)
    {
        share = vec3(0, 0, 1);
    }
#endif
    float diffuseDistance = 0.0, diffuseHitCount = 0.0;
    uint  diffuseSamples = hybrid.sampling.y;
#ifdef HYBRID_YOUNG_RECEIVERS
    if (reuseData.w != 0u && PreviousAge(p, DenoiseGuide(p)).x < float(reuseData.w))
    {
        diffuseSamples *= 2u;
    }
#endif
#if defined(HYBRID_RESTIR)
    initialReflection = EmptyGIReservoir();
    diffuseReflectionView =
        sceneUbo.viewPosition.w < 0.5 ? normalize(sceneUbo.lightInfo.yzw) : normalize(sceneUbo.viewPosition.xyz - position);
    diffuseReflectionRoughness = clamp(texelFetch(receiverRoughness, p, 0).g, 0.04, 1.0);
    // The diffuse rays must trace with the providers the reflection would use.
    reflectFromDiffuse         = hybrid.rejection.y > 0.0 && hybrid.reflection.x != 0u && hybrid.bounce.x != 0u
                              && hybrid.sampling.y <= 4u && diffuseReflectionRoughness >= HYBRID_DIFFUSE_REFLECTION_ROUGHNESS
                              && FeatureHardware(SPECULAR_PROVIDER) == FeatureHardware(SKY_PROVIDER)
                              && FeatureHardware(REFLECTION_PROVIDER) == FeatureHardware(BOUNCE_PROVIDER);
    diffuseReflectionSum       = vec4(0);
    diffuseReflectionHits      = vec2(0);
    diffuseReflectionWeight    = 0.0;
    diffuseReflectionCompleted = vec2(0);
#endif
    uint totalSamples = diffuseSamples;
    for (uint slot = 0u; slot < totalSamples; ++slot)
    {
        bool  specularSlot = slot >= diffuseSamples;
        uint  i            = specularSlot ? slot - diffuseSamples : slot;
        vec3  w;
        float pdf    = 1.0;
        bool  source = false;
        uint  tile   = 0u;
        {
            // Rays from the same proposal share one temporal Sobol net. The pixel's digital shift
            // preserves spatial stratification while each proposal advances by its own ray count.
            vec2 u = HybridSkySample(uvec2(p), sequence, seed + 0x1000u + i);
            // Four-ray budgets split 3:1; other counts stratify the proposal choice per ray.
            float choice   = (diffuseSamples % 4u == 0u) ? (float(i) + 0.5) / float(diffuseSamples) : u.x;
            uint  proposal = choice < share.x ? 0u : choice < share.x + share.y ? 1u : 2u;
            if (diffuseSamples % 4u == 0u)
            {
                uint first = uint(float(diffuseSamples)
                                  * (proposal == 0u   ? 0.0
                                     : proposal == 1u ? share.x
                                                      : share.x + share.y));
                uint count = uint(float(diffuseSamples) * share[proposal]);
                u          = HybridSkySample(uvec2(p), sequence * count + i - first, seed + 0x1000u + proposal);
            }
            if (diffuseSamples % 4u != 0u)
            {
                u.x = min(proposal == 0u   ? u.x / share.x
                          : proposal == 1u ? (u.x - share.x) / share.y
                                           : (u.x - share.x - share.y) / share.z,
                          0.99999994);
            }
#if !defined(HYBRID_NO_GUIDE)
            if (proposal == 0u)
            {
                w = GuideSample(n, guideTotal, guideRows, u, pdf);
            }
            else
#endif
                if (proposal == 1u)
            {
                w = SampleEnvironmentDistribution(u, true, pdf);
            }
            else
            {
                w = HybridCosineSample(n, u, pdf);
            }
#if !defined(HYBRID_NO_GUIDE)
            // Mixture density of the direction under all three proposals.
            uint  face;
            vec2  uv, st;
            uvec2 cell;
            EnvironmentLocate(w, face, uv, cell, st);
            uint size    = ZEN_ENVIRONMENT_IMPORTANCE_SIZE / ZEN_HYBRID_GUIDE_PER_FACE;
            tile         = (face * ZEN_HYBRID_GUIDE_PER_FACE + cell.y / size) * ZEN_HYBRID_GUIDE_PER_FACE + cell.x / size;
            source       = EnvironmentSourceTile(EnvironmentTileOf(face, cell));
            float guided = 0.0, residual = 0.0;
#    if !defined(HYBRID_NO_GUIDE)
            if (share.x > 0.0)
            {
                float tilePDF = proposal == 0u
                                  ? pdf
                                  : EnvironmentCellInTilePDF(tile, face * ZEN_ENVIRONMENT_IMPORTANCE_SIZE + cell.y, cell.x, uv,
                                                             st, ZEN_HYBRID_GUIDE_CELL_SIZE, ZEN_HYBRID_GUIDE_CDF_OFFSET);
                guided        = GuideCosineWeight(tile, n) / guideTotal * tilePDF;
            }
#    endif
            if (share.y > 0.0 && !source && residualEnvironment)
            {
                residual = proposal == 1u ? pdf : EnvironmentDistributionPDF(w, true);
            }
#else
            float guided = 0.0, residual = 0.0;
#endif
            pdf = share.x * guided + share.y * residual + share.z * max(dot(n, w), 0.0) / HYBRID_PI;
        }
        bool            inDomain          = (specularSlot || (pdf > 0.0 && dot(n, w) > 0.0)) && dot(ng, w) > 0.0;
        uint            visibilityFeature = specularSlot ? SPECULAR_PROVIDER : SKY_PROVIDER;
        uint            radianceFeature   = specularSlot ? REFLECTION_PROVIDER : BOUNCE_PROVIDER;
        bool            radianceEnabled   = (specularSlot ? hybrid.reflection.x : hybrid.bounce.x) != 0u;
        bool            sharedBounce      = FeatureHardware(visibilityFeature) == FeatureHardware(radianceFeature);
        HybridRayResult hit               = MakeHybridRayResult(false, position, vec3(0), ivec3(-1), true);
        uint hitSeed = HybridHash(HybridMorton(uvec2(p)) ^ HybridHash(sequence * diffuseSamples + i) ^ seed ^ 0xb01ce123u);
        PrepareRayHitLighting(hitSeed, true, specularSlot ? 3u : 2u);
#ifdef HYBRID_RADIANCE_CACHE
        float cacheRoughness = clamp(texelFetch(receiverRoughness, p, 0).g, 0.04, 1.0);
        rayCacheCone         = specularSlot ? cacheRoughness * cacheRoughness : -1.0;
#    ifdef HYBRID_RESTIR
        // A diffuse ray may also estimate the rough reflection lobe. Both consumers
        // must satisfy the footprint rule before its secondary shading can be cached.
        if (reflectFromDiffuse)
        {
            rayCacheCone = diffuseReflectionRoughness * diffuseReflectionRoughness;
        }
#    endif
#endif
        if (inDomain)
        {
            hit = TraceFeature(visibilityFeature, position, ng, w, radianceEnabled && sharedBounce);
        }
        // Only a covered miss escapes; an unavailable trace leaves both estimates below.
        bool escaped = inDomain && hit.valid && !hit.hit;
        {
            unavailable.x += hit.valid ? 0u : 1u;
        }
        if (inDomain && radianceEnabled && !sharedBounce)
        {
            hit = TraceFeature(radianceFeature, position, ng, w, true);
        }
        {
            unavailable.y += hit.valid ? 0u : 1u;
        }
        if (inDomain && hit.hit && radianceEnabled)
        {
            bool  rejected;
            float weight = max(dot(n, w), 0.0) / (HYBRID_PI * pdf);
#if defined(HYBRID_COUNT_RAYS)
            issuedRayFeature = specularSlot ? 3u : 2u;
#endif
            if (hybrid.reservoir.w != 0u)
            {
                diffuseDistance += length(hit.position - position);
                diffuseHitCount += 1.0;
            }
            vec3 radiance = TraceHitRadiance(hit, w, hitSeed, rejected);
            {
                vec3 value = radiance * weight;
#if defined(HYBRID_RESTIR)
                GIReservoir candidate = GIReservoir(hit.position, hit.normal, radiance, 0.0, 1u);
                float       random    = float(HybridHash(hitSeed ^ 0x719ad312u) >> 8u) / 16777216.0;
                GIStream(initialGI, candidate, GITarget(value), 0u, random);
#endif
                bounce.rgb += value;
                bounce.a   += rejected ? 1.0 : 0.0;
#if defined(HYBRID_RESTIR)
                if (reflectFromDiffuse)
                {
                    float lobe = GISpecularDensity(n, diffuseReflectionView, w, diffuseReflectionRoughness) / pdf;
                    diffuseReflectionSum.rgb += radiance * lobe;
                    diffuseReflectionHits    += vec2(length(hit.position - position) * lobe, rejected ? lobe : 0.0);
                    diffuseReflectionWeight  += lobe;
                    if (restirOptions.y != 0u)
                    {
                        GIReservoir reflected = GIReservoir(hit.position, hit.normal, radiance, 0.0, 1u);
                        float       random    = float(HybridHash(hitSeed ^ 0x87f19ab3u) >> 8u) / 16777216.0;
                        GIStream(initialReflection, reflected, GITarget(radiance) * lobe, 0u, random);
                    }
                }
#endif
                // The same hit in the previous cache: these correlated lookups measure the change
                // with little noise when lighting changes a little.
#ifdef HIT_LIGHT_CACHE
                if (hybrid.bounce.w != 0u)
                {
                    previous.rgb += HitRadiance(hitRadiancePrevious, hitNormal, hit, w, true, rejected) * weight;
                }
#endif
            }
        }
        if (!specularSlot && escaped)
        {
#if defined(HYBRID_RESTIR)
            diffuseReflectionSum.a +=
                reflectFromDiffuse ? GISpecularDensity(n, diffuseReflectionView, w, diffuseReflectionRoughness) / pdf : 0.0;
#endif
            float weight = max(dot(n, w), 0.0) / (HYBRID_PI * pdf);
#if !defined(HYBRID_NO_GUIDE)
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
#endif
            // Alpha estimates the cosine-weighted visible fraction over all directions.
            sky.a += weight;
        }
        if (slot + 1u == diffuseSamples)
        {
            // Average over completed samples. A receiver whose samples were all unavailable has no
            // estimate this frame. Negative alpha keeps it out of temporal accumulation.
            vec2 completed  = vec2(diffuseSamples) - vec2(unavailable);
            sky            /= max(completed.x, 1.0);
            bounce.rgb     /= max(completed.y, 1.0);
            previous.rgb   /= max(completed.y, 1.0);
            bounce.a        = completed.y > 0.0 ? bounce.a : -1.0;
#if defined(HYBRID_RESTIR)
            // Misses and zero-contribution proposals count; unavailable traces do not.
            initialGI.count            = uint(completed.y);
            diffuseReflectionCompleted = completed;
            GIFinalize(initialGI, GITarget(GIDiffuse(initialGI, position, n, ng)));
#endif
            sky.a = completed.x > 0.0 ? sky.a : -1.0;
            // Each source tile gets its own rays, half the residual count (two at the shipping preset),
            // so sources are integrated without selection noise between them. Their directions are
            // coherent across pixels, so they cost far less than residual rays (P4Execution.md).
#if !defined(HYBRID_NO_GUIDE)
            uint sourceRays = max(hybrid.sampling.y / 2u, 1u);
            for (uint k = 0u; k < environmentSourceCount; ++k)
            {
                vec3 sourceSky   = vec3(0);
                uint sourceValid = sourceRays;
                for (uint j = 0u; j < sourceRays; ++j)
                {
                    float           pdf;
                    vec2            u      = HybridSkySample(uvec2(p), sequence * sourceRays + j, seed + 0x8000u + k * 0x1000u);
                    vec3            w      = SampleEnvironmentTile(environmentSourceTiles[k], u, pdf);
                    HybridRayResult source = MakeHybridRayResult(true, position, vec3(0), ivec3(-1), true);
                    if (pdf > 0.0 && dot(n, w) > 0.0 && dot(ng, w) > 0.0)
                    {
                        source       = TraceFeature(SKY_PROVIDER, position, ng, w, false);
                        sourceValid -= source.valid ? 0u : 1u;
                    }
                    if (source.valid && !source.hit)
                    {
                        vec3 radiance  = textureLod(coneEnvironmentMap, EnvironmentDirection(w), 0).rgb;
                        sourceSky     += radiance * max(dot(n, w), 0.0) / (HYBRID_PI * pdf * float(sourceRays)) * scale;
                        uint  face;
                        vec2  uv, st;
                        uvec2 cell;
                        EnvironmentLocate(w, face, uv, cell, st);
                        uint tile =
                            (face * ZEN_HYBRID_GUIDE_PER_FACE + cell.y / ZEN_HYBRID_GUIDE_CELL_SIZE) * ZEN_HYBRID_GUIDE_PER_FACE
                            + cell.x / ZEN_HYBRID_GUIDE_CELL_SIZE;
                        if (learning > 0.0)
                        {
                            GuideLearn(tile, learning * HybridLuminance(radiance) / (HYBRID_PI * pdf * float(sourceRays)));
                        }
                    }
                }
                // Each source tile averages its completed rays only.
                sky.rgb += sourceSky * (float(sourceRays) / float(max(sourceValid, 1u)));
            }
#endif
        }
    }
    return DiffuseResult(diffuseDistance / max(diffuseHitCount, 1.0), sky, bounce, previous);
}

// Specular occlusion and radiance-cache reflection of one receiver, always at full resolution.
void TraceSpecular(ivec2      p,
                   vec3       position,
                   vec3       n,
                   vec3       ng,
                   inout vec4 specular,
                   inout vec2 reflection,
                   inout vec3 reflectionPrevious)
{
#if defined(HYBRID_RESTIR)
    if (reflectFromDiffuse)
    {
        vec2 completed = diffuseReflectionCompleted;
        specular.a     = completed.x > 0.0 ? diffuseReflectionSum.a / completed.x : -1.0;
        specular.rgb   = diffuseReflectionSum.rgb / max(completed.y, 1.0);
        reflection.x   = diffuseReflectionWeight > 0.0 ? diffuseReflectionHits.x / diffuseReflectionWeight : 0.0;
        reflection.y   = completed.y > 0.0 ? diffuseReflectionHits.y / completed.y : -1.0;
        if (restirOptions.y != 0u)
        {
            initialReflection.count = uint(completed.y);
            GIFinalize(initialReflection, GITarget(GIIntegrand(initialReflection, position, n, ng, diffuseReflectionView,
                                                               diffuseReflectionRoughness, true)));
        }
    }
    else
#endif
#if !defined(HYBRID_ABLATE_REFLECTION)
        if (hybrid.rejection.y > 0.0)
    {
        vec3 view =
            sceneUbo.viewPosition.w < 0.5 ? normalize(sceneUbo.lightInfo.yzw) : normalize(sceneUbo.viewPosition.xyz - position);
        float roughness       = clamp(texelFetch(receiverRoughness, p, 0).g, 0.04, 1.0);
        uint  specularSamples = hybrid.sampling.y > 4u ? hybrid.sampling.y : 1u;
        specular.a            = 0.0;
        float hitCount        = 0.0;
        vec2  completed       = vec2(specularSamples); // Visibility and reflection samples with a result.
        for (uint i = 0u; i < specularSamples; ++i)
        {
            vec3 w = HybridSpecularDirection(n, view, roughness, uvec2(p), hybrid.sampling.x * specularSamples + i, 0x1234u);
            if (dot(ng, w) > 0.0)
            {
                uint hitSeed =
                    HybridHash(HybridMorton(uvec2(p)) ^ HybridHash(hybrid.sampling.x * specularSamples + i) ^ 0x5eec1234u);
                PrepareRayHitLighting(hitSeed, false, 3u);
#    ifdef HYBRID_RADIANCE_CACHE
                rayCacheCone = roughness * roughness;
#    endif
                bool            sharedReflection = FeatureHardware(SPECULAR_PROVIDER) == FeatureHardware(REFLECTION_PROVIDER);
                HybridRayResult hit =
                    TraceFeature(SPECULAR_PROVIDER, position, ng, w, hybrid.reflection.x != 0u && sharedReflection);
                completed.x -= hit.valid ? 0.0 : 1.0;
                if (hit.valid && !hit.hit)
                {
                    specular.a += 1.0;
                }
                if (hybrid.reflection.x != 0u && !sharedReflection)
                {
                    hit = TraceFeature(REFLECTION_PROVIDER, position, ng, w, true);
                }
                completed.y -= hit.valid ? 0.0 : 1.0;
                if (hit.hit && hybrid.reflection.x != 0u)
                {
                    bool rejected;
                    // Store the unconditional hit integral C=(1-S)*H. Averaging conditional H
                    // separately from the hit fraction would multiply two noisy estimates.
#    if defined(HYBRID_COUNT_RAYS)
                    issuedRayFeature = 3u;
#    endif
                    vec3 radiance = TraceHitRadiance(hit, w, hitSeed, rejected);
                    AccumulateReflection(hit, w, position, radiance, hitSeed, roughness, rejected, specular, reflection,
                                         reflectionPrevious, hitCount);
                }
            }
        }
        // Negative markers distinguish missing observations from measured occlusion or black
        // radiance. Temporal accumulation holds valid history without adding a sample.
        specular.a          = completed.x > 0.0 ? specular.a / completed.x : -1.0;
        specular.rgb       /= max(completed.y, 1.0);
        reflectionPrevious /= max(completed.y, 1.0);
        reflection.x       /= max(hitCount, 1.0);
        reflection.y        = completed.y > 0.0 ? reflection.y : -1.0;
#    if defined(HYBRID_RESTIR)
        if (restirOptions.y != 0u && roughness >= 0.3)
        {
            initialReflection.count = uint(completed.y);
            GIFinalize(initialReflection, GITarget(GIIntegrand(initialReflection, position, n, ng, view, roughness, true)));
        }
#    endif
    }
#endif
}

void StoreSpecular(ivec2 p, vec4 specular, vec2 reflection, vec3 reflectionPrevious)
{
    imageStore(rawSpecular, p, specular);
    if (hybrid.reflection.x != 0u)
    {
        imageStore(rawReflection, p, vec4(reflection, 0, 0));
        if (hybrid.reflection.w != 0u)
        {
            imageStore(rawReflectionPrevious, p, vec4(reflectionPrevious, 0));
        }
    }
#if defined(HYBRID_RESTIR)
    uint index = uint(p.y * textureSize(receiverDepth, 0).x + p.x);
    if (restirOptions.y != 0u || index == 0u)
    {
        currentReflections[index] = PackGIReservoir(initialReflection);
    }
#endif
}

void StoreDiffuse(ivec2 p, DiffuseResult d)
{
    if (hybrid.reservoir.w != 0u || all(equal(p, ivec2(0))))
    {
        imageStore(rawDiffuseDistance, p, vec4(d.distance));
    }
    imageStore(rawSky, p, d.sky);
    if (hybrid.bounce.x != 0u)
    {
        imageStore(rawBounce, p, d.bounce);
        if (hybrid.bounce.w != 0u)
        {
            imageStore(rawBouncePrevious, p, d.previous);
        }
    }
#if defined(HYBRID_RESTIR)
    uint index = uint(p.y * textureSize(receiverDepth, 0).x + p.x);
    if (restirOptions.z != 0u || index == 0u)
    {
        currentReservoirs[index] = PackGIReservoir(initialGI);
    }
#endif
}

#if defined(HYBRID_COUNT_RAYS)
void StoreRayCounts(uint lane, uint slot)
{
#    ifdef HYBRID_RAY_PIPELINE
    for (uint component = 0u; component < 4u; ++component)
    {
        atomicAdd(rayCounts[slot][component], issuedComputeRays[component]);
        atomicAdd(rayCounts[slot + 1u][component], issuedHardwareRays[component]);
        atomicAdd(rayCounts[slot + 2u][component], issuedFieldWork[component]);
        atomicAdd(rayCounts[slot + 3u][component], hardwareCandidates[component]);
    }
#    else
    computeRaySums[lane]  = issuedComputeRays;
    hardwareRaySums[lane] = issuedHardwareRays;
    fieldWorkSums[lane]   = issuedFieldWork;
    candidateSums[lane]   = hardwareCandidates;
    barrier();
    for (uint stride = TRACE_THREADS / 2u; stride > 0u; stride /= 2u)
    {
        if (lane < stride)
        {
            computeRaySums[lane]  += computeRaySums[lane + stride];
            hardwareRaySums[lane] += hardwareRaySums[lane + stride];
            fieldWorkSums[lane]   += fieldWorkSums[lane + stride];
            candidateSums[lane]   += candidateSums[lane + stride];
        }
        barrier();
    }
    if (lane == 0u)
    {
        rayCounts[slot]      = computeRaySums[0];
        rayCounts[slot + 1u] = hardwareRaySums[0];
        rayCounts[slot + 2u] = fieldWorkSums[0];
        rayCounts[slot + 3u] = candidateSums[0];
    }
#    endif
}
#endif

#if !defined(HYBRID_NO_GUIDE) && !defined(HYBRID_RAY_PIPELINE)
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
#    if defined(HYBRID_COUNT_RAYS)
    uint tile = gl_WorkGroupID.y * gl_NumWorkGroups.x + gl_WorkGroupID.x;
    StoreRayCounts(lane, tile * (ZEN_GI_TRACE_COUNT_WORDS / 4u));
#    endif
}
#endif

// Fixed pixel tiles preserve the archived sparse-reference mask independently of workgroup size.
bool ReferenceReceiver(ivec2 p)
{
    bool selected = true;
    if (hybrid.provider.w > 1u)
    {
        uvec2 tile      = uvec2(p) / 8u;
        uvec2 tiles     = (uvec2(textureSize(receiverDepth, 0)) + 7u) / 8u;
        uint  stride    = hybrid.provider.w;
        uvec2 stratum   = tile / stride;
        uvec2 available = min(uvec2(stride), tiles - stratum * stride);
        uint  chosen    = HybridHash(stratum.x + 4099u * stratum.y + 0x3c6ef372u) % (available.x * available.y);
        selected        = all(equal(tile % stride, uvec2(chosen % available.x, chosen / available.x)));
    }
    return selected;
}

void main()
{
#if defined(HYBRID_RAY_PIPELINE)
    ivec2 p = ivec2(gl_LaunchIDEXT.xy);
#else
    ivec2 p    = ivec2(gl_GlobalInvocationID.xy);
    uint  lane = gl_LocalInvocationIndex;
#endif
#if defined(HYBRID_NO_GUIDE)
    guideValid = false;
#elif defined(HYBRID_RAY_PIPELINE)
    uvec2 tile    = uvec2(p) / 16u;
    uint  columns = (gl_LaunchSizeEXT.x + 15u) / 16u;
    guideBase     = (tile.y * columns + tile.x) * (ZEN_HYBRID_GUIDE_TILES + 1u);
    guideValid    = hybrid.provider.y != 0u;
#else
    BuildGuide(lane);
#endif
#if defined(HYBRID_RESTIR)
    initialGI = EmptyGIReservoir();
#endif
    if (IsReceiver(p) && ReferenceReceiver(p))
    {
#if !defined(HYBRID_NO_GUIDE)
#    ifdef HYBRID_RAY_PIPELINE
        atomicAdd(guideSumWords[(guideBase + ZEN_HYBRID_GUIDE_TILES) * HYBRID_GUIDE_WORDS], 1u);
#    else
        atomicAdd(guideReceivers, 1u);
#    endif
#endif
        vec3 position = HybridPosition(p), n = HybridNormal(p), ng = HybridGeometricNormal(p);
        // ReSTIR traces reset the reflection reservoir in TraceDiffuse, which may stream into it.
        StoreDiffuse(p, TraceDiffuse(p, position, n, ng, hybrid.sampling.x, 0u, 1.0));
        vec4 specular           = vec4(0, 0, 0, 1);
        vec2 reflection         = vec2(0);
        vec3 reflectionPrevious = vec3(0);
        TraceSpecular(p, position, n, ng, specular, reflection, reflectionPrevious);
        StoreSpecular(p, specular, reflection, reflectionPrevious);
    }
    else if (HybridInView(p))
    {
        StoreDiffuse(p, DiffuseResult(0.0, vec4(0), vec4(0), vec4(0)));
#if defined(HYBRID_RESTIR)
        initialReflection = EmptyGIReservoir();
#endif
        StoreSpecular(p, vec4(0, 0, 0, 1), vec2(0), vec3(0));
    }
#if !defined(HYBRID_NO_GUIDE) && !defined(HYBRID_RAY_PIPELINE)
    FinishGuide(lane);
#endif
#if defined(HYBRID_COUNT_RAYS)
    if (HybridInView(p))
    {
        uvec2 extent      = uvec2(textureSize(receiverDepth, 0));
        uvec2 tiles       = (extent + 15u) / 16u;
        uint  offset      = tiles.x * tiles.y * (ZEN_GI_TRACE_COUNT_WORDS / 4u) + uint(p.y * int(extent.x) + p.x) * 2u;
        rayCounts[offset] = traceBreakdown;
#    ifdef HYBRID_RADIANCE_CACHE
        rayCounts[offset + 1u] = uvec4(cacheTraceCounts, 0u, 0u);
#    else
        rayCounts[offset + 1u] = uvec4(0); // cache queries, cache hits, reserved, reserved.
#    endif
    }
#endif
#if defined(HYBRID_COUNT_RAYS) && defined(HYBRID_RAY_PIPELINE)
    StoreRayCounts(0u, (guideBase / (ZEN_HYBRID_GUIDE_TILES + 1u)) * (ZEN_GI_TRACE_COUNT_WORDS / 4u));
#endif
}
