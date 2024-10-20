#version 450

layout (location = 0) in vec4 inPos;
layout (location = 1) in vec3 inNormal;
layout (location = 2) in vec3 inColor;

layout(set = 0, binding = 0) uniform uCameraData
{
	mat4 uProjViewMatrix;
	mat4 uProj;
	mat4 uView;
};

layout (set = 0, binding = 1) uniform uSceneData
{
	vec4 uLightPos;
	mat4 uModel[3];
};

layout (location = 0) out vec3 outNormal;
layout (location = 1) out vec3 outColor;
layout (location = 2) out vec3 outEyePos;
layout (location = 3) out vec3 outLightVec;

void main() 
{
	outNormal = normalize(mat3x3(uModel[gl_InstanceIndex]) * inNormal);
	outColor = inColor;
	mat4 modelView = uView * uModel[gl_InstanceIndex];
	vec4 pos = modelView * inPos;	
	outEyePos = vec3(modelView * pos);
	vec4 lightPos = vec4(uLightPos.xyz, 1.0) * modelView;
	outLightVec = normalize(lightPos.xyz - outEyePos);
	gl_Position = uProj * pos;
}