#include "../Common/bindless_heap.glsl"
#include "gi_common.glsl"
#include "Graphics/Shared/VoxelGI.h"
layout(local_size_x=ZEN_VOXEL_VOLUME_GROUP_SIZE, local_size_x_id=ZEN_VOXEL_VOLUME_GROUP_X_ID,
       local_size_y=ZEN_VOXEL_VOLUME_GROUP_SIZE, local_size_y_id=ZEN_VOXEL_VOLUME_GROUP_Y_ID,
       local_size_z=ZEN_VOXEL_VOLUME_GROUP_SIZE, local_size_z_id=ZEN_VOXEL_VOLUME_GROUP_Z_ID) in;
layout(set=1,binding=0) uniform sampler3D voxelAlbedo;
layout(set=1,binding=1) uniform sampler3D voxelNormal;
layout(set=1,binding=2) uniform sampler3D voxelEmissive;
layout(set=1,binding=3,rgba16f) uniform writeonly image3D voxelRadiance;
layout(set=1,binding=4) uniform sampler3D skyIrradiance;
#ifdef AVERAGED_REFLECTANCE
layout(set=1,binding=5) uniform sampler3D voxelReflectance;
#endif
void main()
{
    ivec3 p=ivec3(gl_GlobalInvocationID);
    if(any(greaterThanEqual(p,imageSize(voxelRadiance)))) return;
    vec4 albedo=texelFetch(voxelAlbedo,p,0);
    vec4 radiance=vec4(0);
    if(albedo.a>0.5)
    {
        vec4 normalMetal=texelFetch(voxelNormal,p,0);
        vec3 normal=normalize(normalMetal.rgb*2.0-1.0);
        vec3 position=gi.gridMinVoxelSize.xyz+(vec3(p)+0.5)*gi.gridMinVoxelSize.w;
        vec3 origin=TraceOrigin(position,normal);
        vec3 irradiance=texelFetch(skyIrradiance,p,0).rgb;
        for(int i=0;i<int(sceneUbo.lightInfo.x);++i)
        {
            vec3 direction; float distanceToLight;
            vec3 incoming=EvaluateLight(sceneUbo.lights[i],position,direction,distanceToLight);
            float cosine=max(dot(normal,direction),0.0);
            if(cosine>0 && any(greaterThan(incoming,vec3(0))))
            {
                float visibility=VoxelLightVisibility(voxelAlbedo,sceneUbo.lights[i],origin);
                irradiance+=incoming*cosine*visibility;
            }
        }
        #ifdef AVERAGED_REFLECTANCE
        vec3 diffuseReflectance=texelFetch(voxelReflectance,p,0).rgb;
#else
        vec3 diffuseReflectance=albedo.rgb*(1.0-normalMetal.a)*0.96;
#endif
        radiance=vec4(texelFetch(voxelEmissive,p,0).rgb+diffuseReflectance*irradiance/3.14159265359,1);
    }
    imageStore(voxelRadiance,p,radiance);
}
