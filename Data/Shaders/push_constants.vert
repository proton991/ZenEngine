#version 460

layout (location = 0) in vec3 inPos;
layout (location = 1) in vec3 inNormal;
layout (location = 2) in vec4 inTangent;
layout (location = 3) in vec2 inUV0;
layout (location = 4) in vec2 inUV1;
layout (location = 5) in vec4 inJoint0;
layout (location = 6) in vec4 inWeight0;
layout (location = 7) in vec4 inColor;


#define lightCount 6

layout(set = 0, binding = 0) uniform uCameraData
{
	mat4 uProjViewMatrix;
	mat4 uProj;
	mat4 uView;
};

layout (set = 0, binding = 1) uniform uModelData
{
	mat4 uModel;
} ;

layout(push_constant) uniform PushConsts {
	vec4 color;
	vec4 position;
} pushConsts;

layout (location = 0) out vec3 outColor;

void main() 
{
	outColor = pushConsts.color.rgb;
	vec3 locPos = vec3(uModel * vec4(inPos, 1.0));
	vec3 worldPos = locPos + pushConsts.position.xyz;
	gl_Position =  uProj * uView * vec4(worldPos, 1.0);
}