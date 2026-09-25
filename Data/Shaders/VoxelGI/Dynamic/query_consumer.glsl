layout(local_size_x=GI_QUERY_GROUP_SIZE) in;
layout(set=2,binding=0,std430) readonly buffer GIRequests { GIQuery requests[]; };
layout(set=2,binding=1,std430) writeonly buffer GIResults { GIQueryResult results[]; };
layout(push_constant) uniform QueryBatch { uint firstItem; uint itemCount; } batch;
void main()
{
    uint group=gl_WorkGroupID.x+gl_NumWorkGroups.x*(gl_WorkGroupID.y+gl_NumWorkGroups.y*gl_WorkGroupID.z);
    uint local=group*GI_QUERY_GROUP_SIZE+gl_LocalInvocationIndex;
    if(local<batch.itemCount)
    {
        uint index=batch.firstItem+local;
        results[index].closest=TraceClosest(requests[index],index);
#ifdef GI_COUNT_TRAVERSAL
        uint closestVisits=giTraversalVisits;
#endif
        results[index].occlusion=uvec4(TraceOccluded(requests[index],index),0,0,0);
#ifdef GI_COUNT_TRAVERSAL
        if(giGrid.dimensions.w!=0u)
            results[index].occlusion.yz=uvec2(closestVisits,giTraversalVisits);
#endif
    }
}
