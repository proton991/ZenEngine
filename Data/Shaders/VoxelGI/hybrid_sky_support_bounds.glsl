layout(set=1,binding=20) uniform sampler2D skySupport;
layout(std430,set=1,binding=23) readonly buffer HybridSkySupportBounds { vec4 supportBounds[]; };
uint SkySupportMask(vec3 geometric)
{
    uint mask=0u;
    // A dominant-axis bucket lies inside a cone with cosine 1/sqrt(3).
    // Reject it only when even that cone cannot meet SurfaceWeight's 0.9 test.
    for(uint bucket=0u;bucket<6u;++bucket)
    {
        float c=clamp(geometric[bucket/2u]*((bucket&1u)==0u ? 1.0 : -1.0),-1.0,1.0);
        float upper=c>=0.57735026 ? 1.0 : c*0.57735026+sqrt(max(1.0-c*c,0.0))*0.81649659;
        if(upper>=0.89999) mask|=1u<<bucket;
    }
    return mask;
}
bool TileHasSkySupport(ivec2 tile, uint mask, vec3 geometric, vec3 position)
{
    uint candidates=uint(texelFetch(skySupport,tile,0).r)&mask;
    while(candidates!=0u)
    {
        uint bucket=uint(findLSB(candidates));
        candidates&=candidates-1u;
        uint base=(uint(tile.y)*uint(textureSize(skySupport,0).x)+uint(tile.x))*12u+bucket*2u;
        vec3 lower=supportBounds[base].xyz-position, upper=supportBounds[base+1u].xyz-position;
        vec3 a=geometric*lower, b=geometric*upper;
        vec3 nearest=min(a,b), farthest=max(a,b);
        float nearPlane=nearest.x+nearest.y+nearest.z, farPlane=farthest.x+farthest.y+farthest.z;
        float distance=max(max(nearPlane,-farPlane),0.0);
        float maximumLength=length(max(abs(lower),abs(upper)));
        // Every point in the box is within maximumLength. Include a float
        // roundoff margin so this coarse test cannot reject a valid tap.
        float tolerance=hybrid.rejection.x*max(1.0,maximumLength)
            +1e-6*max(1.0,maximumLength+length(position));
        if(distance<=tolerance) return true;
    }
    return false;
}
