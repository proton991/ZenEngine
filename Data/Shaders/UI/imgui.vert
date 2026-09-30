#version 450

layout(location = 0) in vec2 inPosition;
layout(location = 1) in vec2 inUV;
// RHI reflection uses tightly packed attributes: uint preserves ImDrawVert's
// four-byte RGBA color without adding backend-specific vertex layout overrides.
layout(location = 2) in uint inColor;

layout(push_constant) uniform UIProjection
{
    vec2 scale;
    vec2 translate;
} projection;

layout(location = 0) out vec2 outUV;
layout(location = 1) out vec4 outColor;

void main()
{
    outUV = inUV;
    outColor = unpackUnorm4x8(inColor);
    gl_Position = vec4(inPosition * projection.scale + projection.translate, 0.0, 1.0);
}
