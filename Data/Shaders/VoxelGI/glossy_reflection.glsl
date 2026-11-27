#ifndef ZEN_GLOSSY_REFLECTION
#define ZEN_GLOSSY_REFLECTION
#ifndef GLOSSY_CLEAR
#    define DENOISE_GUIDE_ONLY
#    include "gi_denoise_common.glsl"
#    include "hybrid_sampling.glsl"
#endif
#include "Graphics/Shared/ScreenProbeGI.h"
layout(std140, set = 1, binding = 100) uniform uGlossyData
{
    uvec4 extent;  // glossy grid extent, history valid, filter pass (1 horizontal/repair, 2 vertical)
    vec4  control; // temporal, spatial, glossy threshold, mirror threshold
}
glossy;
struct GlossyHistory
{
    uvec4 position;
    uvec4 normal;
    vec4  slow;
    vec4  fast;
    vec4  moments; // second luminance moment, hit distance, age, first moment
};
#if defined(GLOSSY_CLAMP)
#    define GLOSSY_ACCESS
#elif defined(GLOSSY_HISTORY_WRITE)
#    define GLOSSY_ACCESS writeonly
#else
#    define GLOSSY_ACCESS readonly
#endif
layout(std430, set = 1, binding = 101) GLOSSY_ACCESS buffer GlossyCurrent
{
    GlossyHistory glossyCurrent[];
};
#ifdef GLOSSY_CLEAR
#    define GLOSSY_PREVIOUS writeonly
#else
#    define GLOSSY_PREVIOUS readonly
#endif
layout(std430, set = 1, binding = 102) GLOSSY_PREVIOUS buffer GlossyPrevious
{
    GlossyHistory glossyPrevious[];
};
#ifdef GLOSSY_RAW_WRITE
#    define GLOSSY_RAW_ACCESS writeonly
#else
#    define GLOSSY_RAW_ACCESS readonly
#endif
#if defined(GLOSSY_RAW_WRITE) || defined(GLOSSY_RAW_READ)
layout(std430, set = 1, binding = 103) GLOSSY_RAW_ACCESS buffer GlossyRaw
{
    vec4 glossyRaw[];
};
#endif
#ifdef GLOSSY_FILTER_READ
layout(std430, set = 1, binding = 104) readonly buffer GlossyFiltered
{
    vec4 glossyFiltered[];
};
#endif
#ifdef GLOSSY_FILTER_WRITE
layout(std430, set = 1, binding = 105) writeonly buffer GlossyOutput
{
    vec4 glossyOutput[];
};
#endif
GlossyHistory GlossyEmpty()
{
    return GlossyHistory(uvec4(0), uvec4(0), vec4(0, 0, 0, 1), vec4(0, 0, 0, 1), vec4(0));
}
#ifndef GLOSSY_CLEAR
GIDenoiseGuide GlossyHistoryGuide(GlossyHistory history)
{
    // Every R8 UNORM receiver roughness is exactly representable in UNORM16.
    uvec4 normal = history.normal;
    normal.z     = floatBitsToUint(unpackUnorm2x16(normal.z).x);
    return DenoiseUnpackGuide(history.position, normal);
}
bool GlossyInView(ivec2 p)
{
    return all(greaterThanEqual(p, ivec2(0))) && all(lessThan(p, ivec2(glossy.extent.xy)));
}
uint GlossyIndex(ivec2 p)
{
    return uint(p.x) + uint(p.y) * glossy.extent.x;
}
ivec2 GlossyPixel(ivec2 p)
{
    return min(p * ZEN_GLOSSY_RECEIVER_SPACING + ZEN_GLOSSY_RECEIVER_SPACING / 2, ivec2(hybrid.resolution.xy) - 1);
}
// Invert the receiver lattice, including the shortened interval at a partial edge tile.
vec2 GlossyCoordinate(vec2 pixel)
{
    ivec2 last     = ivec2(glossy.extent.xy) - 1;
    vec2  previous = vec2(GlossyPixel(last - 1));
    vec2  regular  = (pixel - float(ZEN_GLOSSY_RECEIVER_SPACING / 2)) / float(ZEN_GLOSSY_RECEIVER_SPACING);
    vec2  edge     = vec2(last - 1) + (pixel - previous) / max(vec2(GlossyPixel(last)) - previous, vec2(1));
    return mix(regular, edge, greaterThan(pixel, previous));
}
bool GlossyReceiver(GIDenoiseGuide guide)
{
    return guide.identity != 0u && guide.roughness < glossy.control.z && hybrid.rejection.y > 0.0;
}
bool GlossyDeterministicMirror(GIDenoiseGuide guide)
{
    // HW mirrors sum analytic lights exactly. Only secondary environment sampling adds noise.
    return guide.roughness < glossy.control.w && hybrid.provider.x != 0u
        && (sceneUbo.environment.x == 0.0 || sceneUbo.environment.z == 0.0);
}
float GlossyLuma(vec3 value)
{
    return dot(value, vec3(.2126, .7152, .0722));
}
#endif
#endif
