#include "Graphics/Shared/LightingCapture.h"
layout(local_size_x=ZEN_LIGHTING_CAPTURE_GROUP_SIZE) in;
#ifdef SURFACE_CAPTURE
#define CAPTURE_COMPONENTS ZEN_SURFACE_CAPTURE_COMPONENTS
layout(set=4,binding=3,std430) writeonly buffer SurfaceCapture { vec4 components[]; };
#else
#define CAPTURE_COMPONENTS ZEN_LIGHTING_CAPTURE_COMPONENTS
layout(set=4,binding=2,std430) writeonly buffer LightingCapture { vec4 components[]; };
#endif
layout(push_constant) uniform Constants { uint firstItem; uint itemCount; } pc;
void main()
{
    uint group=gl_WorkGroupID.x+gl_NumWorkGroups.x*(gl_WorkGroupID.y+gl_NumWorkGroups.y*gl_WorkGroupID.z);
    uint local=group*ZEN_LIGHTING_CAPTURE_GROUP_SIZE+gl_LocalInvocationIndex;
    if(local<pc.itemCount)
    {
        uint pixel=pc.firstItem+local;
        for(uint component=0;component<CAPTURE_COMPONENTS;++component)
            components[pixel*CAPTURE_COMPONENTS+component]=vec4(0);
    }
}
