#version 450

layout (set = 0, binding = 0) uniform sampler2D positionMap;
layout (set = 0, binding = 1) uniform sampler2D normalMap;
layout (set = 0, binding = 2) uniform sampler2D albedoMap;
layout (set = 0, binding = 3) uniform sampler2D metallicRoughnessMap;
layout (set = 0, binding = 4) uniform sampler2D emissiveOcclusionMap;

layout (location = 0) in vec2 inUV;
layout (location = 0) out vec4 outFragColor;

const int NUM_LIGHTS = 4;

layout (set = 1, binding = 0) uniform SceneUBO {
	vec4 lightPosition[NUM_LIGHTS];  // Positions of 4 lights
	vec4 lightColor[NUM_LIGHTS];     // Colors of 4 lights
	vec4 lightIntensity[NUM_LIGHTS]; // Intensities of 4 lights
	vec4 viewPosition;               // Camera/View position
} sceneUbo;

void main() {
	// Sample G-buffer textures
	vec3 position = texture(positionMap, inUV).rgb;
	vec3 normal = normalize(texture(normalMap, inUV).rgb);
	vec4 albedoTex = texture(albedoMap, inUV);
	vec3 albedo = albedoTex.rgb;

	float occlusion = texture(emissiveOcclusionMap, inUV).a;
	float metallic = texture(metallicRoughnessMap, inUV).r;
	float roughness = texture(metallicRoughnessMap, inUV).g;
	vec3 emissive = texture(emissiveOcclusionMap, inUV).rgb;

	// Fresnel-Schlick Approximation
	vec3 F0 = mix(vec3(0.04), albedo, metallic);

	// Accumulate lighting from all 4 sources
	vec3 finalColor = vec3(0.0);

	for (int i = 0; i < NUM_LIGHTS; i++) {
		vec3 lightDir = normalize(sceneUbo.lightPosition[i].xyz - position);
		float distance = length(sceneUbo.lightPosition[i].xyz - position);
		float attenuation = 1.0 / (distance * distance + 0.1);
		vec3 radiance = sceneUbo.lightColor[i].rgb * sceneUbo.lightIntensity[i].rgb * attenuation;

		// Lambertian Diffuse
		float NdotL = max(dot(normal, lightDir), 0.0);
		vec3 diffuse = (1.0 - metallic) * albedo / 3.14159265359;

		// Specular (Cook-Torrance Microfacet Model)
		vec3 viewDir = normalize(sceneUbo.viewPosition.xyz - position);
		vec3 halfDir = normalize(viewDir + lightDir);
		float NdotH = max(dot(normal, halfDir), 0.0);
		float NdotV = max(dot(normal, viewDir), 0.0);

		float roughnessSquared = roughness * roughness;
		float D = roughnessSquared / (3.14159265359 * pow(NdotH * (roughnessSquared - 1.0) + 1.0, 2.0));
		float k = (roughness + 1.0) * (roughness + 1.0) / 8.0;
		float G = (NdotL / (NdotL * (1.0 - k) + k)) * (NdotV / (NdotV * (1.0 - k) + k));
		vec3 F = F0 + (1.0 - F0) * pow(1.0 - NdotV, 5.0);

		vec3 specular = (D * G * F) / (4.0 * NdotL * NdotV + 0.001);

		// Combine diffuse and specular
		finalColor += (diffuse + specular) * radiance * NdotL * occlusion;
	}

	// Add ambient term to simulate indirect lighting
	vec3 ambient = vec3(0.03) * albedo * occlusion;
	finalColor += ambient + emissive;

	// Apply gamma correction
	finalColor = pow(finalColor, vec3(1.0 / 2.2));

	outFragColor = vec4(finalColor, 1.0);
}
