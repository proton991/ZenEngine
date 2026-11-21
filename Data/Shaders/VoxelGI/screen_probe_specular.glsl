layout(std430, set = 1, binding = 93) readonly buffer FilteredScreenProbeSpecular
{
    uvec2 filteredProbeSpecularData[];
};
// Octahedral edge extension reflects across the adjoining edge, including corner wrapping.
ivec2 ProbeWrapOct(ivec2 pixel, int side)
{
    if (pixel.x < 0 || pixel.x >= side)
    {
        pixel = ivec2(pixel.x < 0 ? -pixel.x - 1 : 2 * side - pixel.x - 1, side - pixel.y - 1);
    }
    if (pixel.y < 0 || pixel.y >= side)
    {
        pixel = ivec2(side - pixel.x - 1, pixel.y < 0 ? -pixel.y - 1 : 2 * side - pixel.y - 1);
    }
    return clamp(pixel, ivec2(0), ivec2(side - 1));
}
vec4 ProbeSpecularLevel(uint index, vec2 uv, uint level)
{
    uint  offset     = level == 0u ? 0u : 64u + (level - 1u) * 16u;
    int   side       = level == 0u ? 8 : 4;
    vec2  coordinate = uv * float(side) - .5;
    ivec2 base       = ivec2(floor(coordinate));
    vec2  fraction   = fract(coordinate);
    vec4  value      = vec4(0);
    for (uint tap = 0u; tap < 4u; ++tap)
    {
        ivec2 delta = ivec2(tap & 1u, tap >> 1u);
        ivec2 pixel = ProbeWrapOct(base + delta, side);
        uvec2 packed =
            filteredProbeSpecularData[index * ZEN_SCREEN_PROBE_SPECULAR_TEXELS + offset + uint(pixel.x + pixel.y * side)];
        vec2 weight  = mix(vec2(1) - fraction, fraction, vec2(delta));
        value       += vec4(unpackHalf2x16(packed.x), unpackHalf2x16(packed.y)) * weight.x * weight.y;
    }
    return value;
}
vec4 ProbeSpecular(uint index, vec3 direction, float roughness)
{
    float level = clamp(roughness * 4.0 - 1.0, 0.0, 3.0);
    vec2  uv    = EncodeGBufferNormal(direction);
    return mix(ProbeSpecularLevel(index, uv, uint(level)), ProbeSpecularLevel(index, uv, min(uint(level) + 1u, 3u)),
               fract(level));
}
