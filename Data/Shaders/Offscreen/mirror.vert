#version 450


layout (location = 0) in vec4 inPos;
layout (location = 1) in vec4 inNormal;
layout (location = 2) in vec4 inTangent;
layout (location = 3) in vec2 inUV0;
layout (location = 4) in vec2 inUV1;
layout (location = 5) in vec4 inJoint0;
layout (location = 6) in vec4 inWeight0;
layout (location = 7) in vec4 inColor;



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


layout (location = 0) out vec4 outPos;

void main() 
{
	outPos = uProjViewMatrix * uMddel * vec4(inPos.xyz, 1.0);
	gl_Position = outPos;		
}
