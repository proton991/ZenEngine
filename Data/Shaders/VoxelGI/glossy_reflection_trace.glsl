#ifndef GLOSSY_TRACE_UNCOVERED
#    define GLOSSY_RAW_WRITE
#endif
#include "glossy_reflection.glsl"
#ifdef HYBRID_FIELD_TRAVERSAL
#    define NATIVE_FIELD_ENDPOINT
#    define HYBRID_NO_GUIDE
#    include "native_field.glsl"
layout(local_size_x = 8, local_size_y = 8) in;
#else
#    include "hybrid_provider.glsl"
#    include "world_surface_edits.glsl"
#endif
vec4 GlossyTraceRay(GIDenoiseGuide guide, vec3 direction, uint seed, bool mirror, out float distance, out float change)
{
    vec4 value = vec4(0);
    distance   = -1.0;
    change     = 0.0;
    if (dot(direction, guide.geometric) > 0.0)
    {
#ifdef GLOSSY_TRACE_UNCOVERED
        bool closest = hybrid.reflection.x != 0u;
#else
        // The glossy history also needs hit distance for its spatial filter.
        const bool closest = true;
#endif
#ifdef HYBRID_FIELD_TRAVERSAL
        HybridRayResult hit      = NativeTraceRay(guide.position, guide.geometric, direction, 1e20, closest);
        vec3            radiance = hybrid.reflection.x != 0u ? NativeFieldRadiance(hit, seed) : vec3(0);
#else
        rayShading          = uvec3(seed, mirror ? 4u : 0u, 3u);
        hitPayload.radiance = vec3(0);
        HybridRayResult hit =
            HardwareTraceRay(HybridTraceOrigin(guide.position, guide.geometric, 0.0), direction, 1e20, closest);
        vec3 radiance = hitPayload.radiance;
        if (hybrid.reflection.w == 2u)
        {
            change = closest && hit.hit ? uintBitsToFloat(hitPayload.seed)
                                        : float(WorldSurfaceChanged(guide.position, direction, 1e20, 0xffffffffu, 1u));
        }
#endif
        value    = vec4(hit.hit && hybrid.reflection.x != 0u ? radiance : vec3(0), float(hit.valid && !hit.hit));
        distance = hit.hit ? length(hit.position - guide.position) : -1.0;
    }
    return value;
}
#ifdef GLOSSY_TRACE_UNCOVERED
layout(set = 1, binding = 90, rgba16f) uniform image2D probeSpecular;
#endif
void main()
{
#ifdef HYBRID_FIELD_TRAVERSAL
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
#else
    ivec2 p = ivec2(gl_LaunchIDEXT.xy);
#endif
#ifdef GLOSSY_TRACE_UNCOVERED
    if (all(lessThan(p, imageSize(probeSpecular))) && imageLoad(probeSpecular, p).a < 0.0)
    {
        GIDenoiseGuide guide = DenoiseGuide(p);
        // There is no filtered history for this receiver. Use its mirror direction
        // and a stable endpoint sample so a stationary edge stays stationary.
        vec3  direction = reflect(-DenoiseView(guide.position), guide.normal);
        float distance, change;
        vec4  value  = GlossyTraceRay(guide, direction, HybridHash(HybridMorton(uvec2(p))), guide.roughness < glossy.control.w,
                                      distance, change);
        value.rgb   *= gi.volume.w;
        imageStore(probeSpecular, p, value);
    }
#else
    if (GlossyInView(p))
    {
        GIDenoiseGuide guide    = DenoiseGuide(GlossyPixel(p));
        vec4           value    = vec4(0, 0, 0, 1);
        float          distance = 0.0;
        float          change   = 0.0;
        if (GlossyReceiver(guide))
        {
            vec3  view    = DenoiseView(guide.position);
            bool  mirror  = guide.roughness < glossy.control.w;
            uint  samples = mirror ? 1u : 3u;
            float hits    = 0.0;
            value         = vec4(0);
            for (uint sampleIndex = 0u; sampleIndex < samples; ++sampleIndex)
            {
                uint sequence  = mirror ? hybrid.sampling.x : hybrid.sampling.x * samples + sampleIndex;
                vec3 direction = mirror
                                   ? reflect(-view, guide.normal)
                                   : HybridSpecularDirection(guide.normal, view, guide.roughness, uvec2(p), sequence, 0x91ac7u);
                uint seed      = HybridHash(HybridMorton(uvec2(p)) ^ HybridHash(sequence));
                float hitDistance, hitChange;
                value    += GlossyTraceRay(guide, direction, seed, mirror, hitDistance, hitChange);
                change   += hitChange;
                distance += max(hitDistance, 0.0);
                hits     += float(hitDistance >= 0.0);
            }
            // Each GGX sample carries an equal share of the estimate, and of the change it observed.
            value    /= float(samples);
            change   /= float(samples);
            distance /= max(hits, 1.0);
        }
        uint index                 = GlossyIndex(p);
        glossyRaw[index * 2u]      = value;
        glossyRaw[index * 2u + 1u] = vec4(distance, change, 0, 0);
    }
#endif
}
