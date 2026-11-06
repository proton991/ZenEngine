#ifndef ZEN_GBUFFER_DRAW_GLSL
#define ZEN_GBUFFER_DRAW_GLSL

// Matches GBufferDrawData on the CPU (std430, 64 bytes). Commands use the
// record index as firstInstance so every pipeline group shares this table.
struct GBufferDrawData
{
    vec4  boundsMin;
    vec4  boundsMax;
    uvec4 geometry;
    uvec4 indices;
};
layout(std430, set = 1, binding = 5) readonly buffer GBufferDrawBuffer
{
    GBufferDrawData gbufferDraws[];
};

#endif
