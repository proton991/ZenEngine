#ifndef ZEN_PROBE_PIXEL
#define ZEN_PROBE_PIXEL
#include "probe_common.glsl"

// The current receiver guides are shared by reconstruction and pixel history validation.
struct ProbeReceiver
{
    bool  valid;
    vec3  position;
    vec3  geometric;
    vec3  normal;
    uvec2 surface;
    float roughness;
    vec3  view;
};

ProbeReceiver ProbeLoadReceiver(ivec2 pixel)
{
    ProbeReceiver receiver;
    receiver.valid     = ProbeValidReceiver(pixel);
    receiver.position  = vec3(0);
    receiver.geometric = vec3(0);
    receiver.normal    = vec3(0);
    receiver.surface   = uvec2(0);
    receiver.roughness = 0.0;
    receiver.view      = vec3(0);
    if (receiver.valid)
    {
        receiver.position  = HybridPosition(pixel);
        receiver.geometric = HybridGeometricNormal(pixel);
        receiver.normal    = HybridNormal(pixel);
        receiver.surface   = texelFetch(receiverSurface, pixel, 0).rg;
        receiver.roughness = clamp(texelFetch(receiverRoughness, pixel, 0).g, 0.04, 1.0);
        receiver.view      = sceneUbo.viewPosition.w < 0.5 ? normalize(sceneUbo.lightInfo.yzw)
                                                           : normalize(sceneUbo.viewPosition.xyz - receiver.position);
    }
    return receiver;
}
#endif
