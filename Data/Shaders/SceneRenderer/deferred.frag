#version 450

layout (set = 0, binding = 0) uniform sampler2D positionMap;
layout (set = 0, binding = 1) uniform sampler2D normalMap;
layout (set = 0, binding = 2) uniform sampler2D albedoMap;
layout (set = 0, binding = 3) uniform sampler2D metallicRoughnessMap;
layout (set = 0, binding = 4) uniform sampler2D emissiveOcclusionMap;
layout (set = 0, binding = 5) uniform sampler2D depthMap;
layout (set = 0, binding = 6) uniform samplerCube envIrradianceMap;
layout (set = 0, binding = 7) uniform samplerCube envPrefilteredMap;
layout (set = 0, binding = 8) uniform sampler2D lutBRDFMap;

layout (location = 0) in vec2 inUV;
layout (location = 0) out vec4 outFragColor;

const int NUM_LIGHTS = 4;
const float PI = 3.14159265359;
const float MAX_REFLECTION_LOD = 4.0;

layout (set = 1, binding = 0) uniform uSceneData {
	vec4 lightPosition[NUM_LIGHTS];
	vec4 lightColor[NUM_LIGHTS];
	vec4 lightIntensity[NUM_LIGHTS];
	vec4 viewPosition;
} sceneUbo;

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
	float lod = roughness * MAX_REFLECTION_LOD;
	return textureLod(envPrefilteredMap, R, lod).rgb;
}

// ---------- Main ----------
void main() {
	float depth = texture(depthMap, inUV).r;
	if (depth >= 0.9999) discard;

	vec3 worldPos = texture(positionMap, inUV).rgb;
	vec3 N = normalize(texture(normalMap, inUV).rgb);
	vec4 albRGBA = texture(albedoMap, inUV);
	vec3 albedo = albRGBA.rgb;
	if (albRGBA.a < 0.1) discard;

	vec2 mr = texture(metallicRoughnessMap, inUV).rg;
	float metallic = clamp(mr.r, 0.0, 1.0);
	float roughness = clamp(mr.g, 0.04, 1.0);

	vec4 emissiveOccl = texture(emissiveOcclusionMap, inUV);
	vec3 emissive = emissiveOccl.rgb;
	float ao = clamp(emissiveOccl.a, 0.0, 1.0);

	vec3 V = normalize(sceneUbo.viewPosition.xyz - worldPos);
	float NdotV = max(dot(N, V), 0.001);
	vec3 F0 = mix(vec3(0.04), albedo, metallic);

	// ---------- Direct Lighting ----------
	vec3 Lo = vec3(0.0);
	for (int i = 0; i < NUM_LIGHTS; ++i) {
		vec3 L = normalize(sceneUbo.lightPosition[i].xyz - worldPos);
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

		float distance = length(sceneUbo.lightPosition[i].xyz - worldPos);
		float attenuation = 1.0 / (distance * distance + 0.01);
		vec3 radiance = sceneUbo.lightColor[i].rgb * sceneUbo.lightIntensity[i].r * attenuation;

		Lo += (kD * diffuse + specular) * radiance * NdotL;
	}

	// ---------- IBL ----------
	vec3 irradiance = texture(envIrradianceMap, N).rgb;
	vec3 R = reflect(-V, N);
	vec3 prefilteredColor = SamplePrefiltered(R, roughness);
	vec2 brdf = texture(lutBRDFMap, vec2(NdotV, roughness)).rg;
	vec3 F = FresnelSchlick(NdotV, F0);
	vec3 kS = F;
	vec3 kD = (vec3(1.0) - kS) * (1.0 - metallic);
	vec3 diffuseIBL = irradiance * albedo;
	vec3 specularIBL = prefilteredColor * (F * brdf.x + brdf.y);
	vec3 ambient = (kD * diffuseIBL * ao) + (specularIBL * ao);

	vec3 color = Lo + ambient + emissive;

	// simple Reinhard tone mapping
	color = color / (color + vec3(1.0));
	color = pow(max(color, vec3(0.0)), vec3(1.0 / 2.2));

	outFragColor = vec4(color, 1.0);
}
