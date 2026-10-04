#version 450
#extension GL_GOOGLE_include_directive : require
#include "debug_view.glsl"
layout(set=1,binding=0) uniform sampler2DArray sourceImage;
void main()
{
    float value = texture(sourceImage, vec3(inUV, float(debug.selection.y))).r;
    // Shadow maps store normalized radial/orthographic distance, not device Z.
    outColor = vec4(vec3(clamp((value - debug.range.x) / (debug.range.y - debug.range.x), 0.0, 1.0)), 1);
}
