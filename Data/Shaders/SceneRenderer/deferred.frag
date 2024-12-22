#version 450

layout (set = 0, binding = 0) uniform sampler2D samplerPosition;
layout (set = 0, binding = 1) uniform sampler2D samplerNormal;
layout (set = 0, binding = 2) uniform sampler2D samplerAlbedo;

layout (location = 0) in vec2 inUV;

layout (location = 0) out vec4 outFragColor;

layout (set = 1, binding = 0) uniform SceneUBO
{
	vec3 lightPosition;
	vec3 lightColor;
	vec3 lightIntensity;
	vec3 viewPosition;
} sceneUbo;

void main() {
	// Sample G-buffer textures
	vec3 position = texture(samplerPosition, inUV).rgb;
	vec3 normal = normalize(texture(samplerNormal, inUV).rgb);
	vec3 albedo = texture(samplerAlbedo, inUV).rgb;

	// Calculate light direction and distance
	vec3 lightDir = normalize(sceneUbo.lightPosition - position);
	float distance = length(sceneUbo.lightPosition - position);

	// Diffuse lighting
	float diff = max(dot(normal, lightDir), 0.0);

	// Specular lighting (Phong)
	vec3 viewDir = normalize(sceneUbo.viewPosition - position);
	vec3 reflectDir = reflect(-lightDir, normal);
	float spec = pow(max(dot(viewDir, reflectDir), 0.0), 32.0);

	// Attenuation
	float attenuation = 1.0 / (distance * distance);

	// Combine lighting components
	vec3 diffuse = diff * sceneUbo.lightColor * sceneUbo.lightIntensity;
	vec3 specular = spec * sceneUbo.lightColor * sceneUbo.lightIntensity;
	vec3 finalColor = (diffuse + specular) * albedo * attenuation;

	// Output final color
	outFragColor = vec4(diffuse, 1.0);
}
