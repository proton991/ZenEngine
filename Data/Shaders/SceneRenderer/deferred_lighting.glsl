#include "../Common/bindless_heap.glsl"

layout (set = 1, binding = 0) uniform sampler2D positionMap;
layout (set = 1, binding = 1) uniform sampler2D normalMap;
layout (set = 1, binding = 2) uniform sampler2D albedoMap;
layout (set = 1, binding = 3) uniform sampler2D metallicRoughnessMap;
layout (set = 1, binding = 4) uniform sampler2D emissiveOcclusionMap;
layout (set = 1, binding = 5) uniform sampler2D depthMap;
layout (set = 1, binding = 6) uniform samplerCube envIrradianceMap;
layout (set = 1, binding = 7) uniform samplerCube envPrefilteredMap;
layout (set = 1, binding = 8) uniform sampler2D lutBRDFMap;

layout (location = 0) in vec2 inUV;
layout (location = 0) out vec4 outFragColor;
#ifdef LIGHTING_CAPTURE
#include "Graphics/Shared/LightingCapture.h"
layout(set=4,binding=2,std430) buffer LightingCapture { vec4 components[]; };
#ifdef DYNAMIC_VOXEL_GI
layout(set=4,binding=3,std430) buffer SurfaceCapture { vec4 surfaceComponents[]; };
#endif
layout(push_constant) uniform CaptureConstants { uvec2 extent; } capture;
#endif

#include "../Common/scene_lighting.glsl"
#ifdef VOXEL_GI
#include "../VoxelGI/cone_trace.glsl"
#include "../ShadowMapping/scene_shadows.glsl"
#endif
#ifdef DYNAMIC_VOXEL_GI
#include "../VoxelGI/Dynamic/static_composition.glsl"
layout(set=1,binding=12) uniform usampler2D receiverMap;
layout(set=1,binding=13) uniform sampler2D geometricNormalMap;
#include "surface_lookup.glsl"
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
#ifdef DYNAMIC_VOXEL_GI
    ivec2 surfaceTexel=SurfaceTexel(uvec2(gl_FragCoord.xy),surfaceExtent.xy,textureSize(positionMap,0));
#define SURFACE_SAMPLE(map) texelFetch(map,surfaceTexel,0)
    if(!ValidSurface(SURFACE_SAMPLE(depthMap).r,SURFACE_SAMPLE(receiverMap).rg,
                      SURFACE_SAMPLE(positionMap),SURFACE_SAMPLE(normalMap),SURFACE_SAMPLE(geometricNormalMap))) discard;
#else
#define SURFACE_SAMPLE(map) texture(map,inUV)
#endif
	float depth = SURFACE_SAMPLE(depthMap).r;
	if (depth >= 0.9999) discard;

	vec3 worldPos = SURFACE_SAMPLE(positionMap).rgb;
	vec3 N = normalize(SURFACE_SAMPLE(normalMap).rgb);
#ifdef DYNAMIC_VOXEL_GI
    vec3 surfaceNormal=normalize(SURFACE_SAMPLE(geometricNormalMap).xyz);
#elif defined(VOXEL_GI)
    vec3 surfaceNormal=cross(dFdx(worldPos),dFdy(worldPos));
    float normalLength=length(surfaceNormal);
    surfaceNormal=normalLength>1e-8 ? surfaceNormal/normalLength : N;
    if(dot(surfaceNormal,N)<0) surfaceNormal=-surfaceNormal;
#endif
	vec4 albRGBA = SURFACE_SAMPLE(albedoMap);
	vec3 albedo = albRGBA.rgb;
	if (albRGBA.a < 0.1) discard;
	// Preserve scene depth for forward-rendered light markers.
	gl_FragDepth = depth;

	vec2 mr = SURFACE_SAMPLE(metallicRoughnessMap).rg;
	float metallic = clamp(mr.r, 0.0, 1.0);
	float roughness = clamp(mr.g, 0.04, 1.0);

	vec4 emissiveOccl = SURFACE_SAMPLE(emissiveOcclusionMap);
	vec3 emissive = emissiveOccl.rgb;
	float ao = clamp(emissiveOccl.a, 0.0, 1.0);

	vec3 V = normalize(sceneUbo.viewPosition.xyz - worldPos);
	float NdotV = max(dot(N, V), 0.001);
	vec3 F0 = mix(vec3(0.04), albedo, metallic);

	// ---------- Direct Lighting ----------
	vec3 Lo = vec3(0.0);
	for (int i = 0; i < int(sceneUbo.lightInfo.x); ++i) {
		vec3 L;
        float distanceToLight;
        vec3 radiance = EvaluateLight(sceneUbo.lights[i], worldPos, L, distanceToLight);
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
        visibility = SceneLightVisibility(i,worldPos,surfaceNormal);
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
#ifdef DYNAMIC_VOXEL_GI
    vec3 diffuseIrradiance=vec3(0);
    bool directionalReady=giStatus.z==0u &&
        DirectionalDiffuseIrradiance(worldPos,N,SURFACE_SAMPLE(receiverMap).g,diffuseIrradiance);
    vec3 diffuseLighting=directionalReady ? diffuseIrradiance/PI : DiffuseVoxelLighting(worldPos,N);
#ifdef LIGHTING_CAPTURE
    if(directionalReady) { captureDiffuseEscaped=vec3(0); captureDiffuseBounced=diffuseLighting; }
#endif
    vec3 diffuseIBL=diffuseLighting*albedo;
#elif defined(VOXEL_GI)
    vec3 diffuseIBL = DiffuseVoxelLighting(worldPos,N) * albedo;
#else
    vec3 diffuseIBL = irradiance * albedo;
#endif
	vec3 specularIBL = prefilteredColor * (F * brdf.x + brdf.y);
	vec3 ambient = (kD * diffuseIBL * ao) + (specularIBL * ao);

	vec3 color = Lo + ambient + emissive;
#ifdef LIGHTING_CAPTURE
    uvec2 pixel=uvec2(gl_FragCoord.xy);
    if(all(lessThan(pixel,capture.extent)))
    {
        uint index=(pixel.x+capture.extent.x*pixel.y)*ZEN_LIGHTING_CAPTURE_COMPONENTS;
#ifdef DYNAMIC_VOXEL_GI
        uint surfaceIndex=(pixel.x+capture.extent.x*pixel.y)*ZEN_SURFACE_CAPTURE_COMPONENTS;
        surfaceComponents[surfaceIndex]=SURFACE_SAMPLE(positionMap);
        surfaceComponents[surfaceIndex+1u]=vec4(N,1);
        surfaceComponents[surfaceIndex+2u]=SURFACE_SAMPLE(geometricNormalMap);
        surfaceComponents[surfaceIndex+3u]=uintBitsToFloat(uvec4(SURFACE_SAMPLE(receiverMap).rg,uvec2(surfaceTexel)));
        surfaceComponents[surfaceIndex+4u]=vec4(albedo,metallic);
        surfaceComponents[surfaceIndex+5u]=vec4(ao,roughness,directionalReady ? 1 : 0,0);
#endif
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
	color = color / (color + vec3(1.0));
	color = pow(max(color, vec3(0.0)), vec3(1.0 / 2.2));

	outFragColor = vec4(color, 1.0);
}
