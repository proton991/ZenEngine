#version 460

#extension GL_EXT_nonuniform_qualifier : enable

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
struct Material
{
    int bcTexIndex;
    int mrTexIndex;
    int normalTexIndex;
    int aoTexIndex;
    int emissiveTexIndex;
    int   bcTexSet;
    int   mrTexSet;
    int   normalTexSet;
    int   aoTexSet;
    int   emissiveTexSet;
    float metallicFactor;
    float roughnessFactor;
    vec4 baseColorFactor;
    vec4 emissiveFactor;
};

layout (location = 0) in vec3 vPos;
layout (location = 1) in vec3 vNormal;
layout (location = 2) in vec2 vUV0;
layout (location = 3) in vec2 vUV1;
layout (location = 4) in vec4 vColor;

layout (location = 0) out vec4 outColor;

#define MATERIAL_TEXTURE_COUNT 512

layout (set = 1, binding = 0) uniform sampler2D uTextureArray[1024];

layout (std140, set = 1, binding = 1) uniform uMaterialData {
    Material uMaterialArray[64];
};

layout (push_constant) uniform uNodePushConstant
{
    uint uSubMeshIndex;
    uint uMaterialIndex;
};


void main()
{
    Material mat = uMaterialArray[uMaterialIndex];
    vec3 baseColor = texture(uTextureArray[mat.bcTexIndex], vUV0).rgb;
    outColor = vec4(baseColor, 1.0);
}
