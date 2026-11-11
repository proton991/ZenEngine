#include "hybrid_guides.glsl"
#include "hybrid_sampling.glsl"
#include "hybrid_provider.glsl"
#include "restir_target.glsl"
layout(local_size_x = 8, local_size_y = 8) in;
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
    vec3            origin         = HybridTraceOrigin(position, geometric, 0.0);
    vec3            endpointNormal = dot(r.normal, origin - r.position) >= 0.0 ? r.normal : -r.normal;
    vec3            endpoint       = HybridTraceOrigin(r.position, endpointNormal, 0.0);
    vec3            delta          = endpoint - origin;
    float           distance       = length(delta);
    HybridRayResult hit            = HardwareTraceRay(origin, delta / max(distance, 1e-20), distance, false);
    return distance > 0.0 && hit.valid && !hit.hit;
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
    float centerTarget    = GIReceiverTarget(center, q, specular);
    float sourceAtCenter  = GIReceiverTarget(center, source, specular);
    float reverseJacobian = GIJacobian(center, source.position, q.position);
    sourceAtCenter        = reverseJacobian > 0.0 ? sourceAtCenter / reverseJacobian : 0.0;
    if (center.weight > 0.0 && sourceAtCenter > 0.0 && any(notEqual(q.position, source.position))
        && !GIVisible(center, source.position, source.geometric))
    {
        sourceAtCenter = 0.0;
    }
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
void main()
{
    ivec2 p       = ivec2(gl_GlobalInvocationID.xy);
    bool  process = HybridInView(p);
#ifdef RESTIR_SPATIAL
    process = process && (restirOptions.x == 0u || (uint(p.x + p.y) & 1u) == restirOptions.w - 1u);
#endif
    if (process)
    {
        uint       index = GIIndex(p), seed = index ^ HybridHash(hybrid.sampling.x);
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
#ifdef RESTIR_SPATIAL
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
#else
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
#endif
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
#ifdef RESTIR_SPATIAL
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
#endif
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
}
