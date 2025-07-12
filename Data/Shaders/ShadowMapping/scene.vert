#version 450

layout (location = 0) in vec4 inPos;
layout (location = 1) in vec4 inNormal;
layout (location = 2) in vec4 inTangent;
layout (location = 3) in vec2 inUV0;
layout (location = 4) in vec2 inUV1;
layout (location = 5) in vec4 inJoint0;
layout (location = 6) in vec4 inWeight0;
layout (location = 7) in vec4 inColor;


layout (set = 0, binding = 0) uniform UBO
{
	mat4 projection;
	mat4 view;
	mat4 model;
	mat4 lightSpace;
	vec3 lightPos;
	float zNear;
	float zFar;
} ubo;

layout (location = 0) out vec3 outNormal;
layout (location = 1) out vec3 outColor;
layout (location = 2) out vec3 outViewVec;
layout (location = 3) out vec3 outLightVec;
layout (location = 4) out vec4 outShadowCoord;

const mat4 biasMat = mat4( 
	0.5, 0.0, 0.0, 0.0,
	0.0, 0.5, 0.0, 0.0,
	0.0, 0.0, 1.0, 0.0,
	0.5, 0.5, 0.0, 1.0 );

void main() 
{
	outColor = inColor.rgb;
	outNormal = inNormal.xyz;

	gl_Position = ubo.projection * ubo.view * ubo.model * vec4(inPos.xyz, 1.0);
	
    vec4 pos = ubo.model * vec4(inPos.xyz, 1.0);
    outNormal = mat3(ubo.model) * inNormal.xyz;
    outLightVec = normalize(ubo.lightPos - inPos.xyz);
    outViewVec = -pos.xyz;			

	outShadowCoord = ( biasMat * ubo.lightSpace * ubo.model ) * vec4(inPos.xyz, 1.0);
}

