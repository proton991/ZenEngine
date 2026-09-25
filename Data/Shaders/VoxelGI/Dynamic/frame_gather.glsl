#include "frame_common.glsl"
#include "../../Common/scene_lighting.glsl"
#include "lighting_common.glsl"
#ifdef GI_ENVIRONMENT
#include "environment_common.glsl"
layout(set=3,binding=11,rgba16f) readonly uniform image3D staticEnvironment;
layout(set=3,binding=12,rgba16f) readonly uniform image3D dynamicEnvironment;
#endif
layout(local_size_x=GI_QUERY_GROUP_SIZE) in;
layout(set=3,binding=1,std430) readonly buffer GIReceivers { uint receivers[]; };
layout(set=3,binding=2,std430) readonly buffer StaticLightMask { uint staticLightMask[]; };
#include "hit_cache.glsl"
layout(set=3,binding=4,std430) buffer GIStatus { uvec4 giStatus; };
layout(set=3,binding=5,rgba16f) writeonly uniform image3D rawIrradiance;
layout(set=3,binding=7,std430) readonly buffer GIWorkCounts { uvec4 workCounts; };
layout(set=3,binding=8,std430) readonly buffer StaticMap { uint gridToList[]; };
layout(set=3,binding=9,std430) readonly buffer DynamicLightMask { uint dynamicLightMask[]; };
layout(set=3,binding=10,r32ui) readonly uniform uimage3D dynamicOwner;
layout(set=3,binding=16,std430) readonly buffer StaticSenderPositions { vec4 staticSenderPositions[]; };
layout(set=3,binding=17,std430) readonly buffer DynamicSenderPositions { vec4 dynamicSenderPositions[]; };
GIHit ReceiverHit(uint cell,uint compact,uint ray) {
    GIHit hit=EmptyGIHit(GI_UNKNOWN);
    // Reference indices preserve M3 cached/light ranges; transient queries follow them.
    uint n=staticGI.volume.x;
    uint lightEnd=GIReferenceIndex(GI_FACE_COUNT*staticGI.volume.y*staticGI.sampling.x,
        2u*uint(MAX_SCENE_LIGHTS),8u*n*n*n,0u);
    uint reference=GIReferenceIndex(lightEnd,(batch.reserved==GI_DYNAMIC ? GI_FACE_COUNT : 0u)+batch.face,
        n*n*n*staticGI.sampling.x,cell*staticGI.sampling.x+ray);
    if(batch.reserved==GI_STATIC) {
        if(compact<min(giStatus.x,staticGI.volume.y)) hit=LoadCachedHit(compact*staticGI.sampling.x+ray);
        if(giStatus.w!=0u && giWork.state.w!=0u) {
            GIQuery query=StaticRay(cell,batch.face,ray);
            query.source.x=GI_DYNAMIC;
            GIHit moving=TraceClosest(query,reference);
            // Unknown is never a miss; static wins equal-distance class ties.
            if(hit.identity.x==GI_UNKNOWN || moving.identity.x==GI_UNKNOWN) hit=EmptyGIHit(GI_UNKNOWN);
            else if(moving.identity.x==GI_HIT && (hit.identity.x!=GI_HIT || moving.positionDistance.w<hit.positionDistance.w)) hit=moving;
        }
    } else {
        GIQuery query=StaticRay(cell,batch.face,ray);
        query.source=uvec4(GI_ALL,imageLoad(dynamicOwner,StaticCell(cell)).r!=GI_INVALID_CELL ? GI_DYNAMIC : 0u,cell,0);
        hit=TraceClosest(query,reference);
    }
    return hit;
}
// Each workgroup traces one receiver in parallel. Lane zero keeps the original
// ray-order sum, including its final multiply/add, for deterministic energy.
shared vec4 rayContributions[GI_FACE_RAYS];
vec4 ReceiverContribution(uint cell,uint compact,uint ray) {
    GIHit hit=ReceiverHit(cell,compact,ray);
    bool valid=hit.identity.x!=GI_UNKNOWN;
    bool sky=false;
    vec3 outgoing=vec3(0);
    uint n=staticGI.volume.x;
    if(hit.identity.x==GI_HIT && (hit.identity.w&GI_SURFACE_VALID)!=0u && hit.identity.z<n*n*n) {
        uint lit=hit.identity.y==GI_STATIC ? staticLightMask[hit.identity.z] :
            (hit.identity.y==GI_DYNAMIC ? dynamicLightMask[hit.identity.z] : 0u);
        vec4 sender=hit.identity.y==GI_STATIC ? staticSenderPositions[hit.identity.z] : dynamicSenderPositions[hit.identity.z];
        vec3 lightingPosition=sender.xyz;
        if(lit!=0u && sender.w<=0) {
            lightingPosition=hit.positionDistance.xyz;
            if(giWork.cache.x!=0u && batch.reserved==GI_STATIC && hit.identity.y==GI_STATIC) {
                GIQuery query=StaticRay(cell,batch.face,ray);
                lightingPosition=query.originMin.xyz+query.directionMax.xyz*hit.positionDistance.w;
            }
        }
        vec3 incoming=vec3(0);
        for(uint light=0u;light<staticGI.volume.w;++light) {
            if((lit&(1u<<light))!=0u) {
                vec3 direction; float distanceToLight;
                vec3 incident=EvaluateLight(sceneUbo.lights[light],lightingPosition,direction,distanceToLight);
                incoming+=incident*max(dot(hit.normal.xyz,direction),0.0);
            }
        }
#ifdef GI_ENVIRONMENT
        vec4 environment=hit.identity.y==GI_STATIC ? imageLoad(staticEnvironment,StaticCell(hit.identity.z)) :
            imageLoad(dynamicEnvironment,StaticCell(hit.identity.z));
        valid=valid && environment.a>0;
        incoming+=environment.rgb;
#endif
        outgoing=hit.diffuseReflectance.rgb*incoming;
        if(giLighting.enabled.z!=0u) outgoing+=3.141592653589793*hit.emission.rgb;
    }
#ifdef GI_ENVIRONMENT
    else if(hit.identity.x==GI_MISS) {
        sky=true;
        outgoing=GIEnvironmentRadiance(StaticDirection(batch.face,ray));
    }
#endif
    return vec4(outgoing,valid ? (sky ? 2 : 1) : 0);
}
void main() {
    uint group=gl_WorkGroupID.x+gl_NumWorkGroups.x*(gl_WorkGroupID.y+gl_NumWorkGroups.y*gl_WorkGroupID.z);
    uint index=batch.firstItem+group;
    uint count=batch.reserved==GI_DYNAMIC ? workCounts.y : workCounts.x;
    bool enabled=group<batch.itemCount && index<count &&
        giStatus.x<=staticGI.volume.y && giStatus.x<=giWork.state.x;
    uint cell=enabled ? receivers[index] : 0u;
    if(enabled) {
        for(uint ray=gl_LocalInvocationIndex;ray<staticGI.sampling.x;ray+=GI_QUERY_GROUP_SIZE)
            rayContributions[ray]=ReceiverContribution(cell,gridToList[cell],ray);
    }
    barrier();
    if(enabled && gl_LocalInvocationIndex==0u) {
        vec3 sum=vec3(0); bool valid=true;
        for(uint ray=0u;ray<staticGI.sampling.x;++ray) {
            vec4 contribution=rayContributions[ray];
            valid=valid && contribution.w>0;
            if(contribution.w==2) sum+=3.141592653589793*contribution.rgb;
            else sum+=staticGI.lighting.x*contribution.rgb;
        }
        vec4 irradiance=vec4(sum/float(staticGI.sampling.x),valid ? 1 : 0);
        if(!valid || any(isnan(irradiance)) || any(isinf(irradiance)) || any(greaterThan(irradiance.rgb,vec3(65504)))) {
            atomicOr(giStatus.z,GI_STATIC_UNKNOWN); irradiance=vec4(0);
        }
        imageStore(rawIrradiance,StaticCell(cell),irradiance);
    }
}
