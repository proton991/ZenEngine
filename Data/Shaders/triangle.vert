#version 460
/* Copyright (c) 2019-2023, Arm Limited and Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 the "License";
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

precision mediump float;

layout(location = 0) out vec3 outColor;
layout(location = 2) out vec2 outUV;

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inColor;
layout(location = 2) in vec2 inUV;

layout(set = 0, binding = 0) uniform uCameraData
{
    mat4 uProjViewMatrix;
    mat4 uProj;
    mat4 uView;
};

void main()
{
    gl_Position = uProjViewMatrix * vec4(inPos, 1.0);
    outColor = inColor;
    outUV = inUV;
}
