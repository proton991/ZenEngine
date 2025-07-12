#version 450

layout (location = 0) in vec4 inPos;
layout (location = 1) in vec4 inNormal;
layout (location = 2) in vec4 inTangent;
layout (location = 3) in vec2 inUV0;
layout (location = 4) in vec2 inUV1;
layout (location = 5) in vec4 inJoint0;
layout (location = 6) in vec4 inWeight0;
layout (location = 7) in vec4 inColor;

layout (set = 0, binding = 0) uniform uTransformData
{
	mat4 model;
	mat4 view;
	mat4 projection;
} ubo;

layout(std140, set=1, binding = 0) readonly buffer InstanceBuffer {
   vec4 positions[];
};

// output instance index to fragment shader as a flat value
layout (location = 1) flat out int instanceIndex;

void main() 
{
    // Transform position into world space
	vec4 world_pos =  ubo.model * vec4(inPos.xyz, 1.0) + vec4(positions[gl_InstanceIndex]);

    // Transform world position into clip space
	gl_Position = ubo.projection * ubo.view * world_pos;

	instanceIndex = gl_InstanceIndex;
}