#version 460

//#extension GL_EXT_nonuniform_qualifier : enable

/* Copyright (c) 2019, Arm Limited and Contributors
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

//precision mediump float;

layout(location = 1) in vec3 vColor;
layout(location = 2) in vec2 vUV;

layout(location = 0) out vec4 outColor;

layout(set = 1, binding = 0) uniform sampler2D uTexture;

void main()
{
    outColor = vec4(vColor, 1.0);
}
