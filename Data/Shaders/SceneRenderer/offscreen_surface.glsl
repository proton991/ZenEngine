#include "../Common/material_surface.glsl"
#include "../Common/hdr_storage.glsl"
#include "../Common/gbuffer.glsl"
layout(location=0) out vec2 outNormal;
layout(location=1) out vec4 outAlbedo;
layout(location=2) out vec4 outMetallicRoughness;
layout(location=3) out vec4 outEmissiveOcclusion;
#ifdef HYBRID_RECEIVER
layout(location=4) out uvec2 outReceiver;
layout(location=5) out vec4 outMotion;
layout(location=6) out vec4 outPosition;
layout(location=16) in vec4 inPreviousClip;
layout(location=17) in vec4 inCurrentClip;
#endif
layout(std140,set=1,binding=2) readonly buffer MaterialBuffer { Material materialData[]; };
layout(push_constant) uniform Constants { uint uNodeIndex; uint uMaterialIndex;
};


void main()
{
    Material material=materialData[uMaterialIndex];
    vec3 normal=SurfaceNormal(material);
    vec4 albedo=MaterialAlbedo(material,inUV,inUV1,inColor);
    if(!MaterialVisible(material,albedo.a) || (material.materialProperties.z < 0.5 && !(gl_FrontFacing == (inOrientation >= 0.0)))) discard;
    vec4 mr=MaterialSlotTexture(material, 1, material.mrTexIndex,MaterialTransformedUV(material, 1, material.mrTexSet,inUV,inUV1));
    vec3 emission=material.emissiveFactor.rgb*MaterialSlotTexture(material, 4, material.emissiveTexIndex,
        MaterialTransformedUV(material, 4, material.emissiveTexSet,inUV,inUV1)).rgb;
    float occlusion=MaterialSlotTexture(material, 3, material.occlusionTexIndex,
        MaterialTransformedUV(material, 3, material.aoTexSet,inUV,inUV1)).r;
    occlusion=mix(1.0,occlusion,material.materialProperties.x);
#ifdef HYBRID_RECEIVER
    // Exclude layers that must retain per-surface lighting in the forward renderer.
    if(material.surfaceProperties.y==2.0 || material.sheenColorTransmission.w>0.0 ||
       material.diffuseTransmissionColorFactor.w>0.0) discard;
    // Degeneracy is judged relative to the pixel footprint. An absolute threshold rejected
    // nearly every pixel of the unit-normalized scene (|dFdx x dFdy| ~ 4e-7 at 960x540) and
    // silently substituted the shading normal for the geometric one.
    vec3 dx=dFdx(inWorldPos), dy=dFdy(inWorldPos), face=cross(dx,dy);
    vec3 geometric=dot(face,face)>1e-12*dot(dx,dx)*dot(dy,dy) ? normalize(face) : normal;
    if(dot(geometric,normal)<0.0) geometric=-geometric;
    bool facing=gl_FrontFacing == (inOrientation>=0.0);
    outReceiver=uvec2(packUnorm2x16(EncodeGBufferNormal(geometric)),(uNodeIndex+1u)*2u+(facing ? 1u:0u));
    // NDC motion (previous - current) and the previous w; w <= 0 lay behind the previous camera.
    outMotion=inPreviousClip.w>0.0 ? vec4(inPreviousClip.xy/inPreviousClip.w-inCurrentClip.xy/inCurrentClip.w,inPreviousClip.w,0)
                                   : vec4(0,0,inPreviousClip.w,0);
    outPosition=vec4(inWorldPos,1);
#endif
    outNormal=EncodeGBufferNormal(normal);
    outAlbedo=vec4(albedo.rgb,1);
    outMetallicRoughness=vec4(material.metallicFactor*mr.b,material.roughnessFactor*mr.g,material.materialProperties.y,0);
    outEmissiveOcclusion=vec4(ClampHDRStorage(emission),occlusion);
#ifdef HYBRID_RECEIVER
    MaterialSurface surface=ReadMaterialSurface(material);
    outMetallicRoughness.g=surface.roughness;
#endif
}
