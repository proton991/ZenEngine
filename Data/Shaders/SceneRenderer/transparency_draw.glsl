#ifndef ZEN_TRANSPARENCY_DRAW
#define ZEN_TRANSPARENCY_DRAW
struct ForwardTriangle
{
    vec4  center;
    uvec4 identity; // Node, material, source first index, culling cluster.
};
layout(std430, set = 1, binding = 5) readonly buffer ForwardTriangles
{
    ForwardTriangle forwardTriangles[];
};
#endif
