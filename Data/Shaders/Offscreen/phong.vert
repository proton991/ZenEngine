#version 450

layout (location = 0) in vec4 inPos;
layout (location = 1) in vec4 inNormal;
layout (location = 2) in vec4 inTangent;
layout (location = 3) in vec2 inUV0;
layout (location = 4) in vec2 inUV1;
layout (location = 5) in vec4 inJoint0;
layout (location = 6) in vec4 inWeight0;
layout (location = 7) in vec4 inColor;


//layout (binding = 0) uniform UBO 
//{
//	mat4 projection;
//	mat4 view;
//	mat4 model;
//	vec4 lightPos;
//} ubo;

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
layout (location = 2) out vec3 outEyePos;
layout (location = 3) out vec3 outLightVec;

void main() 
{
	outNormal = inNormal.xyz;
	outColor = inColor.rgb;
	gl_Position = uProj * uView * uMddel * vec4(inPos.xyz, 1.0);
	outEyePos = vec3(uView * uMddel * vec4(inPos.xyz, 1.0));
	outLightVec = normalize(uLightPos.xyz - outEyePos);

	// Clip against reflection plane
	vec4 clipPlane = vec4(0.0, -1.0, 0.0, 1.5);	
	gl_ClipDistance[0] = dot(vec4(inPos.xyz, 1.0), clipPlane);
}
