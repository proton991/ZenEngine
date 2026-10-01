#ifdef VOXEL_PRECISE_CONTRIBUTION
#define VOXEL_MATERIAL_PRECISION precise
#else
#define VOXEL_MATERIAL_PRECISION
#endif
vec2 inBindingUV[22];
#define MATERIAL_BINDING_UV
#include "../Common/material.glsl"
#include "voxel_geometry.glsl"
layout(std430, set = VOXEL_GEOMETRY_SET, binding = 5) readonly buffer UVBuffer { vec4 uvValues[]; };
layout(std140, set = 3, binding = 3) readonly buffer MaterialBuffer { Material materialData[]; };
layout(std140, set = 1, binding = 0) uniform uVoxelGrid { vec4 gridMinSize; uvec4 gridSelection; };
void PopulateVoxelBindingUV(uint record, vec3 weights, Material material)
{
    uint first = triangles[record].x;
    uint stride = uint(uvValues[0].x);
    int sets[5] = int[5](material.bcTexSet, material.mrTexSet, material.normalTexSet, material.aoTexSet, material.emissiveTexSet);
    for (int slot = 0; slot < 22; ++slot)
    {
        uint set = uint(max(slot < 5 ? sets[slot] : int(material.featureTextures[slot - 5].properties.y), 0));
        inBindingUV[slot] = vec2(0.0);
        if (set < stride)
        {
            for (uint corner = 0; corner < 3; ++corner)
                inBindingUV[slot] += uvValues[1 + indices[first + corner] * stride + set].xy * weights[corner];
        }
    }
}
vec4 VoxelMaterialAlbedo(Material material, vec2 uv0, vec2 uv1, vec4 color)
{
    return material.surfaceProperties.w > 0.5 ?
        material.diffuseFactor * color * MaterialFeatureTexture(material, 2, uv0, uv1) :
        MaterialAlbedo(material, uv0, uv1, color);
}
float VoxelDiffuseWeight(Material material, vec2 uv0, vec2 uv1)
{
    if (material.surfaceProperties.w > 0.5)
    {
        vec3 specular = material.specularGlossiness.rgb * MaterialFeatureTexture(material, 3, uv0, uv1).rgb;
        return 1.0 - clamp(max(max(specular.r, specular.g), specular.b), 0.0, 1.0);
    }
    VOXEL_MATERIAL_PRECISION float metal = clamp(material.metallicFactor * MaterialSlotTexture(material, 1, material.mrTexIndex,
        MaterialTransformedUV(material, 1, material.mrTexSet, uv0, uv1)).b, 0.0, 1.0);
    return (1.0 - metal) * 0.96;
}
bool SurfaceVisible(uint record, vec3 position, out VOXEL_MATERIAL_PRECISION vec3 reflectance)
{
    Vertex a,b,c; vec3 pa,pb,pc;
    TriangleVertices(record,a,b,c,pa,pb,pc);
    vec3 w = TriangleBarycentrics(pa,pb,pc,position);
    Material material = materialData[triangles[record].z];
    PopulateVoxelBindingUV(record, w, material);
    VOXEL_MATERIAL_PRECISION vec2 uv0=a.texcoord*w.x+b.texcoord*w.y+c.texcoord*w.z;
    VOXEL_MATERIAL_PRECISION vec2 uv1=a.uv1*w.x+b.uv1*w.y+c.uv1*w.z;
    VOXEL_MATERIAL_PRECISION vec4 color=a.color*w.x+b.color*w.y+c.color*w.z;
    vec4 albedo = VoxelMaterialAlbedo(material, uv0, uv1, color);
    reflectance=clamp(albedo.rgb*VoxelDiffuseWeight(material, uv0, uv1),0,1);
    return MaterialVisible(material,albedo.a);
}
bool SurfaceVisible(uint record, vec3 position)
{
    vec3 unusedReflectance;
    return SurfaceVisible(record,position,unusedReflectance);
}
