#include "query_common.glsl"
#ifdef GI_COUNT_TRAVERSAL
uint giTraversalVisits=0u;
#endif
layout(set=1,binding=0,std140) uniform uGIGrid { GIGridUniform giGrid; };
layout(set=1,binding=1,r32ui) readonly uniform uimage3D giStaticOwner;
layout(set=1,binding=2,rgba8) readonly uniform image3D giStaticAlbedo;
layout(set=1,binding=3,rgba8) readonly uniform image3D giStaticNormal;
layout(set=1,binding=4,rgba16f) readonly uniform image3D giStaticEmission;
layout(set=1,binding=5,rgba8) readonly uniform image3D giStaticReflectance;
layout(set=1,binding=6,r32ui) readonly uniform uimage3D giDynamicOwner;
layout(set=1,binding=7,rgba8) readonly uniform image3D giDynamicAlbedo;
layout(set=1,binding=8,rgba8) readonly uniform image3D giDynamicNormal;
layout(set=1,binding=9,rgba16f) readonly uniform image3D giDynamicEmission;
layout(set=1,binding=10,rgba8) readonly uniform image3D giDynamicReflectance;

uint GICellID(ivec3 cell)
{
    uint n=giGrid.dimensions.x;
    return uint(cell.z)+n*(uint(cell.y)+n*uint(cell.x));
}

// Cells are half-open on their upper faces. A tangent or point-only contact is
// not a traversed interval. Exact crossing ties advance all tied axes together.
bool GIClipBox(vec3 origin,vec3 direction,vec3 minimum,vec3 maximum,inout float enter,inout float leave)
{
    bool intersects=true;
    for(int axis=0;axis<3;++axis)
    {
        if(direction[axis]==0)
        {
            intersects=intersects && origin[axis]>=minimum[axis] && origin[axis]<maximum[axis];
        }
        else
        {
            float a=(minimum[axis]-origin[axis])/direction[axis];
            float b=(maximum[axis]-origin[axis])/direction[axis];
            enter=max(enter,min(a,b));
            leave=min(leave,max(a,b));
        }
    }
    return intersects && enter<leave;
}

bool GISkipSource(GIQuery ray,ivec3 cell,uint objectClass,float t)
{
    bool skip=false;
    if(ray.source.y==objectClass && ray.source.z==GICellID(cell))
    {
        vec3 lo=giGrid.minimumCellSize.xyz+vec3(cell)*giGrid.minimumCellSize.w;
        vec3 hi=lo+vec3(giGrid.minimumCellSize.w);
        // Inclusive here: the hint may name a face-center surface. Only its
        // original exit interval is ignored, never a cell reached from outside.
        if(all(greaterThanEqual(ray.originMin.xyz,lo)) && all(lessThanEqual(ray.originMin.xyz,hi)))
        {
            float exitTime=ray.directionMax.w;
            for(int axis=0;axis<3;++axis)
            {
                float d=ray.directionMax[axis];
                if(d!=0) exitTime=min(exitTime,((d>0 ? hi[axis] : lo[axis])-ray.originMin[axis])/d);
            }
            skip=t<exitTime;
        }
    }
    return skip;
}

// Geometry cache identity/distance survives surface-only material changes.
GIHit ResolveHitSurface(GIHit hit)
{
    if(hit.identity.x!=GI_HIT) return hit;
    uint side=giGrid.dimensions.x;
    uint objectClass=hit.identity.y;
    uint id=hit.identity.z;
    if(id>=side*side*side || (objectClass!=GI_STATIC && objectClass!=GI_DYNAMIC))
        return EmptyGIHit(GI_UNKNOWN);
    ivec3 cell=ivec3(id/(side*side),(id/side)%side,id%side);
    bool dynamic=objectClass==GI_DYNAMIC;
    vec4 albedo=dynamic ? imageLoad(giDynamicAlbedo,cell) : imageLoad(giStaticAlbedo,cell);
    vec4 packedNormal=dynamic ? imageLoad(giDynamicNormal,cell) : imageLoad(giStaticNormal,cell);
    vec3 normal=packedNormal.xyz*2.0-1.0;
    float normalLength=length(normal);
    bool surface=albedo.a>0 && normalLength>1e-6;
    hit.normal=vec4(normalLength>1e-6 ? normal/normalLength : vec3(0),0);
    hit.baseColorMetallic=vec4(albedo.rgb,packedNormal.a);
    hit.emission=dynamic ? imageLoad(giDynamicEmission,cell) : imageLoad(giStaticEmission,cell);
    bool averaged=(dynamic ? giGrid.averaged.y : giGrid.averaged.x)!=0;
    vec3 rho=averaged ? (dynamic ? imageLoad(giDynamicReflectance,cell).rgb : imageLoad(giStaticReflectance,cell).rgb) :
        albedo.rgb*(1.0-packedNormal.a)*0.96;
    hit.diffuseReflectance=vec4(rho,0);
    hit.identity=uvec4(GI_HIT,objectClass,GICellID(cell),GI_CELL_PRECISION|(surface ? GI_SURFACE_VALID : 0u));
    return hit;
}

GIHit GIDecodeCell(GIQuery ray,ivec3 cell,uint objectClass,float t)
{
    GIHit hit=EmptyGIHit(GI_HIT);
    hit.positionDistance=vec4(ray.originMin.xyz+ray.directionMax.xyz*t,t);
    hit.identity=uvec4(GI_HIT,objectClass,GICellID(cell),GI_CELL_PRECISION);
    return ResolveHitSurface(hit);
}

GIHit TraceClosest(GIQuery ray,uint queryIndex)
{
#ifdef GI_COUNT_TRAVERSAL
    giTraversalVisits=0u;
#endif
    GIHit result=EmptyGIHit(GI_UNKNOWN);
    uint side=giGrid.dimensions.x;
    bool valid=ValidGIQuery(ray) && (ray.source.y==0 || ray.source.z<side*side*side);
    if(valid && ray.originMin.w==ray.directionMax.w) result=EmptyGIHit(GI_MISS);
    else if(valid && (giGrid.dimensions.y&ray.source.x)==ray.source.x)
    {
        float t=ray.originMin.w,end=ray.directionMax.w;
        vec3 minimum=giGrid.minimumCellSize.xyz;
        float size=giGrid.minimumCellSize.w;
        bool intersects=GIClipBox(ray.originMin.xyz,ray.directionMax.xyz,minimum,minimum+vec3(float(side)*size),t,end);
        result=EmptyGIHit(GI_MISS);
        if(intersects)
        {
            vec3 p=(ray.originMin.xyz+ray.directionMax.xyz*t-minimum)/size;
            ivec3 cell=ivec3(floor(p));
            ivec3 step=ivec3(sign(ray.directionMax.xyz));
            for(int axis=0;axis<3;++axis)
                if(step[axis]<0 && p[axis]==floor(p[axis])) --cell[axis];
            cell=clamp(cell,ivec3(0),ivec3(side)-1);
            uint limit=3u*side+1u;
            if(giGrid.dimensions.z!=0) limit=min(limit,giGrid.dimensions.z);
            result=EmptyGIHit(GI_UNKNOWN);
            for(uint iteration=0;iteration<limit;++iteration)
            {
#ifdef GI_COUNT_TRAVERSAL
                ++giTraversalVisits;
#endif
                vec3 next=vec3(end);
                for(int axis=0;axis<3;++axis)
                    if(step[axis]!=0)
                        next[axis]=(minimum[axis]+float(cell[axis]+(step[axis]>0 ? 1 : 0))*size-ray.originMin[axis])/ray.directionMax[axis];
                float crossing=min(next.x,min(next.y,next.z));
                float cellEnd=min(end,crossing);
                uint objectClass=0;
                if(t<cellEnd)
                {
                    if((ray.source.x&GI_STATIC)!=0 && imageLoad(giStaticOwner,cell).r!=GI_INVALID_CELL && !GISkipSource(ray,cell,GI_STATIC,t)) objectClass=GI_STATIC;
                    // Static wins a co-located tie; the other class never inherits a self hint.
                    if(objectClass==0 && (ray.source.x&GI_DYNAMIC)!=0 && imageLoad(giDynamicOwner,cell).r!=GI_INVALID_CELL && !GISkipSource(ray,cell,GI_DYNAMIC,t)) objectClass=GI_DYNAMIC;
                }
                if(objectClass!=0)
                {
                    result=GIDecodeCell(ray,cell,objectClass,t);
                    break;
                }
                if(cellEnd>=end)
                {
                    result=EmptyGIHit(GI_MISS);
                    break;
                }
                for(int axis=0;axis<3;++axis)
                    if(next[axis]<=crossing) cell[axis]+=step[axis];
                t=max(t,crossing);
                if(any(lessThan(cell,ivec3(0))) || any(greaterThanEqual(cell,ivec3(side))))
                {
                    result=EmptyGIHit(GI_MISS);
                    break;
                }
            }
        }
    }
    return result;
}

uint TraceOccluded(GIQuery ray,uint queryIndex)
{
    return TraceClosest(ray,queryIndex).identity.x;
}
