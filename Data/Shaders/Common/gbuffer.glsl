#ifndef ZEN_GBUFFER_GLSL
#define ZEN_GBUFFER_GLSL

vec2 GBufferSignNotZero(vec2 value)
{
    // sign(0) would collapse the negative-Z pole and octahedron seams.
    return mix(vec2(-1.0), vec2(1.0), greaterThanEqual(value, vec2(0.0)));
}

vec2 EncodeGBufferNormal(vec3 normal)
{
    normal /= abs(normal.x) + abs(normal.y) + abs(normal.z);
    vec2 encoded = normal.xy;
    if (normal.z < 0.0) encoded = (1.0 - abs(encoded.yx)) * GBufferSignNotZero(encoded);
    return encoded * 0.5 + 0.5;
}

vec3 DecodeGBufferNormal(vec2 encoded)
{
    vec2 folded = encoded * 2.0 - 1.0;
    vec3 normal = vec3(folded, 1.0 - abs(folded.x) - abs(folded.y));
    normal.xy -= GBufferSignNotZero(normal.xy) * clamp(-normal.z, 0.0, 1.0);
    return normalize(normal);
}

vec3 ReconstructGBufferPosition(vec2 pixelCenter, ivec2 extent, float depth, mat4 inverseViewProjection)
{
    // The camera projection already flips Y. Vulkan device Z is in [0,1].
    vec2 ndc = pixelCenter / vec2(extent) * 2.0 - 1.0;
    vec4 position = inverseViewProjection * vec4(ndc, depth, 1.0);
    return position.xyz / position.w;
}

#endif
