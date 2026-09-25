#include "frame_common.glsl"
#include "../../Common/scene_lighting.glsl"
#include "environment_common.glsl"
layout(local_size_x=GI_QUERY_GROUP_SIZE) in;
layout(set=3,binding=1,std430) readonly buffer SenderMap { uint gridToList[]; };
layout(set=3,binding=2,rgba8) readonly uniform image3D senderNormal;
layout(set=3,binding=3,rgba16f) writeonly uniform image3D senderEnvironment;
layout(set=3,binding=4,std430) buffer GIStatus { uvec4 giStatus; };
void main() {
    uint cell=StaticItem(),n=staticGI.volume.x;
    if(cell<n*n*n) {
        vec4 irradiance=vec4(0);
        uint count=batch.reserved==GI_STATIC ? giStatus.x : giStatus.w;
        if(gridToList[cell]<count && (batch.reserved==GI_STATIC || giWork.state.w!=0u)) {
            vec3 normal=imageLoad(senderNormal,StaticCell(cell)).xyz*2.0-1.0;
            bool valid=dot(normal,normal)>1e-12;
            if(valid) {
                normal=normalize(normal);
                vec3 tangent=normalize(cross(abs(normal.z)<0.99 ? vec3(0,0,1) : vec3(0,1,0),normal));
                vec3 origin=StaticCenter(cell)+normal*(0.475*staticGI.minimumCellSize.w);
                vec3 sum=vec3(0);
                uint lightEnd=GIReferenceIndex(GI_FACE_COUNT*staticGI.volume.y*staticGI.sampling.x,
                    2u*uint(MAX_SCENE_LIGHTS),8u*n*n*n,0u);
                uint base=GIReferenceIndex(lightEnd,2u*GI_FACE_COUNT,n*n*n*staticGI.sampling.x,0u);
                for(uint ray=0u;ray<staticGI.sampling.x;++ray) {
                    float u=(float(ray)+0.5)/float(staticGI.sampling.x);
                    float phi=6.283185307179586*fract(float(ray)*0.6180339887498949+0.5);
                    vec3 direction=normalize(sqrt(u)*cos(phi)*tangent+sqrt(u)*sin(phi)*cross(normal,tangent)+sqrt(1.0-u)*normal);
                    GIQuery query=GIQuery(vec4(origin,staticGI.lighting.w),vec4(direction,staticGI.lighting.z),uvec4(GI_ALL,batch.reserved,cell,0));
                    uint reference=GIReferenceIndex(base,batch.reserved==GI_DYNAMIC ? 1u : 0u,
                        n*n*n*staticGI.sampling.x,cell*staticGI.sampling.x+ray);
                    uint status=TraceOccluded(query,reference);
                    valid=valid && status!=GI_UNKNOWN;
                    if(status==GI_MISS) sum+=GIEnvironmentRadiance(direction);
                }
                irradiance=vec4(sum*(3.141592653589793/float(staticGI.sampling.x)),valid ? 1 : 0);
            }
            if(!valid || any(isnan(irradiance)) || any(isinf(irradiance)) || any(greaterThan(irradiance.rgb,vec3(65504)))) {
                atomicOr(giStatus.z,GI_STATIC_UNKNOWN); irradiance=vec4(0);
            }
        }
        imageStore(senderEnvironment,StaticCell(cell),irradiance);
    }
}
