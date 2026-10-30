#version 450

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;
layout(set = 1, binding = 0) uniform sampler2D sceneColor;
layout(set = 1, binding = 1, std140) uniform uPresentation
{
    vec4 rectangle;
}
presentation;

void main()
{
    vec2 uv = (inUV - presentation.rectangle.xy) / presentation.rectangle.zw;
    // Match the editor's UNORM presentation of already SDR encoded scene color.
    outColor = any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0))) ? vec4(0.055, 0.06, 0.075, 1.0)
                                                                               : texture(sceneColor, uv);
}
