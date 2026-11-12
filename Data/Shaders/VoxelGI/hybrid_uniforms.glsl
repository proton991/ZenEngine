#ifndef ZEN_HYBRID_UNIFORMS
#define ZEN_HYBRID_UNIFORMS
#include "Graphics/Shared/HybridGI.h"
layout(std140, set = 1, binding = 0) uniform uHybridData
{
    mat4  inverseViewProjection;
    vec4  worldOrigin;
    mat4  previousProjectionView;
    vec4  previousViewPosition;
    vec4  previousViewDirection;
    uvec4 sampling;
    uvec4 provider;
    vec4  rejection;
    uvec4 bounce;
    uvec4 reflection;
    uvec4 resolution;
    uvec4 reservoir;
}
hybrid;
#endif
