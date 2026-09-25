#include "voxel_accumulate.glsl"
#include "voxel_coverage.glsl"
layout(location=0) flat in int depthAxis;
layout(location=1) flat in uint triangleRecord;
void main()
{
    if((triangles[triangleRecord].w&gridSelection.x)==0) return;
    Vertex va,vb,vc; vec3 a,b,c;
    TriangleVertices(triangleRecord,va,vb,vc,a,b,c);
    a=(a-gridMinSize.xyz)/gridMinSize.w;
    b=(b-gridMinSize.xyz)/gridMinSize.w;
    c=(c-gridMinSize.xyz)/gridMinSize.w;
    vec3 normal=cross(b-a,c-a);
    int z=depthAxis, x=(z+1)%3, y=(z+2)%3;
    ivec3 size=imageSize(voxelOwner);
    ivec3 cell=ivec3(0);
    cell[x]=int(gl_FragCoord.x); cell[y]=int(gl_FragCoord.y);
    ivec2 depths=VoxelDepthCells(a,normal,z,cell,size);
    for(cell[z]=depths.x;cell[z]<=depths.y;++cell[z])
    {
        if(VoxelTriangleBox(a,b,c,cell))
        {
            AccumulateVoxel(triangleRecord,cell);
        }
    }
}
