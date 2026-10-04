#version 450
#extension GL_GOOGLE_include_directive : require
#include "../Common/material_surface.glsl"
layout(location=0) out uint outId;
layout(std140,set=1,binding=2) readonly buffer MaterialBuffer { Material materialData[]; };
layout(push_constant) uniform Constants { uint uNodeIndex; uint uMaterialIndex; uint uObjectId; };
void main()
{
    Material material = materialData[uMaterialIndex];
    vec4 albedo = MaterialAlbedo(material, inUV, inUV1, inColor);
    if (!MaterialVisible(material, albedo.a) ||
        (material.materialProperties.z < 0.5 && !(gl_FrontFacing == (inOrientation >= 0.0)))) discard;
    outId = uObjectId;
}
