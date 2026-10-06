#version 450
#extension GL_GOOGLE_include_directive : require

#include "../Common/bindless_heap.glsl"
#include "../Common/linear_to_srgb.glsl"

layout(location = 0) in vec4 voxelColor;

layout(location = 0) out vec4 fragColor;

void main()
{
	fragColor = vec4(LinearToSRGB(voxelColor.rgb), 1.0);
}
