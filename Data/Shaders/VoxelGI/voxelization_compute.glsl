#include "voxel_accumulate.glsl"
#include "voxel_coverage.glsl"
layout(local_size_x=64) in;
layout(push_constant) uniform Constants { uint firstTriangle; uint triangleCount; } pc;

void main()
{
    uint record=pc.firstTriangle+gl_WorkGroupID.x;
    if(record>=pc.triangleCount) return;
    if((triangles[record].w&gridSelection.x)==0) return;
    Vertex va,vb,vc; vec3 a,b,c;
    TriangleVertices(record,va,vb,vc,a,b,c);
    a=(a-gridMinSize.xyz)/gridMinSize.w;
    b=(b-gridMinSize.xyz)/gridMinSize.w;
    c=(c-gridMinSize.xyz)/gridMinSize.w;
    vec3 normal=cross(b-a,c-a);
    int z=VoxelProjectionAxis(normal);
    if(abs(normal[z])<=1e-12) return;
    int x=(z+1)%3, y=(z+2)%3;
    ivec3 size=imageSize(voxelOwner);
    ivec3 lo=VoxelLowerCell(min(a,min(b,c)),size);
    ivec3 hi=VoxelUpperCell(max(a,max(b,c)),size);
    if(any(greaterThan(lo,hi))) return;
    int width=hi[x]-lo[x]+1;
    int count=width*(hi[y]-lo[y]+1);
    for(int index=int(gl_LocalInvocationIndex);index<count;index+=64)
    {
        ivec3 cell=ivec3(0);
        cell[x]=lo[x]+index%width; cell[y]=lo[y]+index/width;
        ivec2 depths=VoxelDepthCells(a,normal,z,cell,size);
        for(cell[z]=depths.x;cell[z]<=depths.y;++cell[z])
        {
            if(VoxelTriangleBox(a,b,c,cell))
            {
                AccumulateVoxel(record,cell);
            }
        }
    }
}
