#version 450

layout (location = 0) in vec3 inPos;
layout (location = 1) in vec3 inNormal;
layout (location = 2) in vec2 inUV0;
layout (location = 3) in vec2 inUV1;
layout (location = 4) in vec4 inJoint0;
layout (location = 5) in vec4 inWeight0;
layout (location = 6) in vec4 inColor;

layout(set = 0, binding = 0) uniform uCameraData
{
	mat4 uProjViewMatrix;
	mat4 uProj;
	mat4 uView;
};

layout (set = 0, binding = 1) uniform uSceneData
{
	vec4 uLightPos;
	mat4 uMddel;
};

layout (location = 0) out vec3 outNormal;
layout (location = 1) out vec3 outColor;
layout (location = 2) out vec2 outUV;
layout (location = 3) out vec3 outViewVec;
layout (location = 4) out vec3 outLightVec;

out gl_PerVertex
{
	vec4 gl_Position;
};

void main() 
{
	mat4 modelView = uView * uMddel;
	outNormal = inNormal;
	outColor = inColor.rgb;
	outUV = inUV0;
	gl_Position = uProj * modelView * vec4(inPos.xyz, 1.0);
	
	vec4 pos = modelView * vec4(inPos, 1.0);
	outNormal = mat3(modelView) * inNormal;
	vec3 lPos = mat3(modelView) * uLightPos.xyz;
	outLightVec = lPos - pos.xyz;
	outViewVec = -pos.xyz;
}