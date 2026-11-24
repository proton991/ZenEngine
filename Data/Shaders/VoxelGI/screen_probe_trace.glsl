#if defined(SCREEN_PROBE_SAMPLE_HITS) || defined(SCREEN_PROBE_SHADE_HITS)
#    define SCREEN_PROBE_HIT_COMPUTE
#endif
#if defined(SCREEN_PROBE_HIT_COMPUTE) || defined(SCREEN_PROBE_TRACE_HITS)
#    define SCREEN_PROBE_HIT_PASS
#else
#    define SCREEN_RAYS_WRITE
#endif
#ifdef SCREEN_PROBE_HIT_COMPUTE
#    define HYBRID_RECORDED_HIT_SHADING
#    define WRC_LIGHTING_WRITE
#endif
#define SCREEN_CDF_READ
#if defined(HYBRID_FIELD_TRAVERSAL) || defined(SCREEN_PROBE_ACCUMULATE)
#    define SCREEN_PROBE_COMPUTE
#endif
#include "screen_probe.glsl"
#ifdef SCREEN_PROBE_ACCUMULATE
layout(local_size_x = ZEN_SCREEN_PROBE_RAYS) in;
#else
#    define NATIVE_CACHE_MISS_TRANSPORT
layout(set = 1, binding = 7) uniform samplerCube coneEnvironmentMap;
#    ifdef HYBRID_FIELD_TRAVERSAL
#        include "native_field.glsl"
layout(local_size_x = ZEN_SCREEN_PROBE_RAYS) in;
#    else
#        include "hybrid_provider.glsl"
#    endif
#    ifdef SCREEN_PROBE_HIT_COMPUTE
// The compute passes read the cache at recorded hits; they trace no paths.
#        include "world_cache.glsl"
#        include "hybrid_common.glsl"
#    else
#        include "native_path.glsl"
#    endif
#    include "probe_light_sampling.glsl"
#    include "environment_sampling.glsl"
#    ifdef SCREEN_PROBE_HIT_COMPUTE
// Every compute variant declares the shading pass's resources, so all bind the same inputs.
#        include "ray_hit_lighting.glsl"
#        ifdef SCREEN_PROBE_SHADE_HITS
#            include "sampled_hit_diffuse.glsl"
#        endif
// Compute passes: an RT pipeline would give these threads its traversal register allocation.
layout(local_size_x = 64) in;
// Threads of the two-dimensional grids of 64-thread groups, in order.
uint ProbeComputeThread()
{
    return (gl_WorkGroupID.y * gl_NumWorkGroups.x + gl_WorkGroupID.x) * gl_WorkGroupSize.x + gl_LocalInvocationID.x;
}
#    endif
#endif // SCREEN_PROBE_ACCUMULATE
#if defined(SCREEN_PROBE_HIT_PASS) || defined(SCREEN_PROBE_ACCUMULATE)
#    include "screen_probe_hits.glsl"
#    ifdef SCREEN_PROBE_HIT_PASS
void ProbeWriteResolved(uint address, vec3 radiance, bool surface, bool valid)
{
    probeHits[address]      = kHitResolved | (surface ? kHitSurface : 0u) | (valid ? kHitValid : 0u);
    probeHits[address + 1u] = floatBitsToUint(radiance.r);
    probeHits[address + 2u] = floatBitsToUint(radiance.g);
    probeHits[address + 3u] = floatBitsToUint(radiance.b);
}
// The sample a record slot holds this frame; false for an empty slot.
bool ProbeSlotSample(uint slot, out uint index, out uint ray, out uint batch)
{
    uint rays  = uint(ZEN_SCREEN_PROBE_RAYS);
    index      = slot / rays % screenProbe.grid.w;
    ray        = slot % rays;
    batch      = slot / rays / screenProbe.grid.w;
    bool valid = probes[index].position.w != 0.0 && batch < ProbeScheduledBatches(index);
    return valid;
}
#    endif
#endif
#ifndef SCREEN_PROBE_ACCUMULATE
// Lanes drawn from the environment's importance distribution when it lights the scene. Without
// them a new probe finds the sun and bright sky only by chance: too dark until a rare hit, and
// a sparkle when one arrives, for hundreds of frames after the view turns.
const uint kProbeEnvironmentLanes = 4u;
bool       ProbeEnvironmentSampling()
{
    return sceneUbo.environment.x * sceneUbo.environment.z * gi.lighting.y > 0.0
        && environmentRows[ZEN_ENVIRONMENT_IMPORTANCE_ROWS] > 0.0;
}
// Sample and evaluate the same quantized CDF, including bins rounded to zero.
float ProbeCDFBoundary(uint index, uint boundary)
{
    uint entry = max(boundary, 1u) - 1u;
    vec2 pair  = unpackUnorm2x16(probeCDF[index * ZEN_SCREEN_PROBE_PROPOSAL_WORDS + entry / 2u]);
    return boundary == 0u ? 0.0 : pair[entry & 1u];
}
vec4 ProbeSampleDirection(uint      index,
                          uint      ray,
                          vec3      normal,
                          vec2      u,
                          float     weightSum,
                          vec3      axes[8],
                          float     weights[8],
                          float     lobes[8],
                          ProbeLobe learned[3],
                          bool      hasLearned)
{
    vec3  direction;
    float pdf;
    uint  environmentCount  = ProbeEnvironmentSampling() ? kProbeEnvironmentLanes : 0u;
    uint  localCount        = weightSum > 1e-20 ? uint(3 * ZEN_SCREEN_PROBE_RAYS / 4) - environmentCount : 0u;
    uint  uniformCount      = (uint(ZEN_SCREEN_PROBE_RAYS) - environmentCount) / 2u - localCount / 2u;
    uint  cdfCount          = uint(ZEN_SCREEN_PROBE_RAYS) - localCount - uniformCount - environmentCount;
    uint  learnedCount      = hasLearned ? min(uint(ZEN_SCREEN_PROBE_RAYS / 4), localCount + cdfCount / 2u) : 0u;
    uint  fromLights        = min(localCount, learnedCount);
    localCount             -= fromLights;
    cdfCount               -= learnedCount - fromLights;
    if (ray < uniformCount)
    {
        // The stratification grid must tile every uniform lane (2, 6 or 8 of them).
        uint columns = uniformCount >= 4u ? uniformCount / 2u : uniformCount;
        u            = (vec2(ray % columns, ray / columns) + u) / vec2(columns, uniformCount / columns);
        float radius = sqrt(max(0.0, 1.0 - u.x * u.x));
        direction = HybridBasis(normal) * vec3(radius * cos(2.0 * HYBRID_PI * u.y), radius * sin(2.0 * HYBRID_PI * u.y), u.x);
    }
    else if (ray < uniformCount + localCount)
    {
        // Stratify the light mixture across lanes, then remap into the selected lobe.
        float ring     = (float(ray - uniformCount) + u.x) / float(localCount);
        uint  selected = 0u;
        while (selected < 7u && ring >= weights[selected])
        {
            ring -= weights[selected];
            ++selected;
        }
        ring      = clamp(ring / max(weights[selected], 1e-20), 0.0, 1.0);
        direction = ProbeLightSample(axes[selected], vec2(ring, u.y), lobes[selected]);
    }
    else if (ray < uniformCount + localCount + learnedCount)
    {
        float select = (float(ray - uniformCount - localCount) + u.x) / float(learnedCount);
        uint  group  = 0u;
        while (group < 2u && select >= learned[group].weight)
        {
            select -= learned[group].weight;
            ++group;
        }
        u.x       = clamp(select / max(learned[group].weight, 1e-20), 0.0, 1.0);
        direction = HybridHenyeyGreensteinSample(learned[group].axis, u, learned[group].concentration);
    }
    else if (ray < uniformCount + localCount + learnedCount + environmentCount)
    {
        // Stratify the environment's row selection across its lanes.
        float row = (float(ray - uniformCount - localCount - learnedCount) + u.y) / float(environmentCount);
        float environmentPdf;
        direction = SampleEnvironment(vec2(u.x, min(row, 0.99999994)), environmentPdf);
    }
    else
    {
        float selection = (float(ray - uniformCount - localCount - learnedCount - environmentCount) + u.x) / float(cdfCount);
        uint  lo = 0u, hi = 64u;
        for (uint step = 0u; step < 6u; ++step)
        {
            uint middle = (lo + hi) / 2u;
            if (selection < ProbeCDFBoundary(index, middle))
            {
                hi = middle;
            }
            else
            {
                lo = middle;
            }
        }
        float a = ProbeCDFBoundary(index, lo), b = ProbeCDFBoundary(index, lo + 1u);
        direction = DecodeGBufferNormal((vec2(lo & 7u, lo >> 3u) + vec2((selection - a) / max(b - a, 1e-20), u.y)) / 8.0);
    }
    uint  bin         = ProbeOctBin(direction);
    float probability = ProbeCDFBoundary(index, bin + 1u) - ProbeCDFBoundary(index, bin);

    float localPdf    = 0.0;
    for (uint i = 0u; i < 8u; ++i)
    {
        localPdf += weights[i] * ProbeLightPDF(axes[i], direction, lobes[i]);
    }
    float learnedPdf = 0.0;
    if (learnedCount != 0u)
    {
        for (uint i = 0u; i < 3u; ++i)
        {
            vec3 chord  = direction - learned[i].axis;
            learnedPdf += learned[i].weight * HybridHenyeyGreensteinPDF(.5 * dot(chord, chord), learned[i].concentration);
        }
    }
    float environmentDensity = environmentCount != 0u ? EnvironmentPDF(direction) : 0.0;
    pdf = (float(uniformCount) * (.5 / HYBRID_PI) + float(localCount) * localPdf + float(environmentCount) * environmentDensity
           + float(cdfCount) * probability * ProbeOctDensity(direction) + float(learnedCount) * learnedPdf)
        / float(ZEN_SCREEN_PROBE_RAYS);
    return vec4(direction, max(pdf, 1e-20));
}
// The light lobes and learned lobes that guide one probe's directions; returns the unnormalized
// light weight.
float ProbeLoadGuides(ScreenProbe   probe,
                      uint          index,
                      out vec3      axes[8],
                      out float     weights[8],
                      out float     lobes[8],
                      out ProbeLobe learned[3],
                      out float     learnedWeight)
{
    uint  lightOffset  = index * ZEN_SCREEN_PROBE_PROPOSAL_WORDS + 32u;
    uvec2 lightIndices = uvec2(probeCDF[lightOffset], probeCDF[lightOffset + 1u]);
    float weightSum    = 0.0;
    for (uint i = 0u; i < 8u; ++i)
    {
        int lightIndex = int((lightIndices[i / 4u] >> ((i % 4u) * 6u)) & 63u) - 1;
        lobes[i]       = kProbeLightAnisotropy;
        axes[i]        = vec3(0);
        weights[i]     = 0.0;
        if (lightIndex >= 0 && lightIndex <= int(sceneUbo.lightInfo.x))
        {
            SceneLight light = lightIndex == int(sceneUbo.lightInfo.x) ? sceneUbo.cameraLight : sceneUbo.lights[lightIndex];
            vec3       delta = light.positionRange.xyz - probe.position.xyz;
            float      dist2 = dot(delta, delta);
            if (dist2 > 1e-12 && light.directionType.w > .5)
            {
                vec4 guide = ProbeReflectedGuide(lightIndex, delta, sqrt(dist2));
                lobes[i]   = guide.w;
                axes[i]    = guide.xyz;
                weights[i] = HybridLuminance(light.colorIntensity.rgb) * light.colorIntensity.w
                           * max(dot(probe.normal.xyz, axes[i]), .02) / dist2;
            }
        }
        weightSum += weights[i];
    }
    for (uint i = 0; i < 8u; ++i)
    {
        weights[i] /= max(weightSum, 1e-20);
    }
    learnedWeight = 0.0;
    for (uint i = 0u; i < 3u; ++i)
    {
        learned[i]     = ProbeReadLobe(probeCDF[lightOffset + 2u + i], probeCDF[lightOffset + 5u + i]);
        learnedWeight += learned[i].weight;
    }
    for (uint i = 0u; i < 3u; ++i)
    {
        learned[i].weight /= max(learnedWeight, 1e-20);
    }
    return weightSum;
}
// Anchored probes keep their sequence when reprojected to another tile.
uint ProbeAnchorSeed(ScreenProbe probe)
{
    uvec3 anchor = floatBitsToUint(probe.position.xyz);
    return HybridHash(anchor.x ^ HybridHash(anchor.y) ^ HybridHash(anchor.z));
}
// Each 64-frame block has an independent scramble and a fixed proposal. Later batches take a
// contiguous power-of-two Sobol interval, which covers each new receiver within this frame.
vec2 ProbeBatchSample(uint seed, uint ray, uint batch, uint batches)
{
    vec2 u;
    if (batch == 0u)
    {
        u = HybridSkySample(uvec2(seed, ray), hybrid.sampling.x & 63u, HybridHash((hybrid.sampling.x >> 6u) ^ 0x83a91u));
    }
    else
    {
        uint count       = batches - 1u;
        uint sampleIndex = (hybrid.sampling.x & 63u) * count + batch - 1u;
        u                = HybridSkySample(uvec2(seed, ray), sampleIndex,
                                           HybridHash((hybrid.sampling.x >> 6u) ^ 0x7c51a3u ^ HybridHash(count)));
    }
    return u;
}
uint ProbePathSeed(uint seed, uint ray, uint batch)
{
    return HybridHash(seed ^ HybridHash(ray) ^ HybridHash(hybrid.sampling.x) ^ HybridHash(batch));
}
#endif // SCREEN_PROBE_ACCUMULATE
#ifdef SCREEN_PROBE_SAMPLE_HITS
// Draw the direction of every recorded sample, one thread per probe ray. Samples below the probe's
// horizon are resolved dark here; the rest wait for the hit trace.
void main()
{
    uint thread = ProbeComputeThread();
    uint index = thread / uint(ZEN_SCREEN_PROBE_RAYS), ray = thread % uint(ZEN_SCREEN_PROBE_RAYS);
    if (index < screenProbe.grid.w)
    {
        ScreenProbe probe = probes[index];
        if (probe.position.w != 0.0)
        {
            uint      batches = ProbeScheduledBatches(index);
            uint      seed    = ProbeAnchorSeed(probe);
            vec3      axes[8];
            float     weights[8], lobes[8];
            ProbeLobe learned[3];
            float     learnedWeight;
            float     weightSum = ProbeLoadGuides(probe, index, axes, weights, lobes, learned, learnedWeight);
            for (uint batch = 0u; batch < batches; ++batch)
            {
                vec2 u         = ProbeBatchSample(seed, ray, batch, batches);
                vec4 sampleRay = ProbeSampleDirection(index, ray, probe.normal.xyz, u, weightSum, axes, weights, lobes, learned,
                                                      learnedWeight > 0.0);
                uint address   = ProbeRecordAddress(ProbeRecordSlot(index, ray, batch));
                probeHits[address + 4u] = packUnorm2x16(EncodeGBufferNormal(sampleRay.xyz));
                probeHits[address + 5u] = floatBitsToUint(sampleRay.w);
                if (dot(probe.normal.xyz, sampleRay.xyz) > 0.0)
                {
                    probeHits[address] = 0u;
                }
                else
                {
                    ProbeWriteResolved(address, vec3(0), false, false);
                }
            }
        }
    }
}
#elif defined(SCREEN_PROBE_TRACE_HITS)
// Trace the first segment of every sample still to be traced with the record-only hit shader.
// Misses are resolved here; hits wait for the shading pass.
void main()
{
    uint slot = gl_LaunchIDEXT.y * uint(ZEN_SCREEN_PROBE_RAYS) + gl_LaunchIDEXT.x;
    uint index, ray, batch;
    if (ProbeSlotSample(slot, index, ray, batch))
    {
        uint address = ProbeRecordAddress(slot);
        if (probeHits[address] == 0u)
        {
            ScreenProbe probe     = probes[index];
            vec3        direction = ProbeHitSample(address).xyz;
            rayShading            = uvec3(0u, 3u, 2u);
            HybridRayResult hit =
                HardwareTraceRay(HybridTraceOrigin(probe.position.xyz, probe.normal.xyz, 0.0), direction, 1e20, true);
            if (hit.hit)
            {
                // The record-only closest-hit shader returns the hit's indices and barycentrics.
                uvec3 primitive         = floatBitsToUint(hit.position);
                probeHits[address]      = kHitRecorded | (primitive.y << 8u);
                probeHits[address + 1u] = primitive.x;
                probeHits[address + 2u] = primitive.z;
                probeHits[address + 3u] = packUnorm2x16(hit.normal.xy);
            }
            else
            {
                ProbeWriteResolved(address, NativeMissRadiance(direction, hit.valid), false, hit.valid);
            }
        }
    }
}
#elif defined(SCREEN_PROBE_SHADE_HITS)
// Shade recorded first hits as the native closest-hit shader would: material, interpolated vertex
// normal and the world cache. A cold hit receives one sampled direct-light evaluation.
void main()
{
    uint slot = ProbeComputeThread();
    uint index, ray, batch;
    if (ProbeSlotSample(slot, index, ray, batch) && (probeHits[ProbeRecordAddress(slot)] & 0xffu) == kHitRecorded)
    {
        uint            address   = ProbeRecordAddress(slot);
        uint            flags     = probeHits[address];
        vec4            sampleRay = ProbeHitSample(address);
        uvec4           triangle  = RayTriangle(probeHits[address + 1u], flags >> 8u, probeHits[address + 2u]);
        HybridRayResult hit       = MakeHybridRayResult(true, vec3(0), vec3(0), ivec3(-1), true);
        hit.triangle              = triangle;
        hit.barycentrics          = unpackUnorm2x16(probeHits[address + 3u]);
        RaySurfaceGeometry(triangle, RayVertexIndices(triangle), hit.barycentrics, hit.position, hit.normal);
        if (dot(hit.normal, sampleRay.xyz) >= 0.0)
        {
            hit.normal = -hit.normal;
        }
        Material material = LoadRayHitMaterial(triangle.z);
        vec3     emission;
        vec3     reflectance = RayHitReflectance(hit, material, emission);
        // Probe rays carry ray flag 1, so their hits shade with the interpolated vertex normal.
        bool flipped;
        vec3 normal = RayHitVertexNormal(hit, vec3(1.0 - hit.barycentrics.x - hit.barycentrics.y, hit.barycentrics), flipped);
        bool found, usable;
        vec3 diffuse = WorldLookup(hit.position, hit.normal, normal, false, vec2(0), found, usable);
        uint seed    = ProbePathSeed(ProbeAnchorSeed(probes[index]), ray, batch);
        if (!usable)
        {
            diffuse = SampledHitDiffuse(hit, normal, seed, false, false);
        }
        WorldCountProbe(seed, hit.position, hit.normal, found, usable);
        ProbeWriteResolved(address, emission + reflectance * diffuse, true, true);
    }
}
#else
void main()
{
    uint index = ProbeGroupIndex(), ray = gl_LocalInvocationID.x;
    if (index < screenProbe.grid.w)
    {
        ScreenProbe probe        = probes[index];
        mat4x3      moments[2]   = {mat4x3(0), mat4x3(0)};
        mat3        basis        = HybridBasis(probe.normal.xyz);
        uint        batches      = ProbeScheduledBatches(index);
#    ifndef SCREEN_PROBE_ACCUMULATE
        uint        specularRays = hybrid.rejection.y != 0.0 ? uint(ZEN_SCREEN_PROBE_SPECULAR_RAYS) : 0u;
        // Only the untraced GGX lanes need clearing. The diffuse slots are fully overwritten below.
        uint firstUnused = probe.position.w != 0.0 ? batches * uint(ZEN_SCREEN_PROBE_RAYS) + ray : ray;
        for (uint sampleIndex = firstUnused; sampleIndex < specularRays; sampleIndex += uint(ZEN_SCREEN_PROBE_RAYS))
        {
            uint address             = ProbeSpecularRayAddress(index, sampleIndex);
            screenRays[address]      = vec2(0);
            screenRays[address + 1u] = vec2(0);
            screenRays[address + 2u] = vec2(0);
        }
#    endif
        if (probe.position.w != 0.0)
        {
#    ifdef HYBRID_FIELD_TRAVERSAL
            uint      seed = ProbeAnchorSeed(probe);
            vec3      axes[8];
            float     weights[8], lobes[8], learnedWeight;
            ProbeLobe learned[3];
            float     weightSum = ProbeLoadGuides(probe, index, axes, weights, lobes, learned, learnedWeight);
#    endif
            for (uint batch = 0u; batch < batches; ++batch)
            {
                vec4 sampleRay;
                vec3 incoming = vec3(0);
                bool hit = false, valid = false;
#    ifdef SCREEN_PROBE_ACCUMULATE
                uint address = ProbeRecordAddress(ProbeRecordSlot(index, ray, batch));
                uint flags   = probeHits[address];
                sampleRay    = ProbeHitSample(address);
                incoming = ProbeHitRadiance(address);
                hit      = (flags & kHitSurface) != 0u;
                valid    = (flags & kHitValid) != 0u;
#    else
                vec2 u    = ProbeBatchSample(seed, ray, batch, batches);
                sampleRay = ProbeSampleDirection(index, ray, probe.normal.xyz, u, weightSum, axes, weights, lobes, learned,
                                                 learnedWeight > 0.0);
                if (dot(probe.normal.xyz, sampleRay.xyz) > 0.0)
                {
                    incoming = NativePathRadiance(probe.position.xyz, probe.normal.xyz, sampleRay.xyz,
                                                  ProbePathSeed(seed, ray, batch), hit, valid);
                }
#    endif
                float cosine        = max(dot(probe.normal.xyz, sampleRay.xyz), 0.0);
                uint  signal        = valid && !hit ? 0u : 1u;
                vec3  weighted      = incoming / sampleRay.w;
                moments[signal][0] += weighted;
                moments[signal][1] += weighted * dot(sampleRay.xyz, basis[0]);
                moments[signal][2] += weighted * dot(sampleRay.xyz, basis[1]);
                moments[signal][3] += weighted * cosine;
#    ifndef SCREEN_PROBE_ACCUMULATE
                uint sampleIndex    = ray + batch * uint(ZEN_SCREEN_PROBE_RAYS);
                if (sampleIndex < specularRays)
                {
                    uint address             = ProbeSpecularRayAddress(index, sampleIndex);
                    screenRays[address]      = incoming.rg;
                    screenRays[address + 1u] = vec2(incoming.b, signal == 0u ? -sampleRay.w : sampleRay.w);
                    screenRays[address + 2u] = EncodeGBufferNormal(sampleRay.xyz);
                }
#    endif
            }
        }
        uint address = (index * uint(ZEN_SCREEN_PROBE_RAYS) + ray) * 4u;
        for (uint signal = 0u; signal < 2u; ++signal)
        {
            mat4x3 m                               = moments[signal] / float(batches);
            screenRays[address + 2u * signal]      = uintBitsToFloat(uvec2(ProbePackRGBE(m[0]), HybridPackSignedRGBE(m[1])));
            screenRays[address + 2u * signal + 1u] = uintBitsToFloat(uvec2(HybridPackSignedRGBE(m[2]), ProbePackRGBE(m[3])));
        }
    }
}
#endif
