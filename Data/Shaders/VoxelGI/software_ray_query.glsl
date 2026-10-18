#ifndef ZEN_SOFTWARE_RAY_QUERY
#define ZEN_SOFTWARE_RAY_QUERY
#include "scene_ray_geometry.glsl"
struct SoftwareRayNode { vec3 minimum; uint firstAndCount; vec3 maximum; uint escape; };
struct SoftwareRayTriangle { vec4 a; vec4 b; vec4 c; uvec4 record; };
layout(std430,set=6,binding=6) readonly buffer SoftwareRayNodes { SoftwareRayNode softwareNodes[]; };
layout(std430,set=6,binding=7) readonly buffer SoftwareRayTriangles { SoftwareRayTriangle softwareTriangles[]; };

layout(std140,set=2,binding=4) uniform uSoftwareRayData { uvec4 counts; } softwareRay;

bool SoftwareRayBox(SoftwareRayNode node, vec3 origin, vec3 direction, vec3 inverseDirection, float maximumDistance)
{
    vec3 a=(node.minimum.xyz-origin)*inverseDirection;
    vec3 b=(node.maximum.xyz-origin)*inverseDirection;
    bvec3 parallel=equal(direction,vec3(0));
    vec3 lower=mix(min(a,b),vec3(-1e30),parallel);
    vec3 upper=mix(max(a,b),vec3(1e30),parallel);
    float enter=max(0.0,max(lower.x,max(lower.y,lower.z)));
    float leave=min(maximumDistance,min(upper.x,min(upper.y,upper.z)));
    bool valid=all(mix(bvec3(true),greaterThanEqual(origin,node.minimum.xyz),parallel))
        && all(mix(bvec3(true),lessThanEqual(origin,node.maximum.xyz),parallel));
    return valid && leave>=enter;
}

bool SoftwareRayIntersection(SoftwareRayTriangle triangle, vec3 origin, ivec3 axes, vec3 shear,
                             float maximumDistance, out float distance, out vec2 barycentric)
{
    // Project along the dominant ray axis; shared edges use the same arithmetic.
    int x=axes.x, y=axes.y, z=axes.z;
    vec3 a=triangle.a.xyz-origin, b=triangle.b.xyz-origin, c=triangle.c.xyz-origin;
    vec2 pa=vec2(a[x],a[y])-shear.xy*a[z];
    vec2 pb=vec2(b[x],b[y])-shear.xy*b[z];
    vec2 pc=vec2(c[x],c[y])-shear.xy*c[z];
    precise float u=pc.x*pb.y-pc.y*pb.x;
    precise float v=pa.x*pc.y-pa.y*pc.x;
    precise float w=pb.x*pa.y-pb.y*pa.x;
    float determinant=u+v+w;
    bool hit=determinant!=0.0 && !(min(u,min(v,w))<0.0 && max(u,max(v,w))>0.0);
    distance=0.0; barycentric=vec2(0);
    if(hit)
    {
        distance=(u*a[z]+v*b[z]+w*c[z])*shear.z/determinant;
        barycentric=vec2(v,w)/determinant;
        hit=distance>=0.0 && distance<=maximumDistance;
    }
    return hit;
}

// Closest-hit and visibility queries; the triangle record identifies the hit for oracle tests.
struct SoftwareRayHit
{
    HybridRayResult ray;
    uvec4 triangle;
    float distance;
    vec2 barycentrics;
};
SoftwareRayHit SoftwareQuery(vec3 origin, vec3 direction, float maximumDistance, bool closest)
{
    HybridRayResult result=HybridRayResult(false,origin,vec3(0),ivec3(-1));
#ifdef HYBRID_COUNT_RAYS
    ++issuedComputeRays[issuedRayFeature];
#endif
    uint index=0u;
    float nearest=maximumDistance;
    vec3 inverseDirection=1.0/mix(direction,vec3(1),equal(direction,vec3(0)));
    vec3 d=abs(direction);
    int z=d.x>d.y ? 0:1;
    if(d.z>d[z]) z=2;
    int x=(z+1)%3, y=(x+1)%3;
    if(direction[z]<0.0) { int swap=x; x=y; y=swap; }
    ivec3 axes=ivec3(x,y,z);
    vec3 shear=vec3(direction[x],direction[y],1.0)/direction[z];
    uint hitTriangle=0u;
    vec2 hitBarycentric=vec2(0);
    // Escape links make traversal stackless and impose no depth/triangle limit.
    while(index<softwareRay.counts.x)
    {
        SoftwareRayNode node=softwareNodes[index];
        uint count=node.firstAndCount&7u, first=node.firstAndCount>>3u;
        if(!SoftwareRayBox(node,origin,direction,inverseDirection,nearest)) index=node.escape;
        else if(count==0u && node.escape>index+1u) ++index;
        else
        {
            for(uint i=0u;i<count;++i)
            {
                SoftwareRayTriangle triangle=softwareTriangles[first+i];
                float distance; vec2 barycentric;
                if(SoftwareRayIntersection(triangle,origin,axes,shear,nearest,distance,barycentric)
                    && RayCandidateAccepted(triangle.record,barycentric))
                {
                    nearest=distance;
                    result.hit=true;
                    hitTriangle=first+i;
                    hitBarycentric=barycentric;
                    if(!closest) break;
                }
            }
            index=node.escape;
            if(result.hit && !closest) break;
        }
    }
    uvec4 triangle=uvec4(0xffffffffu);
    if(result.hit)
    {
        triangle=softwareTriangles[hitTriangle].record;
        result.position=RaySurfacePosition(triangle,hitBarycentric);
        result.normal=RayGeometricNormal(triangle);
        if(dot(result.normal,direction)>0.0) result.normal=-result.normal;
        result.cell=ivec3(floor((result.position-gi.gridMinVoxelSize.xyz)/gi.gridMinVoxelSize.w));
    }
    return SoftwareRayHit(result,triangle,result.hit ? nearest : -1.0,hitBarycentric);
}
HybridRayResult SoftwareTraceRay(vec3 origin, vec3 direction, float maximumDistance, bool closest)
{
    return SoftwareQuery(origin,direction,maximumDistance,closest).ray;
}
#endif
