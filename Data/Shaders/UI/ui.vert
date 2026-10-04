#version 450

layout(location = 0) in vec2 inPosition;
layout(location = 1) in vec2 inUV;
// RHI reflection uses tightly packed attributes: uint keeps UIVertex's four-byte RGBA8
// color without backend-specific vertex layout overrides.
layout(location = 2) in uint inColor;

// Shared with ui.frag, which reads the texture slot.
layout(push_constant) uniform UIDrawConstants
{
    vec2 scale;
    vec2 translate;
    uint textureSlot;
} constants;

layout(location = 0) out vec2 outUV;
layout(location = 1) out vec4 outColor;

void main()
{
    outUV = inUV;
    outColor = unpackUnorm4x8(inColor);
    gl_Position = vec4(inPosition * constants.scale + constants.translate, 0.0, 1.0);
}
