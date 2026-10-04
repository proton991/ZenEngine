#version 450
#extension GL_EXT_nonuniform_qualifier : require

// Must match kUITextureSlots in UIRenderer.cpp. Unused slots stay unbound.
const uint kTextureSlots = 16;

layout(set = 1, binding = 0) uniform texture2D uTextures[kTextureSlots];
layout(set = 1, binding = 1) uniform sampler uSamplers[kTextureSlots];

layout(push_constant) uniform UIDrawConstants
{
    vec2 scale;
    vec2 translate;
    uint textureSlot;
} constants;

layout(location = 0) in vec2 inUV;
layout(location = 1) in vec4 inColor;
layout(location = 0) out vec4 outColor;

void main()
{
    // Viewport composition stores SDR encoded color in a UNORM target. UI colors and
    // images are authored in that space; the font atlas contributes coverage.
    const uint slot = constants.textureSlot;

    outColor = inColor * texture(sampler2D(uTextures[nonuniformEXT(slot)], uSamplers[nonuniformEXT(slot)]), inUV);
}
