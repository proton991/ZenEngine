#version 460

#extension GL_EXT_nonuniform_qualifier: enable

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
    int bcTexSet;
    int mrTexSet;
    int normalTexSet;
    int aoTexSet;
    int emissiveTexSet;
    float metallicFactor;
    float roughnessFactor;
    vec4 baseColorFactor;
    vec4 emissiveFactor;
};

struct LightInfo {
    vec4 position;  // (x, y, z, lightType)
    vec4 color; // (r, g, b, intensity)
    vec4 direction; // only for direction lights

};

layout (location = 0) in vec3 vPos;
layout (location = 1) in vec3 vNormal;
layout (location = 2) in vec2 vUV0;
layout (location = 3) in vec2 vUV1;
layout (location = 4) in vec4 vColor;

layout (location = 0) out vec4 outColor;
layout (set = 0, binding = 0) uniform uCameraData
{
    mat4 uProjViewMatrix;
    vec4 uCameraPos;
};

layout (set = 1, binding = 0) uniform sampler2D uTextureArray[1024];

layout (std140, set = 1, binding = 1) uniform uMaterialData {
    Material uMaterialArray[64];
};

layout (set = 1, binding = 2) uniform uLightsData {
    LightInfo uLightInfos[8];
};

layout (push_constant) uniform uNodePushConstant
{
    uint uSubMeshIndex;
    uint uMaterialIndex;
    uint uNumLights;
};

#define MATERIAL_TEXTURE_COUNT 512
#define PI 3.14159265359

#define DIRECTIONAL_LIGHT 0
#define POINT_LIGHT       1
#define SPOT_LIGHT        2

const vec3 F0 = vec3(0.04);

bool HAS_BASECOLOR_TEXUTER = true;
bool HAS_NORMAL_TEXTURE = false;
bool HAS_METALLIC_ROUGHNESS_TEXTURE = false;
bool HAS_AO_TEXTURE = false;
bool HAS_EMISSIVE_TEXTURE = false;

// Fresnel Schlick
vec3 FresnelSchlick(vec3 F0, float F90, float u)
{
    return F0 + (F90 - F0) * pow(1.0 - u, 5.0);
}

// Renormalized Diffuse Term
float Fr_DisneyDiffuse(float NdotV, float NdotL, float LdotH, float roughness)
{
    float E_bias = 0.0 * (1.0 - roughness) + 0.5 * roughness;
    float E_factor = 1.0 * (1.0 - roughness) + (1.0 / 1.51) * roughness;
    float fd90 = E_bias + 2.0 * LdotH * LdotH * roughness;
    vec3 f0 = vec3(1.0);
    float lightScatter = FresnelSchlick(f0, fd90, NdotL).r;
    float viewScatter = FresnelSchlick(f0, fd90, NdotV).r;
    return lightScatter * viewScatter * E_factor;
}

// Specular Microfacet Model
float V_SmithGGXCorrelated(float NdotV, float NdotL, float roughness)
{
    float alphaRoughnessSq = roughness * roughness;

    float GGXV = NdotL * sqrt(NdotV * NdotV * (1.0 - alphaRoughnessSq) + alphaRoughnessSq);
    float GGXL = NdotV * sqrt(NdotL * NdotL * (1.0 - alphaRoughnessSq) + alphaRoughnessSq);

    float GGX = GGXV + GGXL;
    if (GGX > 0.0)
    {
        return 0.5 / GGX;
    }
    return 0.0;
}

// GGX Normal Distribution Function
float D_GGX(float NdotH, float roughness)
{
    float alphaRoughnessSq = roughness * roughness;
    float f = (NdotH * alphaRoughnessSq - NdotH) * NdotH + 1.0;
    return alphaRoughnessSq / (PI * f * f);
}

vec3 GetNormal() {
    bool hasNormalTex = uMaterialArray[uMaterialIndex].normalTexSet > -1;
    if (hasNormalTex) {
        int normalTexIndex = uMaterialArray[uMaterialIndex].normalTexIndex;
        vec3 n = texture(uTextureArray[normalTexIndex], vUV0).rgb;
        return normalize(n);
    } else {
        return normalize(vNormal);
    }
}

vec3 GetDiffuse(vec3 baseColor, float metallic) {
    return baseColor * (1.0 - metallic) + ((1.0 - metallic) * baseColor) * metallic;
}

float saturate(float t)
{
    return clamp(t, 0.0, 1.0);
}

vec3 saturate(vec3 t)
{
    return clamp(t, 0.0, 1.0);
}

void SetTextureStatus() {
    Material mat = uMaterialArray[uMaterialIndex];
    HAS_BASECOLOR_TEXUTER = mat.bcTexSet > -1;
    HAS_NORMAL_TEXTURE = mat.normalTexSet > -1;
    HAS_METALLIC_ROUGHNESS_TEXTURE = mat.mrTexSet > -1;
    HAS_AO_TEXTURE = mat.aoTexSet > -1;
    HAS_EMISSIVE_TEXTURE = mat.emissiveTexSet > -1;
}

vec3 GetLightDirection(uint index)
{
    if (uLightInfos[index].position.w == DIRECTIONAL_LIGHT)
    {
        return -uLightInfos[index].direction.xyz;
    }
    if (uLightInfos[index].position.w == POINT_LIGHT)
    {
        return uLightInfos[index].position.xyz - vPos.xyz;
    }
}

vec3 ApplyDirectionalLight(vec3 L, vec3 normal, uint lightIndex)
{
    L = normalize(L);

    float ndotl = clamp(dot(normal, L), 0.0, 1.0);

    return ndotl * uLightInfos[lightIndex].color.rgb;
}

vec3 ApplyPointLight(vec3 L, vec3 normal, uint lightIndex)
{
    float dist = length(L);

    float atten = 1.0 / (dist * dist);

    L = normalize(L);

    float ndotl = clamp(dot(normal, L), 0.0, 1.0);

    return ndotl * uLightInfos[lightIndex].color.w * atten * uLightInfos[lightIndex].color.rgb;
}

void main()
{
    SetTextureStatus();
    float F90 = saturate(50.0 * F0.r);
    Material mat = uMaterialArray[uMaterialIndex];
    vec4 baseColor = vec4(1.0, 0.0, 0.0, 1.0);
    if (HAS_BASECOLOR_TEXUTER) {
        baseColor = texture(uTextureArray[mat.bcTexIndex], vUV0);
    }
    float roughness = mat.roughnessFactor;
    float metallic = mat.metallicFactor;
    if (HAS_METALLIC_ROUGHNESS_TEXTURE) {
        roughness = saturate(texture(uTextureArray[mat.mrTexIndex], vUV0).g);
        metallic = saturate(texture(uTextureArray[mat.mrTexIndex], vUV0).b);
    }
    vec3 N = GetNormal();
    vec3 V = normalize(uCameraPos.xyz - vPos);
    float NdotV = saturate(dot(N, V));
    vec3 lightContribution = vec3(0.0);

    vec3 diffuseColor = GetDiffuse(baseColor.rgb, metallic);
    for (uint i = 0; i < uNumLights; i++) {
        vec3 L = GetLightDirection(i);
        vec3 H = normalize(V + L);

        float LdotH = saturate(dot(L, H));
        float NdotH = saturate(dot(N, H));
        float NdotL = saturate(dot(N, L));

        vec3 F = FresnelSchlick(F0, F90, LdotH);
        float Vis = V_SmithGGXCorrelated(NdotV, NdotL, roughness);
        float D = D_GGX(NdotH, roughness);
        vec3 Fr = F * D * Vis;

        float Fd = Fr_DisneyDiffuse(NdotV, NdotL, LdotH, roughness);

        if (uLightInfos[i].position.w == DIRECTIONAL_LIGHT)
        {
            lightContribution += ApplyDirectionalLight(L, N, i) * (diffuseColor * (vec3(1.0) - F) * Fd + Fr);
        }
        if (uLightInfos[i].position.w == POINT_LIGHT)
        {
            lightContribution += ApplyPointLight(L, N, i) * (diffuseColor * (vec3(1.0) - F) * Fd + Fr);
        }
    }
    outColor = vec4(lightContribution.rgb, baseColor.a);
}
