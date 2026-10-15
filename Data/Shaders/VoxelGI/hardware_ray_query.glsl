#ifndef ZEN_HARDWARE_RAY_QUERY
#define ZEN_HARDWARE_RAY_QUERY
#include "hybrid_ray_counts.glsl"
#include "scene_ray_geometry.glsl"
layout(set=6,binding=0) uniform accelerationStructureEXT sceneAccelerationStructure;
layout(std430,set=6,binding=1) readonly buffer RayGeometryMetadata { uvec4 rayGeometries[]; };
layout(std430,set=6,binding=2) readonly buffer RayInstanceMetadata { uvec4 rayInstances[]; };
layout(std140,set=6,binding=5) uniform uRayQueryData { vec4 minimum; vec4 maximum; uvec4 generation; } rayScene;

uvec4 RayTriangle(uint instance,uint geometry,uint primitive)
{
    uvec4 object=rayInstances[instance];
    uvec4 mesh=rayGeometries[object.y+geometry];
    return uvec4(mesh.x+3u*primitive,object.x,mesh.y,instance);
}

// The same query supports visibility and closest-hit consumers. Metadata is
// versioned with the snapshot, not decoded against a later scene generation.
struct HardwareRayHit
{
    HybridRayResult ray;
    uint instance;
    uint primitive;
    uint material;
    vec2 barycentrics;
    uvec2 generation;
    // Raster facing of the hit (node orientation included). Every face occludes;
    // hit-radiance consumers use this to reject back-facing lookups.
    bool frontFacing;
};
HardwareRayHit HardwareQuery(vec3 origin,vec3 direction,float maximumDistance,bool closest)
{
    HardwareRayHit result;
    result.ray=HybridRayResult(false,origin,vec3(0),ivec3(-1));
    result.instance=result.primitive=result.material=0xffffffffu;
    result.barycentrics=vec2(0);
    result.generation=rayScene.generation.xy;
    result.frontFacing=false;
    float enter=0.0,leave=maximumDistance;
    bool intersects=true;
    for(int axis=0;axis<3;++axis)
    {
        if(abs(direction[axis])<1e-12)
            intersects=intersects && origin[axis]>=rayScene.minimum[axis] && origin[axis]<=rayScene.maximum[axis];
        else
        {
            float a=(rayScene.minimum[axis]-origin[axis])/direction[axis];
            float b=(rayScene.maximum[axis]-origin[axis])/direction[axis];
            enter=max(enter,min(a,b)); leave=min(leave,max(a,b));
        }
    }
    if(intersects && leave>=enter && leave>0.0)
    {
#ifdef HYBRID_COUNT_RAYS
        ++issuedHardwareRays[issuedRayFeature];
#endif
        rayQueryEXT query;
        uint flags=closest ? 0u : gl_RayFlagsTerminateOnFirstHitEXT;
        // Pad the scene bound, but never trace beyond a finite caller-supplied endpoint (a light).
        rayQueryInitializeEXT(query,sceneAccelerationStructure,flags,0xffu,origin,0.0,direction,min(leave+1e-5,maximumDistance));
        while(rayQueryProceedEXT(query))
        {
            if(rayQueryGetIntersectionTypeEXT(query,false)==gl_RayQueryCandidateIntersectionTriangleEXT)
            {
                uvec4 triangle=RayTriangle(rayQueryGetIntersectionInstanceCustomIndexEXT(query,false),
                    rayQueryGetIntersectionGeometryIndexEXT(query,false),rayQueryGetIntersectionPrimitiveIndexEXT(query,false));
                if(RayCandidateAccepted(triangle,rayQueryGetIntersectionBarycentricsEXT(query,false)))
                    rayQueryConfirmIntersectionEXT(query);
            }
        }
        if(rayQueryGetIntersectionTypeEXT(query,true)==gl_RayQueryCommittedIntersectionTriangleEXT)
        {
            result.instance=rayQueryGetIntersectionInstanceCustomIndexEXT(query,true);
            result.primitive=rayQueryGetIntersectionPrimitiveIndexEXT(query,true);
            uvec4 triangle=RayTriangle(result.instance,rayQueryGetIntersectionGeometryIndexEXT(query,true),result.primitive);
            result.material=triangle.z;
            result.barycentrics=rayQueryGetIntersectionBarycentricsEXT(query,true);
            result.ray.hit=true;
            result.ray.position=RaySurfacePosition(triangle,result.barycentrics);
            result.ray.normal=RayGeometricNormal(triangle);
            result.frontFacing=dot(result.ray.normal,direction)<0.0;
            if(!result.frontFacing) result.ray.normal=-result.ray.normal;
            result.ray.cell=ivec3(floor((result.ray.position-gi.gridMinVoxelSize.xyz)/gi.gridMinVoxelSize.w));
        }
    }
    return result;
}
HybridRayResult HardwareTraceRay(vec3 origin,vec3 direction,float maximumDistance,bool closest)
{
    return HardwareQuery(origin,direction,maximumDistance,closest).ray;
}
#endif
