#ifndef ZEN_HYBRID_COMMON
#define ZEN_HYBRID_COMMON
#include "Graphics/Shared/HybridGI.h"
float HybridLuminance(vec3 rgb)
{
    return dot(rgb, vec3(0.2126, 0.7152, 0.0722));
}
float HybridRoughnessTolerance(float roughness, bool reducedResolution)
{
    // Coarse receiver texels sample different points of a roughness texture after reprojection.
    // Broad lobes can share history/filter taps within the spatial reservoir's relative tolerance;
    // glossy lobes and native-resolution receivers retain the narrow original threshold.
    return reducedResolution ? max(0.02, 0.15 * roughness) : 0.02;
}

// Scale that bounds a GI sample without history to ZEN_HYBRID_SAMPLE_BOUND times the luminance of
// a local reference from receivers that have history; one without a reference (negative).
float HybridSampleBound(float luminance, float reference)
{
    float limit = ZEN_HYBRID_SAMPLE_BOUND * reference;
    return reference >= 0.0 && luminance > limit ? limit / luminance : 1.0;
}

// Whether a blended layer rests on the opaque receiver behind it. fragment and receiver are view-space
// positions on the same view ray, receiverNormal the receiver's geometric normal in view space,
// normalAgreement the dot of both layers' geometric normals (decal normals are often flipped).
// Voxel GI accepts half a voxel, which it could not resolve anyway. Ray-traced GI uses the
// reconstruction's own limit: plane distance from the receiver, scaled by view distance.
bool HybridLayerRests(vec3 fragment, vec3 receiver, vec3 receiverNormal, float normalAgreement, bool tracedGI, float halfVoxel)
{
    float offset = tracedGI ? abs(dot(receiver - fragment, receiverNormal)) : distance(receiver, fragment);
    float limit  = tracedGI ? ZEN_HYBRID_PLANE_REJECTION * max(1.0, length(fragment)) : halfVoxel;
    return offset < limit && abs(normalAgreement) > 0.5;
}

// Whether a resting layer may reuse the receiver's reflection: a similar shading normal, and either
// two broad lobes (both at least 0.5 rough; New Sponza decals: +0.05% bias against their own
// traced reflections) or the roughness tolerance of reduced-resolution reflection reuse.
bool HybridLayerLobeMatches(vec3 receiverNormal, float receiverRoughness, vec3 normal, float roughness)
{
    bool similar = min(receiverRoughness, roughness) >= 0.5
                || abs(receiverRoughness - roughness) < HybridRoughnessTolerance(roughness, true);
    return similar && dot(receiverNormal, normal) > mix(0.99, 0.9, roughness * roughness);
}
#endif
