/* Copyright (c) 2018-2023, Sascha Willems
 *
 * SPDX-License-Identifier: MIT
 *
 */

#version 450

layout (location = 0) in vec3 inPos;

layout(set = 0, binding = 0) uniform uCameraData
{
	mat4 uProjViewMatrix;
	mat4 uProjMatrix;
	mat4 uViewMatrix;
};

layout (location = 0) out vec3 outUVW;

void main() 
{
	// trick to solve upside down
	outUVW = vec3(inPos.x, -inPos.y, inPos.z);
	mat4 viewMat = mat4(mat3(uViewMatrix));
	vec4 pos = uProjMatrix * viewMat * vec4(inPos, 1.0);
	gl_Position = pos.xyww; // Force depth to 1.0 to avoid clipping
}
