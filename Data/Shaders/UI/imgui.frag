#version 450

layout(set = 1, binding = 0) uniform sampler2D uFont;
layout(location = 0) in vec2 inUV;
layout(location = 1) in vec4 inColor;
layout(location = 0) out vec4 outColor;

void main()
{
    // Existing viewport composition stores SDR encoded color in a UNORM target.
    // UI colors are authored in that same space; the atlas contributes coverage.
    outColor = inColor * texture(uFont, inUV);
}
