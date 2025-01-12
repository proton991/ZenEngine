#version 450

layout (set = 0, binding = 0) uniform sampler2D samplerPosition;
layout (set = 0, binding = 1) uniform sampler2D samplerNormal;
layout (set = 0, binding = 2) uniform sampler2D samplerAlbedo;

layout (location = 0) in vec2 inUV;

layout (location = 0) out vec4 outFragColor;

layout (set = 1, binding = 0) uniform SceneUBO
{
	vec4 lightPosition;
	vec4 lightColor;
	vec4 lightIntensity;
	vec4 viewPosition;
} sceneUbo;

void main() {
	// Sample G-buffer textures
	vec3 position = texture(samplerPosition, inUV).rgb;
	vec3 normal = normalize(texture(samplerNormal, inUV).rgb);
	vec3 albedo = texture(samplerAlbedo, inUV).rgb;

	// Calculate light direction and distance
	vec3 lightDir = normalize(sceneUbo.lightPosition.xyz - position);
	float distance = length(sceneUbo.lightPosition.xyz - position);

	// Diffuse lighting
	float diff = max(dot(normal, lightDir), 0.0);

	// Specular lighting (Phong)
	vec3 viewDir = normalize(sceneUbo.viewPosition.xyz - position);
	vec3 reflectDir = reflect(-lightDir, normal);
	float spec = pow(max(dot(viewDir, reflectDir), 0.0), 32.0);

	// Attenuation
	float attenuation = 1.0 / (distance * distance);

	// Combine lighting components
	vec3 diffuse = diff * sceneUbo.lightColor.rgb * sceneUbo.lightIntensity.rgb;
	vec3 specular = spec * sceneUbo.lightColor.rgb * sceneUbo.lightIntensity.rgb;
	vec3 finalColor = (diffuse + specular) * albedo * attenuation;

	// Output final color
	outFragColor = vec4(finalColor, 1.0);
}
