#ifndef ZEN_LINEAR_TO_SRGB_GLSL
#define ZEN_LINEAR_TO_SRGB_GLSL
vec3 LinearToSRGB(vec3 value)
{
    value = max(value, vec3(0.0));
    return mix(1.055 * pow(value, vec3(1.0 / 2.4)) - 0.055,
               12.92 * value, lessThanEqual(value, vec3(0.0031308)));
}
#endif
