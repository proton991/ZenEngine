#ifndef ZEN_HIT_RADIANCE
#define ZEN_HIT_RADIANCE
// Base-level cache lookup shared by both query providers. The cache is isotropic,
// but its owner normal must face the incoming ray; the back of a thin wall cannot
// borrow radiance from its illuminated front. No radiance mips are sampled.
bool HitRadianceCell(sampler3D radiance, sampler3D normals, ivec3 cell, vec3 direction, out vec3 value)
{
    value=vec3(0);
    bool valid=all(greaterThanEqual(cell,ivec3(0))) && all(lessThan(cell,textureSize(radiance,0)));
    if(valid)
    {
        vec4 sampleValue=texelFetch(radiance,cell,0);
        vec3 normal=texelFetch(normals,cell,0).rgb*2.0-1.0;
        valid=sampleValue.a>0.0 && dot(normal,-direction)>0.0;
        if(valid) value=sampleValue.rgb/sampleValue.a;
    }
    return valid;
}
vec3 HitRadiance(sampler3D radiance, sampler3D normals, HybridRayResult hit, vec3 direction, out bool rejected)
{
    vec3 value=vec3(0);
    bool hasFace=hit.hit && dot(hit.normal,hit.normal)>0.0;
    bool accepted=hasFace && HitRadianceCell(radiance,normals,hit.cell,direction,value);
    if(hasFace && !accepted)
    {
        // The entered face chooses the adjacent cell on the ray's side. This also
        // resolves triangle hits lying exactly on a voxel boundary.
        vec3 face=abs(hit.normal);
        int axis=face.x>face.y ? 0:1;
        if(face.z>face[axis]) axis=2;
        ivec3 adjacent=hit.cell;
        adjacent[axis]+=direction[axis]>0.0 ? -1:1;
        accepted=HitRadianceCell(radiance,normals,adjacent,direction,value);
    }
    rejected=!accepted;
    return accepted ? value*gi.volume.w : vec3(0);
}
#endif
