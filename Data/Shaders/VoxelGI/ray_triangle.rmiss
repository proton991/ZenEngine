#version 460
#extension GL_EXT_ray_tracing : require
layout(location = 0) rayPayloadInEXT uvec4 occluderTriangle;
void main()
{
    occluderTriangle = uvec4(0xffffffffu);
}
