#include "../Common/bindless_heap.glsl"
#include "../Common/hdr_storage.glsl"
#include "hybrid_provider.glsl"
#include "hybrid_sampling.glsl"
#include "environment_sampling.glsl"
#define VOXEL_GEOMETRY_SET 5
#include "voxel_geometry.glsl"
layout(set=1,binding=4,r32ui) uniform readonly uimage3D voxelOwner;
#include "Graphics/Shared/VoxelGI.h"
layout(local_size_x=ZEN_VOXEL_VOLUME_GROUP_SIZE, local_size_x_id=ZEN_VOXEL_VOLUME_GROUP_X_ID,
       local_size_y=ZEN_VOXEL_VOLUME_GROUP_SIZE, local_size_y_id=ZEN_VOXEL_VOLUME_GROUP_Y_ID,
       local_size_z=ZEN_VOXEL_VOLUME_GROUP_SIZE, local_size_z_id=ZEN_VOXEL_VOLUME_GROUP_Z_ID) in;
layout(set=1,binding=0) uniform sampler3D voxelAlbedo;
layout(set=1,binding=1) uniform sampler3D voxelNormal;
layout(set=1,binding=2) uniform samplerCube coneEnvironmentMap;
layout(set=1,binding=3,rgba16f) uniform writeonly image3D skyIrradiance;
void main()
{
    ivec3 p=ivec3(gl_GlobalInvocationID);
    if(any(greaterThanEqual(p,imageSize(skyIrradiance)))) return;
    vec3 irradiance=vec3(0);
    if(texelFetch(voxelAlbedo,p,0).a>0.5 && sceneUbo.environment.z>0 && gi.lighting.y>0)
    {
        vec3 normal=normalize(texelFetch(voxelNormal,p,0).rgb*2.0-1.0);
        vec3 position=gi.gridMinVoxelSize.xyz+(vec3(p)+0.5)*gi.gridMinVoxelSize.w;
        // Averaged reflectance uses the deterministic owner as its representative surface.
        uint owner=imageLoad(voxelOwner,p).r;
        if(owner<triangles.length())
        {
            Vertex a,b,c; vec3 pa,pb,pc;
            TriangleVertices(owner,a,b,c,pa,pb,pc);
            vec3 face=cross(pb-pa,pc-pa);
            if(dot(face,face)>1e-20)
            {
                vec3 reference=mat3(nodesData[triangles[owner].y].normalMatrix)*a.normal.xyz;
                normal=normalize(face);
                if(dot(normal,reference)<0.0) normal=-normal;
                vec3 barycentric=TriangleBarycentrics(pa,pb,pc,position);
                position=pa*barycentric.x+pb*barycentric.y+pc*barycentric.z;
            }
        }
        vec3 origin=HybridTraceOrigin(position,normal,0.0);
        // 48 environment and 16 cosine directions, the per-pixel proposal shares.
        for(int i=0;i<64;++i)
        {
            float pdf;
            bool environmentSample=(i&3)!=3;
            int j=environmentSample ? i-i/4 : i/4;
            vec2 u=vec2((float(j)+0.5)/(environmentSample ? 48.0 : 16.0),fract(float(j)*0.61803398875));
            vec3 direction=HybridSkySample(normal,u,environmentSample,pdf);
            float cosine=max(dot(normal,direction),0.0);
            if(cosine>0.0 && !HybridTraceRay(voxelAlbedo,origin,direction,1e20).hit)
                irradiance+=textureLod(coneEnvironmentMap,EnvironmentDirection(direction),0).rgb
                    *cosine/(HYBRID_PI*max(pdf,1e-8)*64.0);
        }
        irradiance*=3.14159265359*sceneUbo.environment.x;
    }
    imageStore(skyIrradiance,p,vec4(ClampHDRStorage(irradiance),0));
}
