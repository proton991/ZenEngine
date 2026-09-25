#ifndef ZEN_VOXEL_COVERAGE_GLSL
#define ZEN_VOXEL_COVERAGE_GLSL
// Grid-space boundary coverage. Closed cell boxes include face/edge/corner touches.
int VoxelProjectionAxis(vec3 normal)
{
    vec3 absoluteNormal=abs(normal);
    int axis=absoluteNormal.x>absoluteNormal.y ? 0 : 1;
    return absoluteNormal[axis]>absoluteNormal.z ? axis : 2;
}
ivec3 VoxelLowerCell(vec3 minimum, ivec3 size)
{
    return ivec3(clamp(ceil(minimum)-1.0,vec3(0),vec3(size)));
}
ivec3 VoxelUpperCell(vec3 maximum, ivec3 size)
{
    return ivec3(clamp(floor(maximum),vec3(-1),vec3(size-1)));
}
float VoxelCoverageDot(vec3 a, vec3 b)
{
    // Preserve operator order across fragment/compute compilation. A differently
    // contracted dot can change owner election at a shared-edge cell corner.
    precise float result=(a.x*b.x+a.y*b.y)+a.z*b.z;
    return result;
}
bool VoxelAxisOverlap(vec3 axis, vec3 a, vec3 b, vec3 c)
{
    float radius=0.5*VoxelCoverageDot(abs(axis),vec3(1));
    vec3 projected=vec3(VoxelCoverageDot(a,axis),VoxelCoverageDot(b,axis),VoxelCoverageDot(c,axis));
    return min(projected.x,min(projected.y,projected.z))<=radius &&
           max(projected.x,max(projected.y,projected.z))>=-radius;
}
bool VoxelTriangleBox(vec3 a,vec3 b,vec3 c,ivec3 cell)
{
    vec3 center=vec3(cell)+0.5;
    a-=center; b-=center; c-=center;
    vec3 edges[3]=vec3[3](b-a,c-b,a-c);
    bool overlap=VoxelAxisOverlap(cross(b-a,c-a),a,b,c);
    for(int i=0;i<3 && overlap;++i)
    {
        vec3 axis=vec3(0); axis[i]=1;
        overlap=VoxelAxisOverlap(axis,a,b,c);
        for(int j=0;j<3 && overlap;++j) overlap=VoxelAxisOverlap(cross(axis,edges[j]),a,b,c);
    }
    return overlap;
}
ivec2 VoxelDepthCells(vec3 a,vec3 normal,int z,ivec3 cell,ivec3 size)
{
    int x=(z+1)%3, y=(z+2)%3;
    // Dominant-axis slopes have magnitude <=1. Across one projected cell the
    // depth radius is <=1; closed boundary touches can require FOUR depth cells.
    float depth=(dot(normal,a)-normal[x]*(float(cell[x])+0.5)-
                 normal[y]*(float(cell[y])+0.5))/normal[z];
    float radius=0.5*(abs(normal[x])+abs(normal[y]))/abs(normal[z]);
    // Guard the rounded endpoints: an exact touch at depth 5 can evaluate as
    // 5.0000005 and otherwise drop cell 4 before SAT gets to test it. The extra
    // candidates are still rejected by the original, unexpanded triangle-box test.
    int lo=int(clamp(floor(depth-radius)-1.0,0.0,float(size[z])));
    int hi=int(clamp(ceil(depth+radius),-1.0,float(size[z]-1)));
    return ivec2(lo,hi);
}
#endif
