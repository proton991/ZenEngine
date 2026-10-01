#include "../Common/material_surface.glsl"
#include "../Common/hdr_storage.glsl"
layout(location=0) out vec4 outPosition;
layout(location=1) out vec4 outNormal;
layout(location=2) out vec4 outAlbedo;
layout(location=3) out vec4 outMetallicRoughness;
layout(location=4) out vec4 outEmissiveOcclusion;
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
    outPosition=vec4(inWorldPos,1);
    outNormal=vec4(normal,1);
    outAlbedo=vec4(albedo.rgb,1);
    outMetallicRoughness=vec4(material.metallicFactor*mr.b,material.roughnessFactor*mr.g,material.materialProperties.y,0);
    outEmissiveOcclusion=vec4(ClampHDRStorage(emission),occlusion);
}
