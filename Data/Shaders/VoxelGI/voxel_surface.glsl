#include "../Common/material.glsl"
#ifdef VOXEL_PRECISE_CONTRIBUTION
#define VOXEL_MATERIAL_PRECISION precise
#else
#define VOXEL_MATERIAL_PRECISION
#endif
#include "voxel_geometry.glsl"
layout(std140, set = 3, binding = 3) readonly buffer MaterialBuffer { Material materialData[]; };
layout(std140, set = 1, binding = 0) uniform uVoxelGrid { vec4 gridMinSize; uvec4 gridSelection; };
bool SurfaceVisible(uint record, vec3 position, out VOXEL_MATERIAL_PRECISION vec3 reflectance)
{
    Vertex a,b,c; vec3 pa,pb,pc;
    TriangleVertices(record,a,b,c,pa,pb,pc);
    vec3 w = TriangleBarycentrics(pa,pb,pc,position);
    Material material = materialData[triangles[record].z];
    VOXEL_MATERIAL_PRECISION vec2 uv0=a.texcoord*w.x+b.texcoord*w.y+c.texcoord*w.z;
    VOXEL_MATERIAL_PRECISION vec2 uv1=a.uv1*w.x+b.uv1*w.y+c.uv1*w.z;
    VOXEL_MATERIAL_PRECISION vec4 color=a.color*w.x+b.color*w.y+c.color*w.z;
    vec4 albedo = MaterialAlbedo(material, uv0, uv1, color);
    VOXEL_MATERIAL_PRECISION float metal=clamp(material.metallicFactor*MaterialTexture(material.mrTexIndex,
        MaterialUV(material.mrTexSet,uv0,uv1)).b,0,1);
    reflectance=clamp(albedo.rgb*(1.0-metal)*0.96,0,1);
    return MaterialVisible(material,albedo.a);
}
bool SurfaceVisible(uint record, vec3 position)
{
    vec3 unusedReflectance;
    return SurfaceVisible(record,position,unusedReflectance);
}
