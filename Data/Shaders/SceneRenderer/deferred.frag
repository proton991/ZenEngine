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

const bool useIBL = true; // Control for enabling IBL or not

layout (set = 1, binding = 0) uniform uSceneData {
	vec4 lightPosition[NUM_LIGHTS];  // Positions of 4 lights
	vec4 lightColor[NUM_LIGHTS];     // Colors of 4 lights
	vec4 lightIntensity[NUM_LIGHTS]; // Intensities of 4 lights
	vec4 viewPosition;               // Camera/View position
} sceneUbo;

// Function to sample the environment irradiance map for diffuse lighting
vec3 SampleEnvIrradianceMap(vec3 normal) {
	return texture(envIrradianceMap, normal).rgb;
}

// Function to sample the environment prefiltered map for specular reflection
vec3 SampleEnvPrefilteredMap(vec3 normal, float roughness) {
	float lod = roughness * 5.0; // Roughness determines the mip level
	return textureLod(envPrefilteredMap, normal, lod).rgb;
}

// Function to calculate the specular reflection using the BRDF LUT
float CalculateBRDFSpecular(float NdotV, float roughness) {
	return texture(lutBRDFMap, vec2(NdotV, roughness)).r; // Lookup BRDF for specular reflection
}

// Function to calculate the lighting using traditional point lights
vec3 CalculateDirectLighting(vec3 position, vec3 normal, float occlusion, vec3 albedo, float metallic, float roughness) {
	vec3 finalColor = vec3(0.0);

	// Accumulate lighting from all lights in the scene
	for (int i = 0; i < NUM_LIGHTS; i++) {
		vec3 lightDir = normalize(sceneUbo.lightPosition[i].xyz - position);
		float distance = length(sceneUbo.lightPosition[i].xyz - position);
		float attenuation = 1.0 / (distance * distance + 0.1); // Light attenuation
		vec3 radiance = sceneUbo.lightColor[i].rgb * sceneUbo.lightIntensity[i].rgb * attenuation;

		// Lambertian Diffuse (direct lighting)
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
		vec3 F = mix(vec3(0.04), albedo, metallic) + (1.0 - mix(vec3(0.04), albedo, metallic)) * pow(1.0 - NdotV, 5.0);

		vec3 specular = (D * G * F) / (4.0 * NdotL * NdotV + 0.001);

		// Combine diffuse and specular lighting
		finalColor += (diffuse + specular) * radiance * NdotL * occlusion;
	}

	return finalColor;
}

void main() {
	// Sample the depth from the G-buffer texture
	float depth = texture(depthMap, inUV).r; // Retrieve depth from the texture (r component)

	// If depth is 1 (indicating skybox or background), don't perform deferred lighting
	if (depth == 1.0) {
		discard; // Skip lighting calculations (we're rendering the skybox or background)
	}

	// Sample G-buffer textures
	vec3 position = texture(positionMap, inUV).rgb;
	vec3 normal = normalize(texture(normalMap, inUV).rgb);
	vec4 albedoTex = texture(albedoMap, inUV);
	vec3 albedo = albedoTex.rgb;

	// Perform alpha test using albedo alpha component
	if (albedoTex.a < 0.1f) {
		discard; // Discard the fragment if it doesn't pass the alpha threshold
	}


	float occlusion = texture(emissiveOcclusionMap, inUV).a;
	float metallic = texture(metallicRoughnessMap, inUV).r;
	float roughness = texture(metallicRoughnessMap, inUV).g;
	vec3 emissive = texture(emissiveOcclusionMap, inUV).rgb;

	// Fresnel-Schlick Approximation for F0 (the reflection color at normal incidence)
	vec3 F0 = mix(vec3(0.04), albedo, metallic);

	// Initialize final color (which will be adjusted by IBL or direct lighting)
	vec3 finalColor = vec3(0.0);

	if (useIBL) {
		vec3 irradiance = SampleEnvIrradianceMap(normal); // Diffuse from environment
		vec3 prefiltered = SampleEnvPrefilteredMap(normal, roughness); // Specular from environment
		vec2 brdf = texture(lutBRDFMap, vec2(max(dot(normal, normalize(sceneUbo.viewPosition.xyz - position)), 0.0), roughness)).rg; // Use both channels

		// Fresnel-Schlick for IBL
		vec3 F0 = mix(vec3(0.04), albedo, metallic);
		vec3 F = F0 + (1.0 - F0) * pow(1.0 - max(dot(normal, normalize(sceneUbo.viewPosition.xyz - position)), 0.0), 5.0);

		// Diffuse reflection from IBL
		vec3 diffuse = (1.0 - metallic) * albedo * irradiance;

		// Specular reflection from IBL
		vec3 specular = prefiltered * (F * brdf.x + brdf.y);

		// Final color accumulation
		finalColor += diffuse * occlusion + specular;
	} else {
		// Static lighting
		finalColor += CalculateDirectLighting(position, normal, occlusion, albedo, metallic, roughness);
	}

	// Add emissive lighting to the final color
	finalColor += emissive;

	// Apply gamma correction
	finalColor = pow(finalColor, vec3(1.0 / 2.2));

	// Output the final color
	outFragColor = vec4(finalColor, 1.0);
}