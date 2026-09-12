#version 450
#extension GL_EXT_nonuniform_qualifier : require
layout(set = 0, binding = 0) uniform texture2D textures[];
layout(set = 0, binding = 2) uniform sampler samplers[];
layout(push_constant) uniform Constants { uint index; } constants;
layout(location = 0) out vec4 color;
void main()
{
    color = texture(sampler2D(textures[nonuniformEXT(constants.index)],
        samplers[nonuniformEXT(constants.index)]), vec2(0.5));
}
