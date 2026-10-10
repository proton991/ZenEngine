#include "../Common/bindless_heap.glsl"
#include "../Common/linear_to_srgb.glsl"
#include "../Common/gbuffer.glsl"

layout (set = 1, binding = 0, std140) uniform uGBufferData { mat4 inverseViewProjection; vec4 worldOrigin; } gbuffer;
layout (set = 1, binding = 1) uniform sampler2D normalMap;
layout (set = 1, binding = 2) uniform sampler2D albedoMap;
layout (set = 1, binding = 3) uniform sampler2D metallicRoughnessMap;
layout (set = 1, binding = 4) uniform sampler2D emissiveOcclusionMap;
layout (set = 1, binding = 5) uniform sampler2D depthMap;
layout (set = 1, binding = 6) uniform samplerCube envIrradianceMap;
layout (set = 1, binding = 7) uniform samplerCube envPrefilteredMap;
layout (set = 1, binding = 8) uniform sampler2D lutBRDFMap;

layout (location = 0) out vec4 outFragColor;
#ifdef LIGHTING_CAPTURE
#include "Graphics/Shared/LightingCapture.h"
layout(set=4,binding=2,std430) buffer LightingCapture { vec4 components[]; };
layout(push_constant) uniform CaptureConstants { uvec2 extent; } capture;
#endif

#include "../Common/scene_lighting.glsl"
#ifdef VOXEL_GI
#include "../VoxelGI/cone_trace.glsl"
#include "../ShadowMapping/scene_shadows.glsl"
#endif
#ifdef HYBRID_GI
#include "../VoxelGI/hybrid_composition.glsl"
#endif
const float PI = 3.14159265359;

// ---------- PBR Helpers ----------
float DistributionGGX(vec3 N, vec3 H, float roughness) {
	float a = roughness * roughness;
	float a2 = a * a;
	float NdotH = max(dot(N, H), 0.0);
	float NdotH2 = NdotH * NdotH;
	float denom = (NdotH2 * (a2 - 1.0) + 1.0);
	denom = PI * denom * denom;
	return a2 / max(denom, 1e-6);
}

float GeometrySchlickGGX(float NdotV, float k) {
	return NdotV / (NdotV * (1.0 - k) + k);
}

float GeometrySmith(vec3 N, vec3 V, vec3 L, float roughness) {
	float k = (roughness + 1.0);
	k = k * k * 0.125;
	float ggx1 = GeometrySchlickGGX(max(dot(N, V), 0.0), k);
	float ggx2 = GeometrySchlickGGX(max(dot(N, L), 0.0), k);
	return ggx1 * ggx2;
}

vec3 FresnelSchlick(float cosTheta, vec3 F0) {
	return F0 + (1.0 - F0) * pow(1.0 - cosTheta, 5.0);
}

vec3 SamplePrefiltered(vec3 R, float roughness) {
	float lod = roughness * float(textureQueryLevels(envPrefilteredMap) - 1);
	return textureLod(envPrefilteredMap, EnvironmentDirection(R), lod).rgb * sceneUbo.environment.x * sceneUbo.environment.z;
}

// ---------- Main ----------
void main() {
// The G-buffer matches the viewport, so each fragment reads exactly its own texel.
// Filtering would blend positions and normals across silhouettes.
#define SURFACE_SAMPLE(map) texelFetch(map,ivec2(gl_FragCoord.xy),0)
	float depth = SURFACE_SAMPLE(depthMap).r;
	// Authored near/far ranges can place valid geometry arbitrarily close to
	// depth one. Only the attachment's exact clear depth identifies background.
	if (depth >= 1.0) discard;
	vec3 relativePosition = ReconstructGBufferPosition(gl_FragCoord.xy, textureSize(depthMap, 0), depth, gbuffer.inverseViewProjection);
	vec3 worldPos = relativePosition + gbuffer.worldOrigin.xyz;
	vec3 N = DecodeGBufferNormal(SURFACE_SAMPLE(normalMap).rg);
#if defined(VOXEL_GI)
#ifdef HYBRID_GI
    vec3 surfaceNormal=DecodeGBufferNormal(unpackUnorm2x16(texelFetch(hybridSurface,ivec2(gl_FragCoord.xy),0).r));
#else
    vec3 surfaceNormal=cross(dFdx(relativePosition),dFdy(relativePosition));
    float normalLength=length(surfaceNormal);
    surfaceNormal=normalLength>1e-8 ? surfaceNormal/normalLength : N;
    if(dot(surfaceNormal,N)<0) surfaceNormal=-surfaceNormal;
#endif
#endif
	vec4 albRGBA = SURFACE_SAMPLE(albedoMap);
	vec3 albedo = albRGBA.rgb;
	if (albRGBA.a < 0.1) discard;
	// Preserve scene depth for forward-rendered light markers.
	gl_FragDepth = depth;

	vec4 mr = SURFACE_SAMPLE(metallicRoughnessMap);
	float metallic = clamp(mr.r, 0.0, 1.0);
	float roughness = clamp(mr.g, 0.04, 1.0);

	vec4 emissiveOccl = SURFACE_SAMPLE(emissiveOcclusionMap);
	vec3 emissive = emissiveOccl.rgb;
	float ao = clamp(emissiveOccl.a, 0.0, 1.0);

	vec3 V = sceneUbo.viewPosition.w < 0.5 ? normalize(sceneUbo.lightInfo.yzw) :
        normalize(sceneUbo.viewPosition.xyz - worldPos);
	float NdotV = max(dot(N, V), 0.001);
	vec3 F0 = mix(vec3(0.04), albedo, metallic);

	// ---------- Direct Lighting ----------
	vec3 Lo = vec3(0.0);
	for (int i = 0; i <= int(sceneUbo.lightInfo.x); ++i) {
		vec3 L;
        float distanceToLight;
        vec3 radiance = EvaluateLight(i == int(sceneUbo.lightInfo.x) ? sceneUbo.cameraLight : sceneUbo.lights[i], worldPos, L, distanceToLight);
		float NdotL = max(dot(N, L), 0.0);
		if (NdotL <= 0.0) continue;

		vec3 H = normalize(V + L);
		float D = DistributionGGX(N, H, roughness);
		float G = GeometrySmith(N, V, L, roughness);
		vec3 F = FresnelSchlick(max(dot(H, V), 0.0), F0);

		vec3 numerator = D * G * F;
		float denom = max(4.0 * NdotV * NdotL, 1e-6);
		vec3 specular = numerator / denom;

		vec3 kS = F;
		vec3 kD = (vec3(1.0) - kS) * (1.0 - metallic);
		vec3 diffuse = albedo / PI;


        float visibility = 1.0;
#ifdef VOXEL_GI
        visibility = SceneLightVisibility(i == int(sceneUbo.lightInfo.x) ? MAX_SCENE_LIGHTS : i,worldPos,surfaceNormal);
#endif
        Lo += (kD * diffuse + specular) * radiance * NdotL * visibility;
	}

	// ---------- IBL ----------
	vec3 irradiance = texture(envIrradianceMap, EnvironmentDirection(N)).rgb * sceneUbo.environment.x * sceneUbo.environment.z;
	vec3 R = reflect(-V, N);
	vec3 prefilteredColor = SamplePrefiltered(R, roughness);
	vec2 brdf = texture(lutBRDFMap, vec2(NdotV, roughness)).rg;
	vec3 F = FresnelSchlick(NdotV, F0);
	vec3 kS = F;
	vec3 kD = (vec3(1.0) - kS) * (1.0 - metallic);
#if defined(VOXEL_GI)
#ifdef HYBRID_GI
    vec3 sky=texelFetch(hybridSky,ivec2(gl_FragCoord.xy),0).rgb;
    vec3 bounce=gi.lighting.w>0.5 ? texelFetch(hybridBounce,ivec2(gl_FragCoord.xy),0).rgb : DiffuseVoxelLighting(worldPos,N,false);
    vec3 diffuseIBL=(bounce+sky)*albedo;
    prefilteredColor*=texelFetch(hybridSpecular,ivec2(gl_FragCoord.xy),0).a;
#ifdef LIGHTING_CAPTURE
    captureDiffuseEscaped=sky;
    captureDiffuseBounced=bounce;
#endif
#else
    vec3 diffuseIBL = DiffuseVoxelLighting(worldPos,N) * albedo;
#endif
#else
    vec3 diffuseIBL = irradiance * albedo;
#endif
	vec3 specularIBL = prefilteredColor * (F * brdf.x + brdf.y);
	vec3 ambient = (kD * diffuseIBL * ao) + (specularIBL * ao);

	bool unlit = mr.b > 0.5;
    if (unlit) { Lo=vec3(0); ambient=vec3(0); emissive=albedo; }
    vec3 color = Lo + ambient + emissive;
#ifdef LIGHTING_CAPTURE
    uvec2 pixel=uvec2(gl_FragCoord.xy);
    if(all(lessThan(pixel,capture.extent)))
    {
        uint index=(pixel.x+capture.extent.x*pixel.y)*ZEN_LIGHTING_CAPTURE_COMPONENTS;
        components[index+0]=vec4(color,1);
        components[index+1]=vec4(Lo,1);
        components[index+2]=vec4(kD*diffuseIBL*ao,1);
        components[index+3]=vec4(specularIBL*ao,1);
        components[index+4]=vec4(emissive,1);
#ifdef VOXEL_GI
        components[index+5]=vec4(kD*(captureDiffuseEscaped*albedo)*ao,1);
        components[index+6]=vec4(kD*(captureDiffuseBounced*albedo)*ao,1);
#else
        components[index+5]=vec4(kD*diffuseIBL*ao,1);
        components[index+6]=vec4(0,0,0,1);
#endif
    }
#endif

	// simple Reinhard tone mapping
	if (!unlit) color = color / (color + vec3(1.0));
	color = LinearToSRGB(color);

	outFragColor = vec4(color, 1.0);
}
