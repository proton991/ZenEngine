#include "Graphics/Shared/DynamicVoxelGI.h"
layout(set=3,binding=8,std140) uniform uStaticGI { GIStaticUniform staticGI; };
layout(set=3,binding=9,std430) readonly buffer GIStatus { uvec4 giStatus; };
layout(set=3,binding=10) uniform sampler3D staticIrradiance0;
layout(set=3,binding=11) uniform sampler3D staticIrradiance1;
layout(set=3,binding=12) uniform sampler3D staticIrradiance2;
layout(set=3,binding=13) uniform sampler3D staticIrradiance3;
layout(set=3,binding=14) uniform sampler3D staticIrradiance4;
layout(set=3,binding=15) uniform sampler3D staticIrradiance5;

layout(set=3,binding=16) uniform sampler3D dynamicIrradiance0;
layout(set=3,binding=17) uniform sampler3D dynamicIrradiance1;
layout(set=3,binding=18) uniform sampler3D dynamicIrradiance2;
layout(set=3,binding=19) uniform sampler3D dynamicIrradiance3;
layout(set=3,binding=20) uniform sampler3D dynamicIrradiance4;
layout(set=3,binding=21) uniform sampler3D dynamicIrradiance5;

bool DirectionalDiffuseIrradiance(vec3 position,vec3 normal,uint objectClass,out vec3 irradiance)
{
    vec3 uv=(position-staticGI.minimumCellSize.xyz)/(float(staticGI.volume.x)*staticGI.minimumCellSize.w);
    vec3 n=normal/max(length(normal),1e-8);
    vec4 faces[6];
    faces[0]=textureLod(staticIrradiance0,uv,0); faces[1]=textureLod(staticIrradiance1,uv,0);
    faces[2]=textureLod(staticIrradiance2,uv,0); faces[3]=textureLod(staticIrradiance3,uv,0);
    faces[4]=textureLod(staticIrradiance4,uv,0); faces[5]=textureLod(staticIrradiance5,uv,0);
    if(objectClass==GI_DYNAMIC) {
        faces[0]=textureLod(dynamicIrradiance0,uv,0); faces[1]=textureLod(dynamicIrradiance1,uv,0);
        faces[2]=textureLod(dynamicIrradiance2,uv,0); faces[3]=textureLod(dynamicIrradiance3,uv,0);
        faces[4]=textureLod(dynamicIrradiance4,uv,0); faces[5]=textureLod(dynamicIrradiance5,uv,0);
    }
    // Interpolate the even part quadratically and the odd part linearly.
    // This reproduces constant + first-order spherical irradiance exactly,
    // whereas choosing three faces with n^2 loses directional energy off-axis.
    irradiance=vec3(0);
    bool valid=dot(n,n)>0.5;
    for(uint face=0;face<6;++face) {
        float axis=n[face/2u];
        float weight=0.5*(axis*axis+(face%2u==0u ? axis : -axis));
        irradiance+=weight*faces[face].rgb;
        // Signed reconstruction weights must never cancel invalid alpha.
        valid=valid && (weight==0 || faces[face].a>=0.999);
    }
    irradiance=max(irradiance,vec3(0));
    return all(greaterThanEqual(uv,vec3(0))) && all(lessThanEqual(uv,vec3(1))) && valid;
}

bool StaticDiffuseIrradiance(vec3 position,vec3 normal,out vec3 irradiance) {
    return DirectionalDiffuseIrradiance(position,normal,GI_STATIC,irradiance);
}
