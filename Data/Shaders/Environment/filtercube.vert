/* Copyright (c) 2018-2023, Sascha Willems
 *
 * SPDX-License-Identifier: MIT
 *
 */

#version 450
#extension GL_GOOGLE_include_directive : require

#include "../Common/bindless_heap.glsl"

layout (location = 0) in vec3 inPos;

layout(push_constant) uniform PushConsts {
	layout (offset = 0) mat4 mvp;
} pushConsts;

layout (location = 0) out vec3 outUVW;

void main() 
{
	// trick to solve upside down
	outUVW = vec3(inPos.x, -inPos.y, inPos.z);
	gl_Position = pushConsts.mvp * vec4(inPos.xyz, 1.0);
}
