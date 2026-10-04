#include "../Common/material_surface.glsl"
#include "../Common/material_lighting.glsl"
#include "../Common/scene_lighting.glsl"
#ifdef VOXEL_GI
#include "../VoxelGI/cone_trace.glsl"
#include "../ShadowMapping/scene_shadows.glsl"
#endif
layout(set=1,binding=0) uniform uCameraData { mat4 uProjViewMatrix; mat4 uProjMatrix; mat4 uViewMatrix; };
struct NodeData { mat4 modelMatrix; mat4 normalMatrix; vec4 surfaceScale; };
layout(std140,set=1,binding=1) readonly buffer NodeBuffer { NodeData nodesData[]; };
layout(std140,set=1,binding=2) readonly buffer MaterialBuffer { Material materialData[]; };
layout(set=3,binding=1) uniform samplerCube envIrradianceMap;
layout(set=3,binding=2) uniform samplerCube envPrefilteredMap;
layout(set=3,binding=3) uniform sampler2D lutBRDFMap;
layout(set=3,binding=4) uniform sampler2D opaqueColorMap;
layout(set=3,binding=5) uniform samplerCube envSourceMap;
layout(set=3,binding=6) uniform sampler2D scatterLightingMap;
layout(set=3,binding=7) uniform sampler2D scatterPositionMap;
layout(push_constant) uniform Constants { uint uNodeIndex; uint uMaterialIndex; uint uTopology; };
layout(location=0) out vec4 outColor;

vec3 Prefiltered(vec3 direction, float roughness)
{
    return textureLod(envPrefilteredMap, EnvironmentDirection(direction), roughness * float(textureQueryLevels(envPrefilteredMap) - 1)).rgb * sceneUbo.environment.x * sceneUbo.environment.z;
}

vec3 SheenEnvironment(MaterialSurface s, vec3 N, vec3 V)
{
    vec3 result = vec3(0.0);
    if (max(max(s.sheenColor.r, s.sheenColor.g), s.sheenColor.b) > 0.0)
    {
        vec3 axis = abs(N.z) < 0.999 ? vec3(0,0,1) : vec3(0,1,0);
        vec3 T = normalize(cross(axis, N));
        vec3 B = cross(N, T);
        float nv = max(dot(N, V), 1e-5);
        for (uint sampleIndex = 0; sampleIndex < 64; ++sampleIndex)
        {
            float u = (float(sampleIndex) + 0.5) / 64.0;
            float phi = float(sampleIndex) * 2.399963229728653;
            float sinTheta = pow(u, (s.sheenRoughness * s.sheenRoughness) / (1.0 + 2.0 * s.sheenRoughness * s.sheenRoughness));
            float cosTheta = sqrt(max(1.0 - sinTheta * sinTheta, 0.0));
            vec3 H = T * (sinTheta * cos(phi)) + B * (sinTheta * sin(phi)) + N * cosTheta;
            float vh = max(dot(V, H), 0.0);
            vec3 L = reflect(-V, H);
            float nl = max(dot(N, L), 0.0);
            if (nl > 0.0 && vh > 0.0)
                result += textureLod(envSourceMap, EnvironmentSourceDirection(L), 0.0).rgb * V_Sheen(nl, nv, s.sheenRoughness) * nl * 4.0 * vh / max(cosTheta, 1e-5);
        }
        result *= s.sheenColor * sceneUbo.environment.x * sceneUbo.environment.z / 64.0;
    }
    return result;
}

vec3 Refraction(MaterialSurface s, vec3 N, vec3 V)
{
    vec3 modelScale = nodesData[uNodeIndex].surfaceScale.xyz;
    float halfSpread = (s.ior - 1.0) * 0.025 * s.dispersion;
    vec3 iors = vec3(max(s.ior - halfSpread, 1.0), s.ior, s.ior + halfSpread);
    vec3 transmitted = vec3(0.0);
    float distance = 0.0;
    for (int channel = 0; channel < 3; ++channel)
    {
        vec3 ray = refract(-V, N, 1.0 / iors[channel]) * s.thickness * modelScale;
        if (channel == 1) distance = length(ray);
        vec4 projected = uProjViewMatrix * vec4(inWorldPos + ray, 1.0);
        vec2 uv = projected.xy / max(projected.w, 1e-6) * 0.5 + 0.5;
        float lod = log2(float(textureSize(opaqueColorMap, 0).x)) * s.roughness * clamp(2.0 * iors[channel] - 2.0, 0.0, 1.0);
        transmitted[channel] = projected.w > 0.0 && all(greaterThanEqual(uv, vec2(0))) && all(lessThanEqual(uv, vec2(1))) ?
            textureLod(opaqueColorMap, uv, lod)[channel] : Prefiltered(SafeNormalize(ray, -V), s.roughness)[channel];
    }
    return transmitted * s.diffuse * VolumeAttenuation(s, distance);
}

// Burley diffusion profile and single-scattering conversion follow the
// Khronos Sample Renderer implementation of KHR_materials_volume_scatter.
vec3 GatherVolumeScatter(MaterialSurface s)
{
    vec3 radius = s.attenuationDistance * s.multiscatterColor;
    float maximum = max(max(radius.r, radius.g), radius.b);
    vec2 texel = 1.0 / vec2(textureSize(scatterLightingMap, 0));
    vec2 uv = gl_FragCoord.xy * texel;
    vec4 center = textureLod(scatterLightingMap, uv, 0.0);
    vec4 centerPosition = textureLod(scatterPositionMap, uv, 0.0);
    vec4 offset = inverse(uProjViewMatrix) * vec4((uv + vec2(texel.x, 0)) * 2.0 - 1.0, gl_FragCoord.z, 1.0);
    float metersPerPixel = distance(inWorldPos, offset.xyz / offset.w);
    vec3 result = center.rgb;
    if (maximum > metersPerPixel && metersPerPixel > 1e-8)
    {
        vec3 d = max(radius, vec3(maximum * 0.001)) * (0.25 / M_PI) / 1.04;
        float largestD = max(max(d.r, d.g), d.b);
        vec3 accumulated = vec3(0.0);
        vec3 weights = vec3(0.0);
        for (uint sampleIndex = 0; sampleIndex < 64; ++sampleIndex)
        {
            float u = (float(sampleIndex) + 0.5) / 64.0;
            float r = u < 0.25 ? -largestD * log(1.0 - u * 4.0) :
                -3.0 * largestD * log(1.0 - (u - 0.25) / 0.75);
            float phi = float(sampleIndex) * 2.399963229728653;
            vec2 sampleUV = uv + vec2(cos(phi), sin(phi)) * r / metersPerPixel * texel;
            if (any(lessThan(sampleUV, vec2(0))) || any(greaterThan(sampleUV, vec2(1)))) continue;
            vec4 lighting = textureLod(scatterLightingMap, sampleUV, 0.0);
            vec4 position = textureLod(scatterPositionMap, sampleUV, 0.0);
            if (lighting.w != center.w || position.w != centerPosition.w) continue;
            float worldDistance = distance(position.xyz, centerPosition.xyz);
            vec3 profile = (exp(-worldDistance / d) + exp(-worldDistance / (3.0 * d))) / (4.0 * d);
            float pdf = (exp(-r / largestD) + exp(-r / (3.0 * largestD))) / (4.0 * largestD);
            vec3 weight = profile / max(pdf, 1e-8);
            accumulated += lighting.rgb * weight;
            weights += weight;
        }
        result = accumulated / max(weights, vec3(1e-8));
    }
    return result * s.diffuseTransmissionColor;
}

void main()
{
    uint topology = uTopology & 3u;
    bool displayLinear = (uTopology & 4u) != 0u;
    Material material = materialData[uMaterialIndex];
    vec3 N = SurfaceNormal(material);
    vec3 coatN = ClearcoatNormal(material);
    bool backface = material.materialProperties.z > 0.5 && !(gl_FrontFacing == (inOrientation >= 0.0));
    // Missing mesh tangents use the normal texture's authored tangent space.
    vec2 tangentUV = material.normalTexIndex >= 0 ?
        MaterialTransformedUV(material, 2, material.normalTexSet, inUV, inUV1) : MaterialBindingUV(0);
    mat3 basis = SurfaceBasis(N, tangentUV, true, backface);
    MaterialSurface s = ReadMaterialSurface(material);
    s.modelScale = nodesData[uNodeIndex].surfaceScale.xyz;
    if (!MaterialVisible(material, s.baseColor.a) || (topology == 2 &&
        (material.materialProperties.z < 0.5 || material.volumeIridescence.x > 0.0) && !(gl_FrontFacing == (inOrientation >= 0.0)))) discard;
    float alpha = material.surfaceProperties.y == 2.0 ? s.baseColor.a : 1.0;
    vec3 color = s.baseColor.rgb;
    if (material.materialProperties.y < 0.5 && (topology == 2 || dot(inNormal, inNormal) > 1e-12))
    {
        vec3 V = uProjMatrix[3][3] > 0.5 ? normalize(transpose(mat3(uViewMatrix))[2]) :
            SafeNormalize(sceneUbo.viewPosition.xyz - inWorldPos, N);
        vec3 T = SafeNormalize(basis[0] * s.anisotropyDirection.x + basis[1] * s.anisotropyDirection.y, basis[0]);
        vec3 B = cross(N, T);
        float nv = max(dot(N, V), 1e-5);
        vec2 brdf = texture(lutBRDFMap, vec2(nv, s.roughness)).rg;
        vec3 iblF = SurfaceIBLFresnel(s, nv, brdf);
        vec3 irradiance = texture(envIrradianceMap, EnvironmentDirection(N)).rgb * sceneUbo.environment.x * sceneUbo.environment.z;
#ifdef VOXEL_GI
        irradiance = DiffuseVoxelLighting(inWorldPos, N);
#endif
        color = (vec3(1.0) - iblF) * irradiance * s.diffuse * (1.0 - s.transmission) * (1.0 - s.diffuseTransmission);
        vec3 reflectionN = N;
        if (s.anisotropy > 0.0)
        {
            vec3 anisotropicN = SafeNormalize(cross(cross(B, V), B), N);
            float bend = 1.0 - s.anisotropy * (1.0 - s.roughness);
            reflectionN = SafeNormalize(mix(anisotropicN, N, bend * bend * bend * bend), N);
        }
        color += Prefiltered(reflect(-V, reflectionN), s.roughness) * iblF;
        if (s.retroreflection > 0.0)
        {
            vec3 retroV = reflect(-V, N);
            vec3 retroN = N;
            if (s.anisotropy > 0.0)
            {
                vec3 bent = SafeNormalize(cross(cross(B, retroV), B), N);
                float bend = 1.0 - s.anisotropy * (1.0 - s.roughness);
                retroN = SafeNormalize(mix(bent, N, bend * bend * bend * bend), N);
            }
            color += s.retroreflection * (Prefiltered(reflect(-retroV, retroN), s.roughness) -
                Prefiltered(reflect(-V, reflectionN), s.roughness)) * iblF;
        }
        if (s.diffuseTransmission > 0.0)
        {
            vec3 backIrradiance = texture(envIrradianceMap, EnvironmentDirection(-N)).rgb * sceneUbo.environment.x * sceneUbo.environment.z;
            color += (vec3(1.0) - iblF) * backIrradiance * (1.0 - s.metallic) * (1.0 - s.transmission) * s.diffuseTransmissionColor * s.diffuseTransmission * VolumeAttenuation(s, DiffuseVolumeThickness(s)) *
                (vec3(1.0) - MultiscatterToSingleScatter(s.multiscatterColor));
        }
        if (s.transmission > 0.0) color += (vec3(1.0) - iblF) * Refraction(s, N, V) * s.transmission;
        color = color * SheenLayerScale(s, nv, nv) + SheenEnvironment(s, N, V);
        color *= s.ao;
        if (s.clearcoat > 0.0)
        {
            float coatNV = max(dot(coatN, V), 1e-5);
            vec3 coatF = F_Schlick(vec3(0.04), coatNV);
            vec2 coatBRDF = texture(lutBRDFMap, vec2(coatNV, s.clearcoatRoughness)).rg;
            color = color * (vec3(1.0) - s.clearcoat * coatF) + s.clearcoat * Prefiltered(reflect(-V, coatN), s.clearcoatRoughness) * (coatF * coatBRDF.x + coatBRDF.y) * s.ao;
        }
        for (int i = 0; i <= int(sceneUbo.lightInfo.x); ++i)
        {
            vec3 L;
            float distanceToLight;
            vec3 radiance = EvaluateLight(i == int(sceneUbo.lightInfo.x) ? sceneUbo.cameraLight : sceneUbo.lights[i], inWorldPos, L, distanceToLight);
            float visibility = 1.0;
#ifdef VOXEL_GI
            visibility = SceneLightVisibility(i == int(sceneUbo.lightInfo.x) ? MAX_SCENE_LIGHTS : i, inWorldPos, GeometricNormal(material));
#endif
            color += SurfaceDirect(s, N, coatN, T, B, V, L) * radiance * visibility;
        }
        if (max(max(s.multiscatterColor.r, s.multiscatterColor.g), s.multiscatterColor.b) > 0.0 && s.diffuseTransmission > 0.0)
            color += GatherVolumeScatter(s) * (1.0 - s.metallic) * (1.0 - s.transmission) *
                (vec3(1.0) - s.clearcoat * F_Schlick(vec3(0.04), max(dot(coatN, V), 0.0)));
        vec3 emissionLayer = s.emissive;
        if (s.clearcoat > 0.0) emissionLayer *= vec3(1.0) - s.clearcoat * F_Schlick(vec3(0.04), max(dot(coatN, V), 0.0));
        color += emissionLayer;
    }
    else if (topology != 2 && material.materialProperties.y < 0.5) color += s.emissive;
    // Unlit base colors are display-referred. Encode the inverse tone curve so
    // the common HDR composition preserves their authored colors.
    if (material.materialProperties.y > 0.5 && !displayLinear)
        color = clamp(color, 0.0, 1.0) / max(vec3(1.0) - clamp(color, 0.0, 1.0), vec3(1.0 / 65504.0));
    else if (material.materialProperties.y < 0.5 && displayLinear)
    {
        color = clamp(color, 0.0, 65504.0);
        color = color / (color + vec3(1.0));
    }
    outColor = vec4(clamp(color, 0.0, 65504.0), clamp(alpha, 0.0, 1.0));
}
