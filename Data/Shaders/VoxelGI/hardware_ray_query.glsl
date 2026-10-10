#ifndef ZEN_HARDWARE_RAY_QUERY
#define ZEN_HARDWARE_RAY_QUERY
#define VOXEL_GEOMETRY_SET 5
#include "../Common/scene_geometry.glsl"
#include "../Common/material.glsl"
layout(set=6,binding=0) uniform accelerationStructureEXT sceneAccelerationStructure;
layout(std430,set=6,binding=1) readonly buffer RayGeometryMetadata { uvec4 rayGeometries[]; };
layout(std430,set=6,binding=2) readonly buffer RayInstanceMetadata { uvec4 rayInstances[]; };
layout(std140,set=6,binding=3) readonly buffer MaterialBuffer { Material materialData[]; };
layout(std430,set=6,binding=4) readonly buffer UVBuffer { vec4 uvValues[]; };
layout(std140,set=6,binding=5) uniform uRayQueryData { vec4 minimum; vec4 maximum; uvec4 generation; } rayScene;

// A few ULPs cover surface transform arithmetic; the receiver additionally
// supplies its measured depth-quantization error. No whole instance is ignored.
vec3 HybridTraceOrigin(vec3 position,vec3 normal,float error)
{
    vec3 shifted=position+normal*(error+1e-6);
    ivec3 bits=floatBitsToInt(shifted);
    ivec3 offset=ivec3(normal*32.0);
    return intBitsToFloat(bits+mix(offset,-offset,lessThan(shifted,vec3(0))));
}

uvec4 RayTriangle(uint instance,uint geometry,uint primitive)
{
    uvec4 object=rayInstances[instance];
    uvec4 mesh=rayGeometries[object.y+geometry];
    return uvec4(mesh.x+3u*primitive,object.x,mesh.y,instance);
}

vec3 RayGeometricNormal(uvec4 triangle)
{
    vec3 a=vertices[indices[triangle.x]].position.xyz;
    vec3 b=vertices[indices[triangle.x+1u]].position.xyz;
    vec3 c=vertices[indices[triangle.x+2u]].position.xyz;
    return normalize(mat3(nodesData[triangle.y].normalMatrix)*cross(b-a,c-a));
}

// Only alpha-mask geometry is non-opaque in the BLAS. Both faces of single-sided
// surfaces block: raster back-face culling is view-dependent, and ignoring back
// faces would admit sky into closed single-sided shells.
bool RayCandidateAccepted(uvec4 triangle,vec2 barycentric)
{
    Material material=materialData[triangle.z];
    bool accepted=true;
    if(material.surfaceProperties.y==1.0)
    {
        vec3 weights=vec3(1.0-barycentric.x-barycentric.y,barycentric);
        bool specularGlossiness=material.surfaceProperties.w>0.5;
        uint set=uint(max(specularGlossiness ? int(material.featureTextures[2].properties.y) : material.bcTexSet,0));
        uint stride=uint(uvValues[0].x);
        vec2 uv=vec2(0); float alpha=0.0;
        for(uint corner=0u;corner<3u;++corner)
        {
            uint vertex=indices[triangle.x+corner];
            if(set<stride) uv+=weights[corner]*uvValues[1u+vertex*stride+set].xy;
            alpha+=weights[corner]*vertices[vertex].color.a;
        }
        alpha*=specularGlossiness ? material.diffuseFactor.a*MaterialFeatureTexture(material,2,uv,uv).a :
            material.baseColorFactor.a*MaterialSlotTexture(material,0,material.bcTexIndex,
                MaterialTransformedUV(material,0,0,uv,uv)).a;
        accepted=MaterialVisible(material,alpha);
    }
    // Blend/transmission are opaque blockers under the query lighting contract.
    return accepted;
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
        rayQueryEXT query;
        uint flags=closest ? 0u : gl_RayFlagsTerminateOnFirstHitEXT;
        rayQueryInitializeEXT(query,sceneAccelerationStructure,flags,0xffu,origin,0.0,direction,leave+1e-5);
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
            result.ray.position=origin+direction*rayQueryGetIntersectionTEXT(query,true);
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
