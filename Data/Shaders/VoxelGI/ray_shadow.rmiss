#version 460
#extension GL_EXT_ray_tracing : require
layout(location = 1) rayPayloadInEXT uint shadowed;
void main()
{
    // The low bit is visibility; diagnostic any-hit shaders retain a candidate count above it.
    shadowed &= ~1u;
}
