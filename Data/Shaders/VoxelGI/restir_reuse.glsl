#include "hybrid_guides.glsl"
#include "hybrid_sampling.glsl"
#ifdef HYBRID_RAY_PIPELINE
#    include "gi_common.glsl"
#    include "ray_shadow.glsl"
#else
#    include "hybrid_provider.glsl"
#endif
#include "restir_target.glsl"
#ifndef HYBRID_RAY_PIPELINE
layout(local_size_x = 8, local_size_y = 8) in;
#endif
layout(std430, set = 1, binding = 6) readonly buffer GIReservoirCurrent
{
    PackedGIReservoir currentReservoirs[];
};
layout(std430, set = 1, binding = 7) readonly buffer GIReservoirPrevious
{
    PackedGIReservoir previousReservoirs[];
};
layout(set = 1, binding = 8) uniform sampler2D previousPosition;
layout(set = 1, binding = 9) uniform sampler2D previousNormal;
layout(set = 1, binding = 10) uniform usampler2D previousReceiver;
layout(set = 1, binding = 11, rgba16f) uniform image2D rawBounce;
layout(std430, set = 1, binding = 12) writeonly buffer GIReservoirOutput
{
    PackedGIReservoir outputReservoirs[];
};
layout(std430, set = 1, binding = 13) readonly buffer ReflectionReservoirCurrent
{
    PackedGIReservoir currentReflections[];
};
layout(std430, set = 1, binding = 14) readonly buffer ReflectionReservoirPrevious
{
    PackedGIReservoir previousReflections[];
};
layout(std430, set = 1, binding = 15) writeonly buffer ReflectionReservoirOutput
{
    PackedGIReservoir outputReflections[];
};
layout(set = 1, binding = 16, rgba16f) uniform image2D rawSpecular;
layout(set = 1, binding = 17) uniform sampler2D previousLength;
layout(set = 1, binding = 18, r32f) uniform image2D rawDiffuseDistance;
layout(set = 1, binding = 19, rg32f) uniform image2D rawReflection;
// Each texel's receiver packed into 32 bytes: written by temporal reuse for its own texel, read by spatial reuse
// for neighbors as one load in place of six texture fetches scattered across the reuse radius.
// [2i]: position bits, encoded shading normal; [2i + 1]: packed geometric normal, identity (0 without a
// receiver), roughness bits. Values are copied bit for bit, so the estimator is unchanged.
#ifdef RESTIR_SPATIAL
layout(std430, set = 1, binding = 20) readonly buffer GIReceiverGuides
#else
layout(std430, set = 1, binding = 20) writeonly buffer GIReceiverGuides
#endif
{
    uvec4 receiverGuides[];
};
uint GIIndex(ivec2 p)
{
    return uint(p.y * textureSize(receiverDepth, 0).x + p.x);
}
float GIRandom(uint seed)
{
    return float(HybridHash(seed) >> 8u) / 16777216.0;
}
vec3 GIView(vec3 position, bool previous)
{
    return sceneUbo.viewPosition.w < 0.5
             ? (previous ? hybrid.previousViewDirection.xyz : normalize(sceneUbo.lightInfo.yzw))
             : normalize((previous ? hybrid.previousViewPosition.xyz : sceneUbo.viewPosition.xyz) - position);
}
bool GIVisible(GIReservoir r, vec3 position, vec3 geometric)
{
    vec3  origin         = HybridTraceOrigin(position, geometric, 0.0);
    vec3  endpointNormal = dot(r.normal, origin - r.position) >= 0.0 ? r.normal : -r.normal;
    vec3  endpoint       = HybridTraceOrigin(r.position, endpointNormal, 0.0);
    vec3  delta          = endpoint - origin;
    float distance       = length(delta);
    return distance > 0.0 && !HardwareOccluded(origin, delta / max(distance, 1e-20), distance);
}
struct GIReceiver
{
    vec3  position;
    vec3  normal;
    vec3  geometric;
    vec3  view;
    float roughness;
};
float GIReceiverTarget(GIReservoir r, GIReceiver q, bool specular)
{
    return GITarget(GIIntegrand(r, q.position, q.normal, q.geometric, q.view, q.roughness, specular));
}
// Pairwise balance heuristic. Each neighbor contributes one pair with the center;
// averaging the pairs gives weights that partition unity even when visibility supports differ.
// All target densities below use the center's solid-angle measure (source density / Jacobian).
// The center's target at the source in the center's measure, before visibility.
float GISourceAtCenter(GIReservoir center, GIReceiver q, GIReceiver source, bool specular)
{
    float sourceAtCenter  = GIReceiverTarget(center, source, specular);
    float reverseJacobian = GIJacobian(center, source.position, q.position);
    return reverseJacobian > 0.0 ? sourceAtCenter / reverseJacobian : 0.0;
}
// Whether the pair's source-at-center density depends on the center sample being visible from the source.
bool GISourceNeedsVisibility(GIReservoir center, GIReceiver q, GIReceiver source, float sourceAtCenter)
{
    return center.weight > 0.0 && sourceAtCenter > 0.0 && any(notEqual(q.position, source.position));
}
// One pair given its visibility-tested source-at-center density.
float GIPairTested(inout GIReservoir result,
                   GIReservoir       center,
                   GIReservoir       other,
                   GIReceiver        q,
                   GIReceiver        source,
                   bool              specular,
                   float             pairWeight,
                   uint              count,
                   float             random,
                   float             sourceAtCenter)
{
    float centerTarget = GIReceiverTarget(center, q, specular);
    // The center participates in every pair. Divide its confidence between those
    // pairs so equal reservoirs receive equal weight across the whole neighborhood.
    float centerCount  = float(center.count) * pairWeight;
    float centerMass   = centerCount * centerTarget;
    float centerShare  = centerMass / max(centerMass + float(count) * sourceAtCenter, 1e-30);
    float target       = GIReceiverTarget(other, q, specular);
    float jacobian     = GIJacobian(other, source.position, q.position);
    float sourceTarget = jacobian > 0.0 ? GIReceiverTarget(other, source, specular) / jacobian : 0.0;
    float sourceMass   = float(count) * sourceTarget;
    float sourceShare  = sourceMass / max(sourceMass + centerCount * target, 1e-30);
    GIStream(result, other, pairWeight * sourceShare * target * other.weight * jacobian, 0u, random);
    return pairWeight * centerShare;
}
float GIPair(inout GIReservoir result,
             GIReservoir       center,
             GIReservoir       other,
             GIReceiver        q,
             GIReceiver        source,
             bool              specular,
             float             pairWeight,
             uint              count,
             float             random)
{
    float sourceAtCenter = GISourceAtCenter(center, q, source, specular);
    if (GISourceNeedsVisibility(center, q, source, sourceAtCenter) && !GIVisible(center, source.position, source.geometric))
    {
        sourceAtCenter = 0.0;
    }
    return GIPairTested(result, center, other, q, source, specular, pairWeight, count, random, sourceAtCenter);
}
#ifdef RESTIR_SPATIAL
// Spatial reuse traces up to five visibility rays per receiver and signal: one per neighbor for the
// pairwise MIS weights (restirSpatial.y) and one for the selected sample. Without the MIS rays the weights
// assume the center sample is visible from every neighbor; the final ray still removes occluded samples.
// Every value live across a ray-pipeline trace is saved around the call, so the rays are gathered first
// and traced from two loops that keep only packed indices and masks live; the merge then recomputes its
// inputs. The compute variant shares this order, and every estimator value matches the inline-visibility form.
GIReceiver GICenterReceiver(ivec2 p)
{
    vec3 position = HybridPosition(p);
    return GIReceiver(position, HybridNormal(p), HybridGeometricNormal(p), GIView(position, false),
                      clamp(texelFetch(receiverRoughness, p, 0).g, 0.04, 1.0));
}
struct GIGuide
{
    GIReceiver receiver;
    uint       identity;
};
GIGuide GIReceiverGuide(ivec2 pixel)
{
    uint  index    = GIIndex(pixel);
    uvec4 first    = receiverGuides[2u * index];
    uvec4 second   = receiverGuides[2u * index + 1u];
    vec3  position = uintBitsToFloat(first.xyz);
    return GIGuide(GIReceiver(position, DecodeGBufferNormal(unpackUnorm2x16(first.w)),
                              DecodeGBufferNormal(unpackUnorm2x16(second.x)), GIView(position, false),
                              uintBitsToFloat(second.z)),
                   second.y);
}
GIReservoir GICurrentReservoir(uint index, bool specular)
{
    return UnpackGIReservoir(specular ? currentReflections[index] : currentReservoirs[index]);
}
uint GIPackPixel(ivec2 pixel)
{
    return uint(pixel.x) | (uint(pixel.y) << 16u);
}
ivec2 GIUnpackPixel(uint pixel)
{
    return ivec2(pixel & 0xffffu, pixel >> 16u);
}
// One signal per dispatch (RESTIR_SPATIAL_SIGNAL 0 diffuse, 1 reflection) keeps half the neighbor and result
// state live, which raises occupancy: the combined form filled the register file at about 130 per thread.
// Both signals draw the same neighbors and random numbers, so the split matches the combined estimator.
void SpatialMain(ivec2 p)
{
    uint       index = GIIndex(p), seed = index ^ HybridHash(hybrid.sampling.x);
    GIReceiver q         = GICenterReceiver(p);
    uint       identity  = texelFetch(receiverSurface, p, 0).g;
    bool       receiver  = identity != 0u && texelFetch(receiverDepth, p, 0).r < 1.0;
    float      threshold = hybrid.rejection.x * max(1.0, length(q.position - sceneUbo.viewPosition.xyz));
    bool       specular  = RESTIR_SPATIAL_SIGNAL != 0;
    bool       allocated = (specular ? restirOptions.y : restirOptions.z) != 0u;
    bool       enabled   = allocated && receiver && (!specular || q.roughness >= 0.3);
    // Up to four neighbors, their count and the pairs that need visibility.
    uint  neighbors[4];
    float sourceAtCenter[4];
    uint  neighborCount = 0u;
    uint  pending       = 0u;
    if (enabled && restirOptions.x != 0u)
    {
        GIReservoir fresh  = GICurrentReservoir(index, specular);
        float       radius = 32.0 * float(textureSize(receiverDepth, 0).y) / 1080.0;
        for (uint k = 0u; k < min(restirSpatial.x, 4u); ++k)
        {
            float angle    = 2.0 * HYBRID_PI * GIRandom(seed ^ (k * 3u + 0x4179u));
            float distance = radius * sqrt(GIRandom(seed ^ (k * 3u + 0x417au)));
            ivec2 pixel    = p + ivec2(round(distance * vec2(cos(angle), sin(angle))));
            // Only the opposite parity is read during this pass; none of those
            // reservoirs is concurrently written by this dispatch.
            pixel.x += ((pixel.x + pixel.y) & 1) == ((p.x + p.y) & 1) ? 1 : 0;
            if (HybridInView(pixel) && any(notEqual(p, pixel)))
            {
                GIGuide    guide  = GIReceiverGuide(pixel);
                GIReceiver source = guide.receiver;
                if (guide.identity != 0u
                    && GICompatibleSurface(identity, guide.identity, q.position, q.normal, q.geometric, source.position,
                                           source.normal, source.geometric, threshold)
                    && (!specular || (source.roughness >= 0.3 && abs(source.roughness - q.roughness) <= 0.15 * q.roughness)))
                {
                    uint slot             = neighborCount++;
                    neighbors[slot]       = GIPackPixel(pixel);
                    sourceAtCenter[slot]  = GISourceAtCenter(fresh, q, source, specular);
                    bool needed           = GISourceNeedsVisibility(fresh, q, source, sourceAtCenter[slot]);
                    pending              |= restirSpatial.y != 0u && needed ? 1u << slot : 0u;
                }
            }
        }
    }
    // Is each pending center sample visible from its neighbor? One trace site, little live state.
    uint occluded = 0u;
    while (pending != 0u)
    {
        uint slot          = findLSB(pending);
        pending           &= pending - 1u;
        GIReceiver source  = GIReceiverGuide(GIUnpackPixel(neighbors[slot])).receiver;
        if (!GIVisible(GICurrentReservoir(index, specular), source.position, source.geometric))
        {
            occluded |= 1u << slot;
        }
    }
    // Merge, then decide the final visibility ray.
    q                    = GICenterReceiver(p);
    GIReservoir fresh    = allocated ? GICurrentReservoir(index, specular) : EmptyGIReservoir();
    GIReservoir result   = EmptyGIReservoir();
    vec3        value    = vec3(0);
    float       reach    = 0.0;
    bool        finalRay = false;
    if (enabled)
    {
        uint  totalCount  = fresh.count;
        float centerShare = neighborCount > 0u ? 0.0 : 1.0;
        for (uint k = 0u; k < neighborCount; ++k)
        {
            ivec2       pixel      = GIUnpackPixel(neighbors[k]);
            GIReceiver  source     = GIReceiverGuide(pixel).receiver;
            GIReservoir candidate  = GICurrentReservoir(GIIndex(pixel), specular);
            float       tested     = (occluded & (1u << k)) != 0u ? 0.0 : sourceAtCenter[k];
            centerShare           += GIPairTested(result, fresh, candidate, q, source, specular, 1.0 / float(neighborCount),
                                                  candidate.count, GIRandom(seed ^ (k + 0x511fu)), tested);
            totalCount            += candidate.count;
        }
        GIStream(result, fresh, centerShare * GIReceiverTarget(fresh, q, specular) * fresh.weight, 0u,
                 GIRandom(seed ^ 0x51bed19u));
        // Pair weights already normalize the estimator; M is retained only for future reuse.
        result.count = 1u;
        GIFinalize(result, GIReceiverTarget(result, q, specular));
        result.count       = min(totalCount, 127u);
        bool freshSelected = fresh.weight > 0.0 && all(equal(result.position, fresh.position));
        finalRay           = result.weight > 0.0 && neighborCount > 0u && !freshSelected;
        value              = GIIntegrand(result, q.position, q.normal, q.geometric, q.view, q.roughness, specular);
        reach              = result.weight > 0.0 ? length(result.position - q.position) : 0.0;
    }
    PackedGIReservoir packed = PackGIReservoir(result);
    // A reused sample that the receiver cannot see contributes nothing.
    if (finalRay)
    {
        GIReservoir reused = EmptyGIReservoir();
        reused.position    = result.position;
        reused.normal      = result.normal;
        if (!GIVisible(reused, q.position, q.geometric))
        {
            packed.normalCount &= 0x7fffffffu;
            packed.weight       = 0.0;
            reach               = 0.0;
        }
    }
    if (enabled)
    {
        vec4 observation = specular ? imageLoad(rawSpecular, p) : imageLoad(rawBounce, p);
        observation.rgb  = value * packed.weight;
        if (specular)
        {
            imageStore(rawSpecular, p, observation);
        }
        else
        {
            imageStore(rawBounce, p, observation);
        }
        if (hybrid.reservoir.w != 0u)
        {
            // The denoiser's footprint and virtual reprojection must describe
            // the path selected by reuse, rather than the original ray batch.
            if (specular)
            {
                vec4 guide = imageLoad(rawReflection, p);
                guide.x    = reach;
                imageStore(rawReflection, p, guide);
            }
            else
            {
                imageStore(rawDiffuseDistance, p, vec4(reach));
            }
        }
    }
    if (allocated || index == 0u)
    {
        if (specular)
        {
            outputReflections[index] = packed;
        }
        else
        {
            outputReservoirs[index] = packed;
        }
    }
}
#endif
void main()
{
#ifdef HYBRID_RAY_PIPELINE
    ivec2 p = ivec2(gl_LaunchIDEXT.xy);
#else
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
#endif
    bool process = HybridInView(p);
#ifdef RESTIR_SPATIAL
    process = process && (restirOptions.x == 0u || (uint(p.x + p.y) & 1u) == restirOptions.w - 1u);
    if (process)
    {
        SpatialMain(p);
    }
#else
    if (process)
    {
        uint index = GIIndex(p), seed = index ^ HybridHash(hybrid.sampling.x);
        // This texel's packed receiver for the spatial passes that follow.
        uvec2 packedSurface = texelFetch(receiverSurface, p, 0).rg;
        bool  covered       = texelFetch(receiverDepth, p, 0).r < 1.0;
        receiverGuides[2u * index] =
            uvec4(floatBitsToUint(HybridPosition(p)), packUnorm2x16(texelFetch(receiverNormal, p, 0).rg));
        receiverGuides[2u * index + 1u] =
            uvec4(packedSurface.r, covered ? packedSurface.g : 0u, floatBitsToUint(texelFetch(receiverRoughness, p, 0).g), 0u);
        vec3       position  = HybridPosition(p);
        GIReceiver q         = GIReceiver(position, HybridNormal(p), HybridGeometricNormal(p), GIView(position, false),
                                          clamp(texelFetch(receiverRoughness, p, 0).g, 0.04, 1.0));
        uint       identity  = texelFetch(receiverSurface, p, 0).g;
        bool       receiver  = identity != 0u && texelFetch(receiverDepth, p, 0).r < 1.0;
        float      threshold = hybrid.rejection.x * max(1.0, length(position - sceneUbo.viewPosition.xyz));
        for (uint type = 0u; type < 2u; ++type)
        {
            bool        specular  = type != 0u;
            bool        allocated = (specular ? restirOptions.y : restirOptions.z) != 0u;
            bool        enabled   = allocated && receiver && (!specular || q.roughness >= 0.3);
            GIReservoir fresh     = EmptyGIReservoir();
            if (allocated)
            {
                fresh = UnpackGIReservoir(specular ? currentReflections[index] : currentReservoirs[index]);
            }
            GIReservoir result = EmptyGIReservoir();
            if (enabled)
            {
                uint  totalCount         = fresh.count;
                float centerShare        = 1.0;
                bool  visibilityRequired = false;
#    ifdef RESTIR_SPATIAL
                ivec2 neighbors[4];
                uint  neighborCount = 0u;
                if (restirOptions.x != 0u)
                {
                    float radius = 32.0 * float(textureSize(receiverDepth, 0).y) / 1080.0;
                    for (uint k = 0u; k < 4u; ++k)
                    {
                        float angle    = 2.0 * HYBRID_PI * GIRandom(seed ^ (k * 3u + 0x4179u));
                        float distance = radius * sqrt(GIRandom(seed ^ (k * 3u + 0x417au)));
                        ivec2 pixel    = p + ivec2(round(distance * vec2(cos(angle), sin(angle))));
                        // Only the opposite parity is read during this pass; none of those
                        // reservoirs is concurrently written by this dispatch.
                        pixel.x += ((pixel.x + pixel.y) & 1) == ((p.x + p.y) & 1) ? 1 : 0;
                        if (HybridInView(pixel) && any(notEqual(p, pixel)) && texelFetch(receiverDepth, pixel, 0).r < 1.0)
                        {
                            float roughness = texelFetch(receiverRoughness, pixel, 0).g;
                            if (GICompatibleSurface(identity, texelFetch(receiverSurface, pixel, 0).g, q.position, q.normal,
                                                    q.geometric, HybridPosition(pixel), HybridNormal(pixel),
                                                    HybridGeometricNormal(pixel), threshold)
                                && (!specular || (roughness >= 0.3 && abs(roughness - q.roughness) <= 0.15 * q.roughness)))
                            {
                                neighbors[neighborCount++] = pixel;
                            }
                        }
                    }
                }
                centerShare        = neighborCount > 0u ? 0.0 : 1.0;
                visibilityRequired = neighborCount > 0u;
                for (uint k = 0u; k < neighborCount; ++k)
                {
                    ivec2       pixel          = neighbors[k];
                    vec3        sourcePosition = HybridPosition(pixel);
                    GIReceiver  source = GIReceiver(sourcePosition, HybridNormal(pixel), HybridGeometricNormal(pixel),
                                                    GIView(sourcePosition, false), texelFetch(receiverRoughness, pixel, 0).g);
                    GIReservoir candidate =
                        UnpackGIReservoir(specular ? currentReflections[GIIndex(pixel)] : currentReservoirs[GIIndex(pixel)]);
                    centerShare += GIPair(result, fresh, candidate, q, source, specular, 1.0 / float(neighborCount),
                                          candidate.count, GIRandom(seed ^ (k + 0x511fu)));
                    totalCount  += candidate.count;
                }
#    else
                vec4  motion = HybridMotion(p);
                ivec2 pixel  = ivec2(floor(vec2(p) + motion.xy * 0.5 * vec2(textureSize(receiverDepth, 0)) + 0.5));
                bool  reuse  = (specular ? hybrid.reflection.y : hybrid.bounce.y) != 0u && hybrid.rejection.z > 0.0
                            && motion.z > 0.0 && HybridInView(pixel);
                if (reuse)
                {
                    vec3       oldPosition = texelFetch(previousPosition, pixel, 0).xyz;
                    uvec2      oldReceiver = texelFetch(previousReceiver, pixel, 0).rg;
                    GIReceiver source = GIReceiver(oldPosition, DecodeGBufferNormal(texelFetch(previousNormal, pixel, 0).rg),
                                                   DecodeGBufferNormal(unpackUnorm2x16(oldReceiver.r)),
                                                   GIView(oldPosition, true), texelFetch(previousLength, pixel, 0).a);
                    if (GICompatibleSurface(identity, oldReceiver.g, q.position, q.normal, q.geometric, source.position,
                                            source.normal, source.geometric, threshold)
                        && (!specular
                            || (source.roughness >= 0.3
                                && abs(source.roughness - q.roughness) <= HybridReflectionRoughnessTolerance(q.roughness))))
                    {
                        GIReservoir old        = UnpackGIReservoir(specular ? previousReflections[GIIndex(pixel)]
                                                                            : previousReservoirs[GIIndex(pixel)]);
                        uint        historyCap = specular ? hybrid.reservoir.z : hybrid.reservoir.y;
                        uint        count      = min(old.count, min(120u, historyCap * max(fresh.count, 1u)));
                        centerShare = GIPair(result, fresh, old, q, source, specular, 1.0, count, GIRandom(seed ^ 0x3194a7bdu));
                        totalCount         += count;
                        visibilityRequired  = any(notEqual(q.position, source.position));
                    }
                }
#    endif
                GIStream(result, fresh, centerShare * GIReceiverTarget(fresh, q, specular) * fresh.weight, 0u,
                         GIRandom(seed ^ 0x51bed19u));
                // Pair weights already normalize the estimator; M is retained only for future reuse.
                result.count = 1u;
                GIFinalize(result, GIReceiverTarget(result, q, specular));
                result.count       = min(totalCount, 127u);
                bool freshSelected = fresh.weight > 0.0 && all(equal(result.position, fresh.position));
                if (result.weight > 0.0 && visibilityRequired && !freshSelected && !GIVisible(result, position, q.geometric))
                {
                    result.weight = 0.0;
                }
#    ifdef RESTIR_SPATIAL
                vec3 value =
                    GIIntegrand(result, q.position, q.normal, q.geometric, q.view, q.roughness, specular) * result.weight;
                vec4 observation = specular ? imageLoad(rawSpecular, p) : imageLoad(rawBounce, p);
                observation.rgb  = value;
                if (specular)
                {
                    imageStore(rawSpecular, p, observation);
                }
                else
                {
                    imageStore(rawBounce, p, observation);
                }
                if (hybrid.reservoir.w != 0u)
                {
                    // The denoiser's footprint and virtual reprojection must describe
                    // the path selected by reuse, rather than the original ray batch.
                    float distance = result.weight > 0.0 ? length(result.position - position) : 0.0;
                    if (specular)
                    {
                        vec4 guide = imageLoad(rawReflection, p);
                        guide.x    = distance;
                        imageStore(rawReflection, p, guide);
                    }
                    else
                    {
                        imageStore(rawDiffuseDistance, p, vec4(distance));
                    }
                }
#    endif
            }
            if (allocated || index == 0u)
            {
                if (specular)
                {
                    outputReflections[index] = PackGIReservoir(result);
                }
                else
                {
                    outputReservoirs[index] = PackGIReservoir(result);
                }
            }
        }
    }
#endif
}
