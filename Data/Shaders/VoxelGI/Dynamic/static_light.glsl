#include "frame_common.glsl"
#include "../../Common/scene_lighting.glsl"
#include "lighting_common.glsl"
layout(local_size_x=GI_QUERY_GROUP_SIZE) in;
layout(set=3,binding=1,std430) readonly buffer StaticMap { uint gridToList[]; };
layout(set=3,binding=2,std430) buffer StaticLightMask { uint lightMask[]; };
layout(set=3,binding=3,std430) buffer GIStatus { uvec4 giStatus; };
layout(set=3,binding=15,std430) buffer GILightMaskStatus { uvec4 lightMaskStatus; };
layout(set=3,binding=16,std430) writeonly buffer SenderPositions { vec4 senderPositions[]; };
void main()
{
    uint cell=StaticItem();
    uint n=staticGI.volume.x;
    if(cell<n*n*n)
    {
        vec4 senderPosition=vec4(0);
        uint updates=giLighting.enabled.w;
        uint mask=updates==0xffffffffu ? 0u : lightMask[cell]&~updates;
        uint receiver=gridToList[cell];
        uint count=batch.reserved==GI_DYNAMIC ? giStatus.w : giStatus.x;
        if(giLighting.enabled.x!=0u && receiver<count && (batch.reserved!=GI_DYNAMIC || giWork.state.w!=0u))
        {
#ifdef GI_MESH_LIGHT_VISIBILITY
            uint owner=batch.reserved==GI_DYNAMIC ? imageLoad(giDynamicOwner,StaticCell(cell)).r :
                imageLoad(giStaticOwner,StaticCell(cell)).r;
            bool validSurface=owner<triangles.length();
            Vertex a,b,c; vec3 pa=vec3(0),pb=vec3(0),pc=vec3(0),surfaceNormal=vec3(0);
            if(validSurface) {
                TriangleVertices(owner,a,b,c,pa,pb,pc);
                surfaceNormal=cross(pb-pa,pc-pa);
                validSurface=dot(surfaceNormal,surfaceNormal)>1e-20;
                if(validSurface) surfaceNormal=normalize(surfaceNormal);
                vec3 shadingNormal=mat3(nodesData[triangles[owner].y].normalMatrix)*a.normal.xyz;
                if(dot(surfaceNormal,shadingNormal)<0) surfaceNormal=-surfaceNormal;
                if(validSurface) {
                    vec3 weights=TriangleBarycentrics(pa,pb,pc,StaticCenter(cell));
                    senderPosition=vec4(pa*weights.x+pb*weights.y+pc*weights.z,1);
                }
            }
#endif
            for(uint light=0u;light<staticGI.volume.w;++light)
            {
                if((updates&(1u<<light))==0u) continue;
                uint visible=0u;
                bool unknown=false;
                // Cache geometric visibility even while a light is black/off.
                // Gathering still evaluates its current color and intensity.
                SceneLight visibilityLight=sceneUbo.lights[light];
                visibilityLight.colorIntensity=vec4(1);
                #ifdef GI_MESH_LIGHT_VISIBILITY
                const uint samples=1u;
#else
                const uint samples=8u;
#endif
                for(uint sampleID=0u;sampleID<samples;++sampleID)
                {
                    vec3 signs=vec3((sampleID&1u)!=0u ? 1 : -1,(sampleID&2u)!=0u ? 1 : -1,
                                    (sampleID&4u)!=0u ? 1 : -1);
                    vec3 origin=StaticCenter(cell)+signs*(0.475*staticGI.minimumCellSize.w);
#ifdef GI_MESH_LIGHT_VISIBILITY
                    // Match the representative surface point used for sender
                    // lighting. A cell-corner majority can hide that point even
                    // when the light reaches it through the actual geometry.
                    origin=senderPosition.xyz;
#endif
                    vec3 direction; float distanceToLight;
                    vec3 incident=EvaluateLight(visibilityLight,origin,direction,distanceToLight);
                    if(dot(incident,incident)>0 && dot(direction,direction)>0.5)
                    {
                        uint status=GI_MISS;
                        if(staticGI.lighting.y!=0 && sceneUbo.lights[light].coneShadow.z!=0)
                        {
#ifdef GI_MESH_LIGHT_VISIBILITY
                            status=validSurface ? (SceneLightVisibility(int(light),origin,surfaceNormal)>0.5 ? GI_MISS : GI_HIT) : GI_UNKNOWN;
#else
                            float end=min(distanceToLight,staticGI.lighting.z);
                            end=max(staticGI.lighting.w,end-staticGI.lighting.w);
                            GIQuery ray=GIQuery(vec4(origin,staticGI.lighting.w),vec4(direction,end),
                                                uvec4(GI_ALL,batch.reserved,cell,0));
                            uint reference=GIReferenceIndex(GI_FACE_COUNT*staticGI.volume.y*staticGI.sampling.x,
                                2u*light+(batch.reserved==GI_DYNAMIC ? 1u : 0u),8u*n*n*n,8u*receiver+sampleID);
                            status=TraceOccluded(ray,reference);
#endif
                        }
                        visible+=status==GI_MISS ? 1u : 0u;
                        unknown=unknown || status==GI_UNKNOWN;
                    }
                }
                mask|=visible>samples/2u ? (1u<<light) : 0u;
                if(unknown) {
                    atomicOr(giStatus.z,GI_STATIC_UNKNOWN);
                    atomicOr(lightMaskStatus.x,1u<<light);
                }
            }
        }
        lightMask[cell]=mask;
        senderPositions[cell]=senderPosition;
    }
}
