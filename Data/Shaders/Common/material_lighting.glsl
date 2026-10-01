#ifndef ZEN_MATERIAL_LIGHTING_GLSL
#define ZEN_MATERIAL_LIGHTING_GLSL
const float M_PI = 3.141592653589793;
float sq(float value) { return value * value; }
vec3 sq(vec3 value) { return value * value; }
#define MATERIAL_ANISOTROPY
// Khronos glTF Sample Renderer, Apache-2.0; see Khronos-LICENSE.md.
#include "gltf_brdf.glsl"
#include "gltf_iridescence.glsl"

struct MaterialSurface
{
    vec4 baseColor;
    vec3 f0;
    vec3 f90;
    vec3 diffuse;
    vec3 emissive;
    vec3 sheenColor;
    vec3 attenuationColor;
    vec3 diffuseTransmissionColor;
    float metallic;
    float roughness;
    float ao;
    float clearcoat;
    float clearcoatRoughness;
    float sheenRoughness;
    float transmission;
    float thickness;
    vec3 modelScale;
    float attenuationDistance;
    float ior;
    float dispersion;
    float iridescence;
    float iridescenceIor;
    float iridescenceThickness;
    float anisotropy;
    vec2 anisotropyDirection;
    float diffuseTransmission;
    vec3 multiscatterColor;
    float retroreflection;
};

MaterialSurface ReadMaterialSurface(Material material)
{
    MaterialSurface s;
    s.baseColor = MaterialAlbedo(material, inUV, inUV1, inColor);
    vec4 mr = MaterialSlotTexture(material, 1, material.mrTexIndex,
        MaterialTransformedUV(material, 1, material.mrTexSet, inUV, inUV1));
    s.metallic = clamp(material.metallicFactor * mr.b, 0.0, 1.0);
    s.roughness = clamp(material.roughnessFactor * mr.g, 0.001, 1.0);
    bool infiniteIor = material.specularColorIor.w == 0.0;
    s.ior = infiniteIor ? 1e6 : max(material.specularColorIor.w, 1.0);
    float dielectricF0 = infiniteIor ? 1.0 : sq((s.ior - 1.0) / (s.ior + 1.0));
    float specularWeight = material.clearcoatSheenSpecular.w * MaterialFeatureTexture(material, 0, inUV, inUV1).a;
    vec3 specularColor = material.specularColorIor.rgb * MaterialFeatureTexture(material, 1, inUV, inUV1).rgb;
    s.f0 = mix(min(vec3(dielectricF0) * specularColor, vec3(1.0)) * specularWeight, s.baseColor.rgb, s.metallic);
    s.f90 = mix(infiniteIor ? min(specularColor, vec3(1.0)) * specularWeight : vec3(specularWeight), vec3(1.0), s.metallic);
    s.diffuse = s.baseColor.rgb * (1.0 - s.metallic);
    if (material.surfaceProperties.w > 0.5)
    {
        s.baseColor = material.diffuseFactor * inColor * MaterialFeatureTexture(material, 2, inUV, inUV1);
        vec4 specGloss = MaterialFeatureTexture(material, 3, inUV, inUV1);
        s.f0 = material.specularGlossiness.rgb * specGloss.rgb;
        s.f90 = vec3(1.0);
        s.metallic = 0.0;
        s.roughness = clamp(1.0 - material.specularGlossiness.w * specGloss.a, 0.001, 1.0);
        s.diffuse = s.baseColor.rgb * (1.0 - max(max(s.f0.r, s.f0.g), s.f0.b));
    }
    s.emissive = material.emissiveFactor.rgb * MaterialSlotTexture(material, 4, material.emissiveTexIndex,
        MaterialTransformedUV(material, 4, material.emissiveTexSet, inUV, inUV1)).rgb;
    s.ao = mix(1.0, MaterialSlotTexture(material, 3, material.occlusionTexIndex,
        MaterialTransformedUV(material, 3, material.aoTexSet, inUV, inUV1)).r, material.materialProperties.x);
    s.clearcoat = material.clearcoatSheenSpecular.x * MaterialFeatureTexture(material, 4, inUV, inUV1).r;
    s.clearcoatRoughness = clamp(material.clearcoatSheenSpecular.y * MaterialFeatureTexture(material, 5, inUV, inUV1).g, 0.001, 1.0);
    s.sheenColor = material.sheenColorTransmission.rgb * MaterialFeatureTexture(material, 7, inUV, inUV1).rgb;
    s.sheenRoughness = clamp(material.clearcoatSheenSpecular.z * MaterialFeatureTexture(material, 8, inUV, inUV1).a, 0.001, 1.0);
    s.transmission = material.sheenColorTransmission.w * MaterialFeatureTexture(material, 9, inUV, inUV1).r;
    s.thickness = material.volumeIridescence.x * MaterialFeatureTexture(material, 10, inUV, inUV1).g;
    s.modelScale = vec3(1.0);
    s.attenuationDistance = material.volumeIridescence.y;
    s.attenuationColor = material.attenuationColorDispersion.rgb;
    s.dispersion = infiniteIor ? 0.0 : material.attenuationColorDispersion.w;
    s.iridescence = material.volumeIridescence.z * MaterialFeatureTexture(material, 11, inUV, inUV1).r;
    s.iridescenceIor = material.volumeIridescence.w;
    float thicknessWeight = material.featureTextures[12].properties.x >= 0.0 ? MaterialFeatureTexture(material, 12, inUV, inUV1).g : 1.0;
    s.iridescenceThickness = mix(material.iridescenceAnisotropy.x, material.iridescenceAnisotropy.y, thicknessWeight);
    s.anisotropy = material.iridescenceAnisotropy.z;
    vec2 direction = vec2(1, 0);
    if (material.featureTextures[13].properties.x >= 0.0)
    {
        vec3 texel = MaterialFeatureTexture(material, 13, inUV, inUV1).rgb;
        direction = texel.rg * 2.0 - 1.0;
        direction = dot(direction, direction) > 1e-8 ? normalize(direction) : vec2(1, 0);
        s.anisotropy *= texel.b;
    }
    float rotation = material.iridescenceAnisotropy.w;
    s.anisotropyDirection = mat2(cos(rotation), sin(rotation), -sin(rotation), cos(rotation)) * direction;
    s.diffuseTransmission = material.diffuseTransmissionColorFactor.w * MaterialFeatureTexture(material, 14, inUV, inUV1).a;
    s.diffuseTransmissionColor = material.diffuseTransmissionColorFactor.rgb * MaterialFeatureTexture(material, 15, inUV, inUV1).rgb;
    s.multiscatterColor = material.volumeScatterColorRetroreflection.rgb;
    s.retroreflection = material.volumeScatterColorRetroreflection.w * MaterialFeatureTexture(material, 16, inUV, inUV1).r;
    return s;
}

vec3 MultiscatterToSingleScatter(vec3 color)
{
    vec3 transformed = 4.09712 + 4.20863 * color - sqrt(9.59217 + 41.6808 * color + 17.7126 * color * color);
    return clamp(vec3(1.0) - transformed * transformed, 0.0, 1.0);
}

vec3 SurfaceFresnel(MaterialSurface s, float cosine)
{
    vec3 fresnel = F_Schlick(s.f0, s.f90, cosine);
    if (s.iridescence > 0.0 && s.iridescenceThickness > 0.0)
        fresnel = mix(fresnel, evalIridescence(1.0, s.iridescenceIor, cosine, s.iridescenceThickness, s.f0), s.iridescence);
    return fresnel;
}

vec3 SurfaceIBLFresnel(MaterialSurface s, float nv, vec2 brdf)
{
    vec3 grazing = min(s.f90, max(vec3(1.0 - s.roughness), s.f0));
    vec3 reflected = s.f0 + (grazing - s.f0) * pow(1.0 - nv, 5.0);
    vec3 single = reflected * brdf.x + s.f90 * brdf.y;
    float escaped = max(1.0 - brdf.x - brdf.y, 0.0);
    vec3 average = s.f0 + (s.f90 - s.f0) / 21.0;
    vec3 multiple = escaped * single * average / max(vec3(1.0) - average * escaped, vec3(1e-6));
    vec3 result = single + multiple;
    if (s.iridescence > 0.0 && s.iridescenceThickness > 0.0)
        result = mix(result, evalIridescence(1.0, s.iridescenceIor, nv, s.iridescenceThickness, s.f0), s.iridescence);
    return result;
}

vec3 VolumeAttenuation(MaterialSurface s, float distance)
{
    return s.attenuationDistance > 0.0 ? pow(max(s.attenuationColor, vec3(1e-6)), vec3(distance / s.attenuationDistance)) : vec3(1.0);
}

float DiffuseVolumeThickness(MaterialSurface s)
{
    return s.thickness * (s.modelScale.x + s.modelScale.y + s.modelScale.z) / 3.0;
}

// Directional Charlie albedo. Integrating the same BRDF keeps sheen energy
// compensation consistent with the environment integral without a separate LUT.
float SheenDirectionalAlbedo(float nv, float roughness)
{
    vec3 V = vec3(sqrt(max(1.0 - nv * nv, 0.0)), 0.0, nv);
    float result = 0.0;
    float alpha = roughness * roughness;
    for (uint sampleIndex = 0; sampleIndex < 32; ++sampleIndex)
    {
        float u = (float(sampleIndex) + 0.5) / 32.0;
        float phi = float(sampleIndex) * 2.399963229728653;
        float sinTheta = pow(u, alpha / (1.0 + 2.0 * alpha));
        float cosTheta = sqrt(max(1.0 - sinTheta * sinTheta, 0.0));
        vec3 H = vec3(sinTheta * cos(phi), sinTheta * sin(phi), cosTheta);
        float vh = max(dot(V, H), 0.0);
        vec3 L = reflect(-V, H);
        float nl = max(L.z, 0.0);
        if (nl > 0.0 && vh > 0.0)
            result += V_Sheen(nl, max(nv, 1e-5), roughness) * nl * 4.0 * vh / max(cosTheta, 1e-5);
    }
    return clamp(result / 32.0, 0.0, 1.0);
}

float SheenLayerScale(MaterialSurface s, float nv, float nl)
{
    float sheen = max(max(s.sheenColor.r, s.sheenColor.g), s.sheenColor.b);
    float scale = 1.0;
    if (sheen > 0.0)
        scale = 1.0 - sheen * max(SheenDirectionalAlbedo(nv, s.sheenRoughness),
                                 SheenDirectionalAlbedo(nl, s.sheenRoughness));
    return clamp(scale, 0.0, 1.0);
}

vec3 SurfaceDirect(MaterialSurface s, vec3 N, vec3 coatN, vec3 T, vec3 B, vec3 V, vec3 L)
{
    vec3 H = SafeNormalize(V + L, N);
    float nl = max(dot(N, L), 0.0);
    float nv = max(dot(N, V), 1e-5);
    float nh = max(dot(N, H), 0.0);
    float vh = max(dot(V, H), 0.0);
    vec3 F = SurfaceFresnel(s, vh);
    float alpha = s.roughness * s.roughness;
    vec3 brdf = s.anisotropy > 0.0 ? BRDF_specularGGXAnisotropy(alpha, s.anisotropy, N, V, L, H, T, B) : BRDF_specularGGX(alpha, nl, nv, nh);
    vec3 reflection = nl * (F * brdf + (vec3(1.0) - F) * s.diffuse / M_PI * (1.0 - s.transmission) * (1.0 - s.diffuseTransmission));
    if (s.retroreflection > 0.0)
    {
        vec3 retroV = reflect(-V, N);
        vec3 retroH = SafeNormalize(retroV + L, N);
        vec3 retroF = SurfaceFresnel(s, max(dot(retroV, retroH), 0.0));
        vec3 retroBRDF = s.anisotropy > 0.0 ? BRDF_specularGGXAnisotropy(alpha, s.anisotropy, N, retroV, L, retroH, T, B) :
            BRDF_specularGGX(alpha, nl, nv, max(dot(N, retroH), 0.0));
        vec3 retroReflection = nl * (retroF * retroBRDF + (vec3(1.0) - retroF) * s.diffuse / M_PI * (1.0 - s.transmission) * (1.0 - s.diffuseTransmission));
        reflection = mix(reflection, retroReflection, s.retroreflection);
    }
    if (s.diffuseTransmission > 0.0)
        reflection += max(dot(-N, L), 0.0) * (vec3(1.0) - SurfaceFresnel(s, nv)) * (1.0 - s.metallic) * (1.0 - s.transmission) * s.diffuseTransmissionColor * s.diffuseTransmission / M_PI * VolumeAttenuation(s, DiffuseVolumeThickness(s)) *
              (vec3(1.0) - MultiscatterToSingleScatter(s.multiscatterColor));
    if (s.transmission > 0.0 && dot(N, L) < 0.0)
    {
        vec3 mirrorL = SafeNormalize(L - 2.0 * N * dot(N, L), N);
        vec3 transmissionH = SafeNormalize(mirrorL + V, N);
        float transmissionRoughness = alpha * clamp(2.0 * s.ior - 2.0, 0.0, 1.0);
        vec3 btdf = BRDF_specularGGX(max(transmissionRoughness, 1e-5), max(dot(N, mirrorL), 0.0), nv, max(dot(N, transmissionH), 0.0));
        vec3 transmissionRay = refract(-mirrorL, N, 1.0 / s.ior) * s.thickness * s.modelScale;
        reflection += (vec3(1.0) - F) * s.diffuse * btdf * max(dot(-N, L), 0.0) * s.transmission * VolumeAttenuation(s, length(transmissionRay));
    }
    if (max(max(s.sheenColor.r, s.sheenColor.g), s.sheenColor.b) > 0.0)
        reflection = reflection * SheenLayerScale(s, nv, nl) + nl * BRDF_specularSheen(s.sheenColor, s.sheenRoughness, nl, nv, nh);
    if (s.clearcoat > 0.0)
    {
        float coatNL = max(dot(coatN, L), 0.0);
        float coatNV = max(dot(coatN, V), 1e-5);
        vec3 coatF = F_Schlick(vec3(0.04), coatNV);
        reflection = reflection * (vec3(1.0) - s.clearcoat * coatF) + s.clearcoat * coatNL * coatF *
            BRDF_specularGGX(s.clearcoatRoughness * s.clearcoatRoughness, coatNL, coatNV, max(dot(coatN, H), 0.0));
    }
    return reflection;
}
#endif
