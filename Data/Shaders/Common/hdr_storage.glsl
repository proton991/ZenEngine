#ifndef ZEN_HDR_STORAGE_GLSL
#define ZEN_HDR_STORAGE_GLSL
vec3 ClampHDRStorage(vec3 radiance)
{
    // Saturate before RGBA16F conversion so HDR overflow cannot seed infinities/NaNs.
    return clamp(mix(radiance, vec3(0.0), isnan(radiance)), 0.0, 65504.0);
}
#endif
