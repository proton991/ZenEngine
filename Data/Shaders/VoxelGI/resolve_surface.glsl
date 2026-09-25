#include "voxel_surface.glsl"
#include "Graphics/Shared/VoxelGI.h"
layout(local_size_x=ZEN_VOXEL_VOLUME_GROUP_SIZE, local_size_x_id=ZEN_VOXEL_VOLUME_GROUP_X_ID,
       local_size_y=ZEN_VOXEL_VOLUME_GROUP_SIZE, local_size_y_id=ZEN_VOXEL_VOLUME_GROUP_Y_ID,
       local_size_z=ZEN_VOXEL_VOLUME_GROUP_SIZE, local_size_z_id=ZEN_VOXEL_VOLUME_GROUP_Z_ID) in;
layout(set=4,binding=0,r32ui) uniform readonly uimage3D voxelOwner;
layout(set=4,binding=1,rgba8) uniform writeonly image3D voxelAlbedo;
#ifdef AVERAGED_REFLECTANCE
layout(set=4,binding=4,std430) readonly buffer ReflectanceSums { uvec4 reflectanceSums[]; };
layout(set=4,binding=5,rgba8) uniform writeonly image3D voxelReflectance;
#endif
#ifdef RADIANCE_INPUTS
layout(set=4,binding=2,rgba8) uniform writeonly image3D voxelNormal;
layout(set=4,binding=3,rgba16f) uniform writeonly image3D voxelEmissive;
#endif
void main()
{
    ivec3 p=ivec3(gl_GlobalInvocationID);
    if(any(greaterThanEqual(p,imageSize(voxelAlbedo)))) return;
    uint record=imageLoad(voxelOwner,p).r;
    vec4 albedo=vec4(0), normalMetal=vec4(0), emission=vec4(0);
    if(record != 0xffffffffu && record < triangles.length())
    {
        Vertex a,b,c; vec3 pa,pb,pc;
        TriangleVertices(record,a,b,c,pa,pb,pc);
        vec3 position=gridMinSize.xyz+(vec3(p)+0.5)*gridMinSize.w;
        vec3 w=TriangleBarycentrics(pa,pb,pc,position);
        vec2 uv0=a.texcoord*w.x+b.texcoord*w.y+c.texcoord*w.z;
        vec2 uv1=a.uv1*w.x+b.uv1*w.y+c.uv1*w.z;
        Material material=materialData[triangles[record].z];
        albedo=vec4(MaterialAlbedo(material,uv0,uv1,a.color*w.x+b.color*w.y+c.color*w.z).rgb,1);
        vec3 normal=mat3(nodesData[triangles[record].y].normalMatrix)*
            (a.normal.xyz*w.x+b.normal.xyz*w.y+c.normal.xyz*w.z);
        float normalLengthSquared=dot(normal,normal);
        if(!(normalLengthSquared>1e-12) || isinf(normalLengthSquared))
            normal=cross(pb-pa,pc-pa);
        normalLengthSquared=dot(normal,normal);
        normal=normalLengthSquared>1e-12 && !isinf(normalLengthSquared) ? normalize(normal) : vec3(0,1,0);
        float metal=material.metallicFactor*MaterialTexture(material.mrTexIndex,
            MaterialUV(material.mrTexSet,uv0,uv1)).b;
        normalMetal=vec4(normal*0.5+0.5,clamp(metal,0,1));
        emission=vec4(material.emissiveFactor.rgb*MaterialTexture(material.emissiveTexIndex,
            MaterialUV(material.emissiveTexSet,uv0,uv1)).rgb,0);
    }
    imageStore(voxelAlbedo,p,albedo);
#ifdef AVERAGED_REFLECTANCE
    ivec3 size=imageSize(voxelAlbedo);
    uvec4 sums=reflectanceSums[p.x+size.x*(p.y+size.y*p.z)];
    vec4 mean=sums.w==0u ? vec4(0) :
        vec4(vec3(sums.xyz)/(float(ZEN_VOXEL_REFLECTANCE_SCALE)*float(sums.w)),1);
    // Choose the UNORM code explicitly so implementation-specific image-store
    // conversion precision cannot move values near a half-code boundary.
    mean.rgb=floor(clamp(mean.rgb,0,1)*255.0+0.5)/255.0;
    imageStore(voxelReflectance,p,mean);
#endif
#ifdef RADIANCE_INPUTS
    imageStore(voxelNormal,p,normalMetal);
    imageStore(voxelEmissive,p,emission);
#endif
}
