#version 450
#extension GL_GOOGLE_include_directive : require
#include "../Common/bindless_heap.glsl"
#include "voxel_coverage.glsl"
layout(triangles) in;
layout(triangle_strip,max_vertices=4) out;
layout(location=0) flat out int depthAxis;
layout(location=1) flat out uint triangleRecord;
layout(std140,set=1,binding=0) uniform uVoxelGrid { vec4 gridMinSize; uvec4 gridSelection; };
layout(push_constant) uniform Constants { uint nodeIndex; uint materialIndex; uint firstTriangle; uint volumeDimension; } pc;
void main()
{
    vec3 a=(gl_in[0].gl_Position.xyz-gridMinSize.xyz)/gridMinSize.w;
    vec3 b=(gl_in[1].gl_Position.xyz-gridMinSize.xyz)/gridMinSize.w;
    vec3 c=(gl_in[2].gl_Position.xyz-gridMinSize.xyz)/gridMinSize.w;
    vec3 normal=cross(b-a,c-a);
    int z=VoxelProjectionAxis(normal);
    if(abs(normal[z])<=1e-12) return;
    ivec3 size=ivec3(pc.volumeDimension);
    ivec3 lo=VoxelLowerCell(min(a,min(b,c)),size);
    ivec3 hi=VoxelUpperCell(max(a,max(b,c)),size);
    if(any(greaterThan(lo,hi))) return;
    int x=(z+1)%3, y=(z+2)%3;
    // Rasterize the bounded projected rectangle; the fragment pass rejects every
    // cell not intersecting the original triangle. No hardware extension is needed.
    for(int i=0;i<4;++i)
    {
        vec2 corner=vec2((i&1)==0 ? lo[x] : hi[x]+1, (i&2)==0 ? lo[y] : hi[y]+1);
        gl_Position=vec4(corner/float(pc.volumeDimension)*2.0-1.0,0.5,1);
        depthAxis=z;
        triangleRecord=pc.firstTriangle+uint(gl_PrimitiveIDIn);
        EmitVertex();
    }
    EndPrimitive();
}
