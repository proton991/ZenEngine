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
    vec4 position;// (x, y, z, lightType)
    vec4 color;// (r, g, b, intensity)
    vec4 direction;// only for direction lights

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
#define RECIPROCAL_PI 0.3183098861837907

#define DIRECTIONAL_LIGHT 0
#define POINT_LIGHT       1
#define SPOT_LIGHT        2
#define CAMERA_LIGHT      3

vec3 F0 = vec3(0.04);

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

float G1_GGX_Schlick(float NoV, float roughness) {
    float alpha = roughness * roughness;
    float k = alpha / 2.0;
    return max(NoV, 0.001) / (NoV * (1.0 - k) + k);
}

float G_Smith(float NdotV, float NdotL, float roughness) {
    return G1_GGX_Schlick(NdotV, roughness) * G1_GGX_Schlick(NdotL, roughness);
}

// GGX Normal Distribution Function
float D_GGX(float NdotH, float roughness)
{
    float alphaRoughnessSq = roughness * roughness;
    float f = (NdotH * alphaRoughnessSq - NdotH) * NdotH + 1.0;
    return alphaRoughnessSq / (PI * f * f);
}

// from http://www.thetenthplanet.de/archives/1180
mat3 cotangentFrame(in vec3 N, in vec3 p, in vec2 uv)
{
    // get edge vectors of the pixel triangle
    vec3 dp1 = dFdx(p);
    vec3 dp2 = dFdy(p);
    vec2 duv1 = dFdx(uv);
    vec2 duv2 = dFdy(uv);

    // solve the linear system
    vec3 dp2perp = cross(dp2, N);
    vec3 dp1perp = cross(N, dp1);
    vec3 T = dp2perp * duv1.x + dp1perp * duv2.x;
    vec3 B = dp2perp * duv1.y + dp1perp * duv2.y;

    // construct a scale-invariant frame
    float invmax = inversesqrt(max(dot(T, T), dot(B, B)));
    return mat3(T * invmax, B * invmax, N);
}

vec3 GetNormal() {
    vec3 pos_dx = dFdx(vPos);
    vec3 pos_dy = dFdy(vPos);
    vec3 st1    = dFdx(vec3(vUV0, 0.0));
    vec3 st2    = dFdy(vec3(vUV0, 0.0));
    vec3 T      = (st2.t * pos_dx - st1.t * pos_dy) / (st1.s * st2.t - st2.s * st1.t);
    vec3 N      = normalize(vNormal);
    T           = normalize(T - N * dot(N, T));
    vec3 B      = normalize(cross(N, T));
    mat3 TBN    = mat3(T, B, N);
    if (HAS_NORMAL_TEXTURE) {
        int normalTexIndex = uMaterialArray[uMaterialIndex].normalTexIndex;
        vec3 n = texture(uTextureArray[normalTexIndex], vUV0).rgb;
        return normalize(TBN * (2.0 * n - 1.0));
    } else {
        return normalize(TBN[2].xyz);
    }
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

vec3 GetCameraLightDirection() {
    return uCameraPos.xyz - vPos.xyz;
}

vec3 ApplyCameraLight(vec3 L, vec3 N, uint lightIndex)
{
    L = normalize(L);

    float NdotL = clamp(dot(N, L), 0.0, 1.0);

    return NdotL * uLightInfos[lightIndex].color.rgb * uLightInfos[lightIndex].color.w;
}

vec3 ApplyDirectionalLight(vec3 L, vec3 N, uint lightIndex)
{
    float NdotL = max(dot(N, L), 0.0f);
    vec3 intensity = vec3(uLightInfos[lightIndex].color.w);
    return NdotL * uLightInfos[lightIndex].color.rgb * intensity;
}

vec3 ApplyPointLight(vec3 L, vec3 N, uint lightIndex)
{
    float dist = length(L);

    float atten = min(1.0f / (dist * dist), 1.0f);

    float NdotL = max(dot(N, L), 0.0f);

    return NdotL * uLightInfos[lightIndex].color.w  * uLightInfos[lightIndex].color.rgb;
}

void main()
{
    SetTextureStatus();

    Material mat = uMaterialArray[uMaterialIndex];
    vec4 baseColor = mat.baseColorFactor;
    if (HAS_BASECOLOR_TEXUTER) {
        baseColor *= texture(uTextureArray[mat.bcTexIndex], vUV0);
    }
    float roughness = mat.roughnessFactor;
    float metallic = mat.metallicFactor;
    if (HAS_METALLIC_ROUGHNESS_TEXTURE) {
        roughness *= texture(uTextureArray[mat.mrTexIndex], vUV0).g;
        metallic *= texture(uTextureArray[mat.mrTexIndex], vUV0).b;
    }

    F0 = mix(F0, baseColor.rgb, metallic);
    float F90 = saturate(50.0f * F0.r);

    vec3 V = normalize(uCameraPos.xyz - vPos);
    vec3 N = GetNormal();
    float NdotV = saturate(dot(N, V));
    vec3 lightContribution = vec3(0.0f);

    vec3 diffuseColor = baseColor.rgb * (1.0f - metallic);
    for (uint i = 0; i < uNumLights; i++) {
        vec3 L = GetLightDirection(i);
        vec3 H = normalize(V + L);

        float LdotH = saturate(dot(L, H));
        float NdotH = saturate(dot(N, H));
        float NdotL = saturate(dot(N, L));

        vec3 F = FresnelSchlick(F0, F90, LdotH);
        float G = G_Smith(NdotV, NdotL, roughness);
        float D = D_GGX(NdotH, roughness);
        // compute reflection part
        vec3 Fr = F * D * G / (4.0f * max(NdotV, 0.001f) * max(NdotL, 0.001f));
        // compute diffuse part
        vec3 Fd = diffuseColor;
        Fd *= (vec3(1.0f) - F);
        Fd *= Fr_DisneyDiffuse(NdotV, NdotL, LdotH, roughness);
        Fd *= RECIPROCAL_PI;

        if (uLightInfos[i].position.w == DIRECTIONAL_LIGHT)
        {
            lightContribution += ApplyDirectionalLight(L, N, i) * (Fd + Fr);
        }
        if (uLightInfos[i].position.w == POINT_LIGHT)
        {
            lightContribution += ApplyPointLight(L, N, i) * (Fd + Fr);
        }
    }
    vec3 finalColor = lightContribution.rgb;
    const float occlusionStrength = 1.0f;
    if (HAS_AO_TEXTURE) {
        float ao = texture(uTextureArray[mat.aoTexIndex], vUV0).r;
        finalColor = mix(finalColor, finalColor * ao, occlusionStrength);
    }

    const float emissiveFactor = 1.0f;
    if (HAS_EMISSIVE_TEXTURE) {
        vec3 emissive = texture(uTextureArray[mat.emissiveTexIndex], vUV0).rgb * emissiveFactor;
        finalColor += emissive;
    }

    vec3 irradiance  = vec3(0.5f);
    vec3 ambientColor = irradiance * baseColor.rgb;
    outColor = vec4(0.3f * ambientColor + finalColor, baseColor.a);
}
