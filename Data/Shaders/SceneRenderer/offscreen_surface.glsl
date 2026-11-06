#include "../Common/material_surface.glsl"
#include "../Common/hdr_storage.glsl"
#include "../Common/gbuffer.glsl"
layout(location = 0) out vec2 outNormal;
layout(location = 1) out vec4 outAlbedo;
layout(location = 2) out vec4 outMetallicRoughness;
layout(location = 3) out vec4 outEmissiveOcclusion;
#ifdef HYBRID_RECEIVER
layout(location = 4) out uvec2 outReceiver;
layout(location = 5) out vec4 outMotion;
layout(location = 6) out vec4 outPosition;
layout(location = 16) in vec4 inPreviousClip;
layout(location = 17) in vec4 inCurrentClip;
#endif
layout(std140, set = 1, binding = 2) readonly buffer MaterialBuffer
{
    Material materialData[];
};
#ifdef GPU_CULLED_DRAW
layout(location = 18) flat in uvec2 inDrawIndices;
#else
layout(push_constant) uniform Constants
{
    uint uNodeIndex;
    uint uMaterialIndex;
};
#endif


void main()
{
#ifdef GPU_CULLED_DRAW
    uint uNodeIndex     = inDrawIndices.x;
    uint uMaterialIndex = inDrawIndices.y;
#endif
    Material material = materialData[uMaterialIndex];
    vec3     normal   = SurfaceNormal(material);
#ifdef HYBRID_RECEIVER
    vec3 geometric = TriangleNormal(normal);
#endif
    vec4 albedo = MaterialAlbedo(material, inUV, inUV1, inColor);
#ifdef GBUFFER_ALPHA_MASK
    // Specular-glossiness coverage comes from the diffuse texture, as in the forward pass that
    // depth-tests against these receivers.
    float coverage = material.surfaceProperties.w > 0.5
                       ? material.diffuseFactor.a * inColor.a * MaterialFeatureTexture(material, 2, inUV, inUV1).a
                       : albedo.a;
    if (!MaterialVisible(material, coverage))
    {
        discard;
    }
#endif
    vec4  mr        = MaterialSlotTexture(material, 1, material.mrTexIndex,
                                          MaterialTransformedUV(material, 1, material.mrTexSet, inUV, inUV1));
    vec3  emission  = material.emissiveFactor.rgb
                    * MaterialSlotTexture(material, 4, material.emissiveTexIndex,
                                          MaterialTransformedUV(material, 4, material.emissiveTexSet, inUV, inUV1))
                          .rgb;
    float occlusion = MaterialSlotTexture(material, 3, material.occlusionTexIndex,
                                          MaterialTransformedUV(material, 3, material.aoTexSet, inUV, inUV1))
                          .r;
    occlusion       = mix(1.0, occlusion, material.materialProperties.x);
#ifdef HYBRID_RECEIVER
    // Forward-only layers are excluded from the CPU draw list. Hardware culling handles
    // single-sided geometry; preserve the logical facing for two-sided/mirrored receivers.
    bool facing = gl_FrontFacing == (inOrientation >= 0.0);
    outReceiver = uvec2(packUnorm2x16(EncodeGBufferNormal(geometric)), (uNodeIndex + 1u) * 2u + (facing ? 1u : 0u));
    // NDC motion (previous - current) and the previous w; w <= 0 lay behind the previous camera.
    outMotion   = inPreviousClip.w > 0.0
                    ? vec4(inPreviousClip.xy / inPreviousClip.w - inCurrentClip.xy / inCurrentClip.w, inPreviousClip.w, 0)
                    : vec4(0, 0, inPreviousClip.w, 0);
    outPosition = vec4(inWorldPos, 1);
#endif
    outNormal = EncodeGBufferNormal(normal);
    outAlbedo = vec4(albedo.rgb, 1);
    outMetallicRoughness =
        vec4(material.metallicFactor * mr.b, material.roughnessFactor * mr.g, material.materialProperties.y, 0);
    outEmissiveOcclusion = vec4(ClampHDRStorage(emission), occlusion);
#ifdef HYBRID_RECEIVER
    MaterialSurface surface = ReadMaterialSurface(material);
    outMetallicRoughness.g  = surface.roughness;
#endif
}
