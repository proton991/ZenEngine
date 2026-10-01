#ifndef ZEN_MATERIAL_SHADOW_GLSL
#define ZEN_MATERIAL_SHADOW_GLSL
// Scalar depth maps cannot retain chromatic transmittance. Stable stochastic
// coverage preserves the transmitted luminance under the existing PCF/EVSM filter.
float MaterialShadowCoverage(Material material, vec4 albedo, vec2 uv0, vec2 uv1,
                             vec3 position, vec3 lightVector, vec3 modelScale)
{
    float alpha = material.surfaceProperties.y == 2.0 ? clamp(albedo.a, 0.0, 1.0) : 1.0;
    float transmission = material.sheenColorTransmission.w * MaterialFeatureTexture(material, 9, uv0, uv1).r;
    if (transmission > 0.0)
    {
        vec3 geometric = cross(dFdx(position), dFdy(position));
        vec3 L = dot(lightVector, lightVector) > 1e-12 ? normalize(lightVector) : vec3(0, 0, 1);
        vec3 N = dot(geometric, geometric) > 1e-20 ? normalize(geometric) : L;
        if (dot(N, L) < 0.0) N = -N;
        float cosine = clamp(dot(N, L), 0.0, 1.0);
        bool infiniteIor = material.specularColorIor.w == 0.0;
        float ior = infiniteIor ? 1e6 : max(material.specularColorIor.w, 1.0);
        float dielectricF0 = infiniteIor ? 1.0 : pow((ior - 1.0) / (ior + 1.0), 2.0);
        float specularWeight = material.clearcoatSheenSpecular.w * MaterialFeatureTexture(material, 0, uv0, uv1).a;
        vec3 specularColor = material.specularColorIor.rgb * MaterialFeatureTexture(material, 1, uv0, uv1).rgb;
        vec3 f0 = min(vec3(dielectricF0) * specularColor, vec3(1.0)) * specularWeight;
        vec3 f90 = infiniteIor ? min(specularColor, vec3(1.0)) * specularWeight : vec3(specularWeight);
        vec3 fresnel = f0 + (f90 - f0) * pow(1.0 - cosine, 5.0);
        float metal = clamp(material.metallicFactor * MaterialSlotTexture(material, 1, material.mrTexIndex,
            MaterialTransformedUV(material, 1, material.mrTexSet, uv0, uv1)).b, 0.0, 1.0);
        float thickness = material.volumeIridescence.x * MaterialFeatureTexture(material, 10, uv0, uv1).g;
        float distance = length(refract(-L, N, 1.0 / ior) * thickness * modelScale);
        vec3 attenuation = material.volumeIridescence.y > 0.0 ?
            pow(max(material.attenuationColorDispersion.rgb, vec3(1e-6)), vec3(distance / material.volumeIridescence.y)) : vec3(1.0);
        float clearcoat = material.clearcoatSheenSpecular.x * MaterialFeatureTexture(material, 4, uv0, uv1).r;
        float coatFresnel = 0.04 + 0.96 * pow(1.0 - cosine, 5.0);
        vec3 transmitted = transmission * (1.0 - metal) * albedo.rgb * (vec3(1.0) - fresnel) * attenuation *
            pow(1.0 - clearcoat * coatFresnel, 2.0);
        alpha *= 1.0 - clamp(dot(transmitted, vec3(0.2126, 0.7152, 0.0722)), 0.0, 1.0);
    }
    return clamp(alpha, 0.0, 1.0);
}

float MaterialShadowNoise(uint nodeIndex, uint materialIndex)
{
    uvec2 texel = uvec2(gl_FragCoord.xy);
    uint seed = texel.x * 1973u + texel.y * 9277u + nodeIndex * 26699u + materialIndex * 31847u;
    // Independent layer phases avoid treating multiple transparent shells as one.
    seed ^= uint(gl_FragCoord.z * 65535.0) * 1013u;
    seed ^= seed >> 16;
    seed *= 0x7feb352du;
    seed ^= seed >> 15;
    seed *= 0x846ca68bu;
    seed ^= seed >> 16;
    return float(seed >> 8) * (1.0 / 16777216.0);
}
#endif
